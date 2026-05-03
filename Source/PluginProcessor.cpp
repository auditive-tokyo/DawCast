#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <CoreFoundation/CoreFoundation.h>

DawCastProcessor::DawCastProcessor()
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
  outputPath = juce::File::getSpecialLocation(juce::File::userMoviesDirectory)
                   .getChildFile("DawCast")
                   .getFullPathName();

  // ステータスコールバックを設定
  ipcClient.setStatusCallback([this](const juce::String &json) {
    juce::Logger::writeToLog("DawCastPlugin: status from recorder: " + json);

    if (json.contains("\"ready\"")) {
      // Recorder が接続完了→ REC ボタンを有効化
      recorderReady.store(true);
    } else if (json.contains("\"recording\"")) {
      // Recorder が実際に録画を開始した瞬間にタイマーをスタート
      recordingStartMs.store(juce::Time::currentTimeMillis());
    } else if (json.contains("\"cancelled\"")) {
      // Region 選択キャンセル: 録画フラグとタイマーをリセット
      recordingActive.store(false);
      recordingStartMs.store(0);
    } else if (json.contains("\"error\"")) {
      // Recorder 側エラー（TCC 権限なし等）: 録画フラグとタイマーをリセット
      recordingActive.store(false);
      recordingStartMs.store(0);
    } else if (json.contains("\"done\"")) {
      // 録画完了
      recordingActive.store(false);
      recordingStartMs.store(0);
    }
  });

  // IPC 切断コールバック: Recorder が落ちた（TCC kill, クラッシュ等）→
  // 自動再起動 DAW 終了時は ~DawCastProcessor() が disconnect() を呼ぶため、
  // callBack を nullptr クリアしてから disconnect()
  // を呼ぶ（デストラクタ参照）。 callAsync ラムダで alive を確認し、破棄済み
  // this を参照しないようにする。
  ipcClient.setDisconnectCallback([this, aliveFlag = alive] {
    recorderReady.store(false);
    recordingActive.store(false);
    recordingStartMs.store(0);

    if (!relaunchScheduled.exchange(true)) {
      juce::MessageManager::callAsync([this, aliveFlag] {
        if (aliveFlag->load())
          startTimer(1000);
      });
    }
  });
}

DawCastProcessor::~DawCastProcessor() {
  // ① alive フラグを落として遅延ラムダ（callAfterDelay 等）を無効化
  alive->store(false);

  // ② 1000ms 再起動タイマーをキャンセル
  stopTimer();

  if (recordingActive.load())
    ipcClient.sendStop();

  // ③ disconnect() によって connectionLost() → 切断コールバックが発火するが、
  //    コールバックを nullptr にしておくことで Recorder の再起動を防ぐ。
  ipcClient.setDisconnectCallback(nullptr);

  // Recorder プロセスに quit を通知してから切断する。
  // Recorder は onConnectionLost でも systemRequestedQuit() するため二重安全。
  // open --background で起動しているため ChildProcess::kill()
  // では終了できない。
  ipcClient.sendQuit();
  // sendQuit() の TCP 送信が flush されるよう disconnect() 前に少し待つ。
  juce::Thread::sleep(50);
  ipcClient.disconnect();
  launcher.quit(); // running フラグをリセット
}

void DawCastProcessor::prepareToPlay(double sampleRate,
                                     int /*samplesPerBlock*/) {
  currentSampleRate = sampleRate;

  // 共有メモリを初期化（失敗してもローカルバッファにフォールバック）
  ringBuffer.initSharedMemory();

  // 録画アプリを起動（未起動の場合のみ）
  if (!launcher.isRunning()) {
    recorderReady.store(false); // 起動中はグレイアウト
    launcher.launch();
    // open --background は即時に戻るが Recorder の listen
    // 開始まで時間がかかる。 800ms 待ってから接続を試みる。失敗しても REC
    // 押下時に再試行される。
    juce::Timer::callAfterDelay(
        800, [this, aliveFlag = alive, port = launcher.currentPort()] {
          if (aliveFlag->load() && !ipcClient.isConnected())
            ipcClient.connect(port);
        });
  } else if (!ipcClient.isConnected()) {
    ipcClient.connect(launcher.currentPort());
  }
}

