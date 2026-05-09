#include "RecorderApp.h"
#include "../SharedAudio.h"
#include "AudioReceiver.h"
#include "FFmpegMuxer.h"
#include "IPCServer.h"
#include "OutputManager.h"
#include "RecordingBorderWindow.h"
#include "RegionSelectorWindow.h"
#include "ScreenRecorder.h"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

// ─── 音声ポンプスレッド ────────────────────────────────────────────
// AudioReceiver をポーリングして FFmpegMuxer に流す専用スレッド。

struct RecorderApp::AudioPumpThread : public juce::Thread {
  AudioPumpThread(AudioReceiver &ar, FFmpegMuxer &fm,
                  juce::CriticalSection &lock, double recStartSecs, double sr)
      : juce::Thread("DawCast Audio Pump"), receiver(ar), muxer(fm),
        muxerLock(lock), recordingStartSecs(recStartSecs), sampleRate(sr) {}

  void run() override {
    juce::AudioBuffer<float> buf(DawCastShm::kNumChannels, 1024);
    int64_t samplesRead = 0; // 録画開始後に読み出した累積サンプル数

    while (!threadShouldExit()) {
      const int got = receiver.read(buf, 1024);
      if (got > 0) {
        // このチャンクの先頭サンプルが「録画開始から何秒後か」を計算し、
        // recordingStartSecs（= Plugin 側の mach_absolute_time を秒変換した値）
        // に加算する。映像 PTS の原点（同一クロック）と揃うため A/V ズレゼロ。
        const double chunkHostTimeSecs =
            recordingStartSecs + static_cast<double>(samplesRead) / sampleRate;
        samplesRead += got;

        juce::AudioBuffer<float> chunk(buf.getArrayOfWritePointers(),
                                       DawCastShm::kNumChannels, got);
        juce::ScopedLock sl(muxerLock);
        muxer.writeAudioSamples(chunk, chunkHostTimeSecs);
      } else {
        juce::Thread::sleep(5);
      }
    }
  }

  AudioReceiver &receiver;
  FFmpegMuxer &muxer;
  juce::CriticalSection &muxerLock;
  double recordingStartSecs; // mach_absolute_time を秒変換した録画開始時刻
  double sampleRate;
};

// ─── RecorderApp ──────────────────────────────────────────────────

RecorderApp::RecorderApp() = default;
RecorderApp::~RecorderApp() = default;

void RecorderApp::initialise(const juce::String &commandLine) {
  // 起動引数 --port <n> をパース。
  // Plugin が空きポートを取得して渡すため、
  // VST3 と AU を同時にロードしてもポートが衝突しない。
  int ipcPort = 9527; // フォールバック
  {
    auto tokens = juce::StringArray::fromTokens(commandLine, " ", "\"");
    const int idx = tokens.indexOf("--port");
    if (idx >= 0 && idx + 1 < tokens.size())
      ipcPort = tokens[idx + 1].getIntValue();
  }

  ipcServer = std::make_unique<IPCServer>();
  screenRecorder = std::make_unique<ScreenRecorder>();
  audioReceiver = std::make_unique<AudioReceiver>();
  ffmpegMuxer = std::make_unique<FFmpegMuxer>();
  outputManager = std::make_unique<OutputManager>();
  regionSelector = std::make_unique<RegionSelectorWindow>();
  recordingBorder = std::make_unique<RecordingBorderWindow>();

  // ── フレームコールバック: ScreenCaptureKit → FFmpegMuxer ──────
  // ScreenRecorder.mm 側で SCStreamFrameInfoStatus をチェックし、
  // Blank / Suspended / Started 等の無効フレームは既に除外済み。
  screenRecorder->setFrameCallback([this](CMSampleBufferRef sampleBuffer,
                                          double ts) {
    CVPixelBufferRef pixbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixbuf)
      return;

    CVPixelBufferLockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly);

    const auto *data =
        static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(pixbuf));
    const auto stride = static_cast<int>(CVPixelBufferGetBytesPerRow(pixbuf));
    const auto srcW = static_cast<int>(CVPixelBufferGetWidth(pixbuf));
    const auto srcH = static_cast<int>(CVPixelBufferGetHeight(pixbuf));

    {
      juce::ScopedLock sl(muxerLock);
      ffmpegMuxer->writeVideoFrame(data, stride, srcW, srcH, ts);
    }

    CVPixelBufferUnlockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly);
  });

  // ── IPC コマンドコールバック ──────────────────────────────────
  // IPC 受信スレッドからメッセージスレッドへデリゲート
  ipcServer->setCommandCallback([this](const juce::String &json) {
    juce::MessageManager::callAsync([this, json] { handleCommand(json); });
  });

  // Plugin 切断時 = DAW 終了 → Recorder も自己終了する。
  // 「"quit"コマンドを受ける前にソケットが閉じた」際のフォールバック。
  // callAsync でメッセージスレッドから呼ぶ。
  ipcServer->setDisconnectCallback([this] {
    juce::MessageManager::callAsync([this] { systemRequestedQuit(); });
  });

  ipcServer->start(ipcPort);

  DBG("DawCastRecorder: IPCServer listening on port " + juce::String(ipcPort));
}

