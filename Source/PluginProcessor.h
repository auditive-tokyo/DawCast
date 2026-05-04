#pragma once
#include "DSP/AudioRingBuffer.h"
#include "DSP/IPCClient.h"
#include "DSP/RecorderAppLauncher.h"
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

class DawCastProcessor : public juce::AudioProcessor, private juce::Timer {
public:
  DawCastProcessor();
  ~DawCastProcessor() override;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;

  using juce::AudioProcessor::processBlock;
  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return JucePlugin_Name; } // NOSONAR - base class signature requires const return type

  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override { /* single preset – no-op */ }
  const juce::String getProgramName(int) override { return {}; } // NOSONAR - base class signature requires const return type
  void changeProgramName(int, const juce::String &) override { /* single preset – no-op */ }

  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;

  // ─── Editor から呼ばれる録画制御 ─────────────────────────────
  void startRecording(const IPCClient::StartParams &params);
  void stopRecording();

  bool isRecording() const noexcept { return recordingActive.load(); }
  bool isRecorderReady() const noexcept { return recorderReady.load(); }

  /** 録画開始時刻（juce::Time::currentTimeMillis()）。0 = 未録画。 */
  juce::int64 getRecordingStartTime() const noexcept {
    return recordingStartMs.load();
  }

  double getCurrentSampleRate() const noexcept { return currentSampleRate; }

  IPCClient &getIPCClient() noexcept { return ipcClient; }

  juce::String getOutputPath() const { return outputPath; }
  void setOutputPath(const juce::String &path) { outputPath = path; }

  /** キャプチャモード ("display" / "application" / "region") */
  juce::String getCaptureMode() const { return captureMode; }
  void setCaptureMode(const juce::String &mode) { captureMode = mode; }

  bool getUseProRes() const noexcept { return useProRes; }
  void setUseProRes(bool v) noexcept { useProRes = v; }

  /** 4K (3840x2160) 出力。false = HD (1920x1080) */
  bool getResolution4K() const noexcept { return resolution4K; }
  void setResolution4K(bool v) noexcept { resolution4K = v; }

  /** ホストアプリ（DAW）の Bundle ID を返す。取得できない場合は空文字列。 */
  static juce::String getHostBundleID() noexcept;

private:
  AudioRingBuffer ringBuffer;
  IPCClient ipcClient;
  RecorderAppLauncher launcher;

  double currentSampleRate = 44100.0;
  std::atomic<bool> recordingActive{false};
  std::atomic<bool> recorderReady{false};
  std::atomic<juce::int64> recordingStartMs{0};
  juce::String outputPath;
  juce::String captureMode{"display"};
  bool useProRes{false};
  bool resolution4K{false};

  // ── Recorder 自動再起動用 Timer ───────────────────────────
  // "disconnected" ステータス受信時にシングルショットで起動し、
  // Recorder を再起動して IPC を再接続する。
  void timerCallback() override;
  std::atomic<bool> relaunchScheduled{false};

  // ── ダングリング防止 ─────────────────────────────────────
  // デストラクタで false にセットし、callAfterDelay 等の遅延ラムダが
  // 破棄済みの this を参照しないようにする。
  std::shared_ptr<std::atomic<bool>> alive{
      std::make_shared<std::atomic<bool>>(true)};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DawCastProcessor)
};