void DawCastProcessor::releaseResources() {}

void DawCastProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                    juce::MidiBuffer & /*midiMessages*/) {
  // 入力バッファをそのままスルーしつつ、録画中のみリングバッファに書き込む
  if (recordingActive.load(std::memory_order_relaxed))
    ringBuffer.write(buffer, buffer.getNumSamples());
}

juce::AudioProcessorEditor *DawCastProcessor::createEditor() {
  return new DawCastEditor(*this);
}

void DawCastProcessor::getStateInformation(juce::MemoryBlock &destData) {
  auto xml = std::make_unique<juce::XmlElement>("DawCastState");
  xml->setAttribute("outputPath", outputPath);
  xml->setAttribute("captureMode", captureMode);
  xml->setAttribute("useProRes", useProRes ? 1 : 0);
  copyXmlToBinary(*xml, destData);
}

void DawCastProcessor::setStateInformation(const void *data, int sizeInBytes) {
  if (auto xml = getXmlFromBinary(data, sizeInBytes)) {
    outputPath = xml->getStringAttribute("outputPath", outputPath);
    captureMode = xml->getStringAttribute("captureMode", captureMode);
    useProRes = xml->getIntAttribute("useProRes", useProRes ? 1 : 0) != 0;
  }
}

// ─── 録画制御 ─────────────────────────────────────────────────────────────

void DawCastProcessor::startRecording(const IPCClient::StartParams &params) {
  if (recordingActive.load())
    return;

  // 未接続なら再接続を試みる
  if (!ipcClient.isConnected()) {
    if (!ipcClient.connect(launcher.currentPort())) {
      juce::Logger::writeToLog("DawCastPlugin: cannot connect to recorder");
      return;
    }
  }

  IPCClient::StartParams p = params;
  p.sampleRate = currentSampleRate;
  p.outputPath = outputPath;
  p.useProRes = useProRes;

  ipcClient.sendStart(p);
  // タイマーは Recorder からの "recording" 応答で開始する。
  // Region モードはオーバーレイ選択完了後、それ以外は beginCapture() 完了後。
  recordingActive.store(true);

  // 録画開始時刻を共有メモリに書き込む（mach_absolute_time）。
  // Recorder 側が映像タイムスタンプ（SCK / CMSampleBuffer = 同一クロック）と
  // 同じ原点で音声 PTS を計算するために使う。長時間録画でも A/V
  // ズレが生じない。
  ringBuffer.setRecordingStart();
}

juce::String DawCastProcessor::getHostBundleID() noexcept {
  // プラグインをホストしているアプリ（DAW）の Bundle ID を CFBundle
  // から取得する。
  if (CFBundleRef main = CFBundleGetMainBundle()) {
    if (CFStringRef bid = CFBundleGetIdentifier(main)) {
      char buf[256] = {};
      if (CFStringGetCString(bid, buf, sizeof(buf), kCFStringEncodingUTF8))
        return juce::String::fromUTF8(buf);
    }
  }
  return {};
}

void DawCastProcessor::stopRecording() {
  if (!recordingActive.load())
    return;

  ipcClient.sendStop();
  recordingActive.store(false);
  recordingStartMs.store(0);
  ringBuffer.clearRecordingStart();
}

void DawCastProcessor::timerCallback() {
  // "disconnected" 受信から 1 秒後に呼ばれる単発タイマー。
  // Recorder を再起動し、IPC 再接続を試みる。
  stopTimer();
  relaunchScheduled.store(false);

  juce::Logger::writeToLog(
      "DawCastPlugin: relaunching recorder after disconnect");

  launcher.relaunch();

  // open --background は即時に戻るため、Recorder のソケット listen 開始を
  // 少し待ってから接続を試みる。失敗しても次回 REC 押下時に再試行される。
  // aliveFlag で破棄済み this へのアクセスを防ぐ。
  juce::Timer::callAfterDelay(
      800, [this, aliveFlag = alive, port = launcher.currentPort()] {
        if (aliveFlag->load() && !ipcClient.isConnected())
          ipcClient.connect(port);
      });
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new DawCastProcessor(); // NOSONAR - required by JUCE plugin API
}