void RecorderApp::shutdown() {
  if (recordingActive.load())
    stopCapture();

  if (ipcServer)
    ipcServer->stop();
}

void RecorderApp::systemRequestedQuit() { quit(); }

// ─── コマンドハンドラ（メッセージスレッド） ───────────────────────

void RecorderApp::handleCommand(const juce::String &jsonStr) {
  const juce::var parsed = juce::JSON::parse(jsonStr);
  if (!parsed.isObject())
    return;

  const juce::String cmd = parsed["cmd"].toString();

  if (cmd == "start" && !recordingActive.load())
    startCapture(parsed);
  else if (cmd == "stop" && recordingActive.load())
    stopCapture();
  else if (cmd == "quit")
    quit(); // プラグイン(DAW)が終了する際にプロセスを正常終了させる
}

void RecorderApp::startCapture(const juce::var &json) {
  if (const juce::String modeStr = json["captureMode"].toString(); modeStr == "region") {
    // 矩形選択 UI を表示し、完了後に beginCapture() を呼ぶ（非同期）
    regionSelector->show([this, json](int rx, int ry, int rw, int rh) {
      if (rw <= 0 || rh <= 0) {
        // キャンセル: Plugin 側に通知してタイマーをリセットさせる
        ipcServer->sendStatus(R"({"status":"cancelled"})");
        return;
      }
      beginCapture(json, rx, ry, rw, rh);
    });
    return; // 非同期 → show() 完了コールバックから beginCapture() を呼ぶ
  }

  beginCapture(json, 0, 0, 0, 0);
}

