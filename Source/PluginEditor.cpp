#include "PluginEditor.h"

DawCastEditor::DawCastEditor(DawCastProcessor &p)
    : AudioProcessorEditor(&p), processorRef(p) {
  // ─── キャプチャモード選択 ─────────────────────────────────────
  captureModeBox.addItem("Full Screen", 1);
  captureModeBox.addItem("DAW Window", 2);
  captureModeBox.addItem("Custom Region", 3);

  // プロセッサー保存値から起動時に復元
  {
    const juce::String mode = processorRef.getCaptureMode();
    int initId = 1;
    if (mode == "application")
      initId = 2;
    else if (mode == "region")
      initId = 3;
    captureModeBox.setSelectedId(initId, juce::dontSendNotification);
  }

  captureModeBox.onChange = [this] {
    switch (captureModeBox.getSelectedId()) {
    case 2:
      processorRef.setCaptureMode("application");
      break;
    case 3:
      processorRef.setCaptureMode("region");
      break;
    default:
      processorRef.setCaptureMode("display");
      break;
    }
  };

  // ─── REC ボタン ───────────────────────────────────────────────
  recButton.onClick = [this] {
    if (processorRef.isRecording())
      return;

    IPCClient::StartParams params;
    params.sampleRate = processorRef.getCurrentSampleRate();
    params.captureMode = processorRef.getCaptureMode();

    if (params.captureMode == "application")
      params.applicationBundleId = DawCastProcessor::getHostBundleID();

    processorRef.startRecording(params);
  };

  // ─── STOP ボタン ──────────────────────────────────────────────
  stopButton.onClick = [this] { processorRef.stopRecording(); };

  // ─── ラベル初期設定 ───────────────────────────────────────────
  timeLabel.setText("Time: 00:00:00", juce::dontSendNotification);
  statusLabel.setText(
      "Audio: " +
          juce::String(processorRef.getCurrentSampleRate() / 1000.0, 1) + "kHz",
      juce::dontSendNotification);

  // ─── ウィジェット追加 ─────────────────────────────────────────
  addAndMakeVisible(recButton);
  addAndMakeVisible(stopButton);
  addAndMakeVisible(timeLabel);
  addAndMakeVisible(statusLabel);
  addAndMakeVisible(captureModeBox);

  // ─── 保存先フォルダ選択 ────────────────────────────────────────
  pathPresetBox.addItem("Movies", 1);
  pathPresetBox.addItem("Desktop", 2);
  pathPresetBox.addItem("Downloads", 3);
  pathPresetBox.addItem("Custom...", 4);

  // プロセッサーの outputPath から初期選択を復元
  {
    const juce::String saved = processorRef.getOutputPath();
    int initId = 4; // Custom
    for (int i = 1; i <= 3; ++i) {
      if (saved == pathForPreset(i).getFullPathName()) {
        initId = i;
        break;
      }
    }
    pathPresetBox.setSelectedId(initId, juce::dontSendNotification);
  }

  pathPresetBox.onChange = [this] {
    const int id = pathPresetBox.getSelectedId();
    if (id >= 1 && id <= 3) {
      processorRef.setOutputPath(pathForPreset(id).getFullPathName());
      updatePathDisplay();
    }
  };

  browseButton.onClick = [this] {
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select output folder", juce::File(processorRef.getOutputPath()));
    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser &fc) {
          const auto result = fc.getResult();
          if (result.isDirectory()) {
            processorRef.setOutputPath(result.getFullPathName());
            pathPresetBox.setSelectedId(4, juce::dontSendNotification);
            updatePathDisplay();
          }
        });
  };

  pathLabel.setFont(juce::FontOptions(11.0f));
  pathLabel.setJustificationType(juce::Justification::centredLeft);
  updatePathDisplay();

  addAndMakeVisible(pathPresetBox);
  addAndMakeVisible(browseButton);
  addAndMakeVisible(pathLabel);

  // ─── 出力フォーマット選択 ──────────────────────────────────
  outputFormatBox.addItem("H.264 / AAC  (.mp4)", 1);
  outputFormatBox.addItem("ProRes / AAC (.mov)", 2);
  outputFormatBox.setSelectedId(processorRef.getUseProRes() ? 2 : 1,
                                juce::dontSendNotification);
  outputFormatBox.onChange = [this] {
    processorRef.setUseProRes(outputFormatBox.getSelectedId() == 2);
  };
  addAndMakeVisible(outputFormatBox);

  // 200ms ごとに UI を更新
  startTimer(200);

  setSize(300, 200);
}