void RecorderApp::beginCapture(const juce::var &json, int regionX, int regionY,
                               int regionW, int regionH) {
  // ── Screen Recording 権限の事前チェック ────────────────────────
  // CGPreflightScreenCaptureAccess() は同期・高速。権限なしのまま SCK
  // を起動すると 音声だけの MP4
  // が作成されてしまうため、ここで早期リターンする。
  if (!ScreenRecorder::hasScreenRecordingPermission()) {
    ipcServer->sendStatus(
        R"({"status":"error","reason":"screen_recording_permission_denied"})");
    return;
  }

  // ── JSON からパラメータ取得 ────────────────────────────────────
  const auto sampleRate = static_cast<double>(json["sampleRate"]);
  const juce::String modeStr = json["captureMode"].toString(); // NOSONAR - used across multiple branches
  const auto useProRes = static_cast<bool>(json["useProRes"]);

  ScreenRecorder::CaptureMode mode = ScreenRecorder::CaptureMode::EntireDisplay;
  ScreenRecorder::ApplicationTarget appTarget;
  ScreenRecorder::RegionTarget region;

  if (modeStr == "application") {
    mode = ScreenRecorder::CaptureMode::Application;
    appTarget.bundleID = json["applicationBundleId"].toString().toStdString();
  } else if (modeStr == "region") {
    mode = ScreenRecorder::CaptureMode::CustomRegion;
    // startCapture() から渡された選択座標を使う（JSON の regionX/Y/W/H は無視）
    region.x = regionX;
    region.y = regionY;
    region.width = regionW;
    region.height = regionH;
  }

  // region モードのときは選択サイズを出力解像度にする（偶数アラインメント済み）
  // それ以外は JSON の outputWidth/Height を優先（プラグイン UI で HD/4K 選択）
  const auto requestedW = static_cast<int>(json["outputWidth"]);
  const auto requestedH = static_cast<int>(json["outputHeight"]);
  int kOutputWidth;
  if (modeStr == "region" && regionW > 0)
    kOutputWidth = regionW;
  else if (requestedW > 0)
    kOutputWidth = requestedW;
  else
    kOutputWidth = 1920;
  int kOutputHeight;
  if (modeStr == "region" && regionH > 0)
    kOutputHeight = regionH;
  else if (requestedH > 0)
    kOutputHeight = requestedH;
  else
    kOutputHeight = 1080;
  constexpr int kOutputFps = 60;

  // ── 保存先ディレクトリ（プラグインから指定された場合は上書き） ────────
  if (const juce::String outputPath = json["outputPath"].toString(); outputPath.isNotEmpty())
    outputManager->setOutputDirectory(juce::File(outputPath));

  // ── 出力ファイルパスを確定 ─────────────────────────────────────
  currentOutputFile = outputManager->generateOutputFile(useProRes);

  // ── FFmpegMuxer を開く ─────────────────────────────────────────
  {
    FFmpegMuxer::Settings muxSettings;
    muxSettings.outputFile = currentOutputFile;
    muxSettings.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    muxSettings.numChannels = DawCastShm::kNumChannels;
    muxSettings.width = kOutputWidth;
    muxSettings.height = kOutputHeight;
    muxSettings.fps = kOutputFps;
    muxSettings.useProRes = useProRes;

    juce::ScopedLock sl(muxerLock);
    if (!ffmpegMuxer->open(muxSettings)) {
      ipcServer->sendStatus(
          R"({"status":"error","reason":"muxer open failed"})");
      return;
    }
  }

  // ── AudioReceiver を開く ───────────────────────────────────────
  if (!audioReceiver->open(juce::String(DawCastShm::kName))) {
    juce::ScopedLock sl(muxerLock);
    ffmpegMuxer->close();
    ipcServer->sendStatus(
        R"({"status":"error","reason":"shared memory open failed"})");
    return;
  }

  // ── 音声ポンプスレッド起動 ─────────────────────────────────────
  // 録画開始時刻（mach_absolute_time を秒変換）を取得して渡す。
  // Plugin 側が setRecordingStart() を呼んだ後でないと 0.0 になるが、
  // 0.0 の場合は FFmpegMuxer 側で firstTs から相対時刻にフォールバックする。
  const double recStartSecs = audioReceiver->recordingStartSecs();
  audioPump = std::make_unique<AudioPumpThread>(
      *audioReceiver, *ffmpegMuxer, muxerLock, recStartSecs, sampleRate);
  audioPump->startThread();

  // ── ScreenRecorder 起動 ────────────────────────────────────────
  // キャプチャは常にネイティブ解像度 (display.width × backingScaleFactor)。
  // Retina ディスプレイで 4K 選択時は真の 4K、それ以外は swscale で拡縮される。
  // CustomRegion は ScreenRecorder 内部で region.width × scale に設定される。
  if (!screenRecorder->startRecording(mode, kOutputFps, /*width=*/0,
                                      /*height=*/0, appTarget, region)) {
    audioPump->stopThread(1000);
    audioPump.reset();
    audioReceiver->close();
    {
      juce::ScopedLock sl(muxerLock);
      ffmpegMuxer->close();
    }
    ipcServer->sendStatus(
        R"({"status":"error","reason":"screen recording failed"})");
    return;
  }

  recordingActive = true;
  // region モード時のみ録画中の枠インジケータを表示する
  if (modeStr == "region" && regionW > 0 && regionH > 0 && recordingBorder)
    recordingBorder->show(regionX, regionY, regionW, regionH);
  ipcServer->sendStatus(R"({"status":"recording"})");
  DBG("DawCastRecorder: recording started → " +
      currentOutputFile.getFullPathName());
}

void RecorderApp::stopCapture() {
  recordingActive = false;

  // 録画枠インジケータを先に隠す
  if (recordingBorder)
    recordingBorder->hide();

  // フレーム callback が以後呼ばれないよう先に停止
  if (screenRecorder)
    screenRecorder->stopRecording();

  // 音声ポンプ停止（最後のサンプルを書き終えるまで待つ）
  if (audioPump) {
    audioPump->stopThread(2000);
    audioPump.reset();
  }

  // AudioReceiver を閉じる
  if (audioReceiver)
    audioReceiver->close();

  // Muxer をフラッシュして trailer を書き出す
  {
    juce::ScopedLock sl(muxerLock);
    if (ffmpegMuxer)
      ffmpegMuxer->close();
  }

  // 完了通知: {"status":"done","path":"..."} をプラグインへ返す
  if (ipcServer) {
    juce::DynamicObject::Ptr obj(new juce::DynamicObject());
    obj->setProperty("status", juce::var(juce::String("done")));
    obj->setProperty("path", juce::var(currentOutputFile.getFullPathName()));
    ipcServer->sendStatus(juce::JSON::toString(juce::var(obj.get())));
  }

  DBG("DawCastRecorder: recording stopped → " +
      currentOutputFile.getFullPathName());
}