DawCastEditor::~DawCastEditor() { stopTimer(); }

void DawCastEditor::timerCallback() {
  const bool rec = processorRef.isRecording();

  // 録画時間の更新
  if (rec) {
    const juce::int64 startMs = processorRef.getRecordingStartTime();
    const juce::int64 elapsed = juce::Time::currentTimeMillis() - startMs;
    timeLabel.setText("Time: " + formatElapsed(elapsed),
                      juce::dontSendNotification);
  } else {
    timeLabel.setText("Time: 00:00:00", juce::dontSendNotification);
  }

  // 接続状態とサンプルレートの表示
  const bool connected = processorRef.getIPCClient().isConnected();
  const juce::String srStr =
      juce::String(processorRef.getCurrentSampleRate() / 1000.0, 1) + "kHz";
  const juce::String connStr = connected ? "" : " [disconnected]";
  statusLabel.setText("Audio: " + srStr + connStr, juce::dontSendNotification);

  // ボタンの有効 / 無効
  recButton.setEnabled(!rec && processorRef.isRecorderReady());
  stopButton.setEnabled(rec);
  captureModeBox.setEnabled(!rec);
  pathPresetBox.setEnabled(!rec);
  browseButton.setEnabled(!rec);
  outputFormatBox.setEnabled(!rec);
}

juce::String DawCastEditor::formatElapsed(juce::int64 elapsedMs) noexcept {
  const int totalSec = (int)(elapsedMs / 1000);
  const int h = totalSec / 3600;
  const int m = (totalSec % 3600) / 60;
  const int s = totalSec % 60;

  return juce::String::formatted("%02d:%02d:%02d", h, m, s);
}

void DawCastEditor::paint(juce::Graphics &g) {
  g.fillAll(juce::Colours::darkgrey);
}

void DawCastEditor::resized() {
  auto area = getLocalBounds().reduced(8);

  // 上段: REC / STOP
  auto top = area.removeFromTop(36);
  recButton.setBounds(top.removeFromLeft(120));
  top.removeFromLeft(8);
  stopButton.setBounds(top.removeFromLeft(120));

  area.removeFromTop(4);

  // キャプチャモード
  captureModeBox.setBounds(area.removeFromTop(24));

  area.removeFromTop(4);

  // 保存先フォルダ選択
  auto pathRow = area.removeFromTop(24);
  browseButton.setBounds(pathRow.removeFromRight(72));
  pathRow.removeFromRight(4);
  pathPresetBox.setBounds(pathRow);

  area.removeFromTop(2);
  pathLabel.setBounds(area.removeFromTop(14));

  area.removeFromTop(4);

  // 出力フォーマット
  outputFormatBox.setBounds(area.removeFromTop(24));

  area.removeFromTop(4);

  // Time / Status
  timeLabel.setBounds(area.removeFromTop(20));
  statusLabel.setBounds(area.removeFromTop(20));
}

juce::File DawCastEditor::pathForPreset(int id) noexcept {
  switch (id) {
  case 2:
    return juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
        .getChildFile("DawCast");
  case 3:
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("Downloads/DawCast");
  default:
    return juce::File::getSpecialLocation(juce::File::userMoviesDirectory)
        .getChildFile("DawCast");
  }
}

void DawCastEditor::updatePathDisplay() {
  const juce::String path = processorRef.getOutputPath();
  const juce::String home =
      juce::File::getSpecialLocation(juce::File::userHomeDirectory)
          .getFullPathName();
  const juce::String display =
      path.startsWith(home) ? "~" + path.substring(home.length()) : path;
  pathLabel.setText(display, juce::dontSendNotification);
}
