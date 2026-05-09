#pragma once
#include <atomic>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

class IPCServer;
class ScreenRecorder;
class AudioReceiver;
class FFmpegMuxer;
class OutputManager;
class RegionSelectorWindow;
class RecordingBorderWindow;

/**
 * RecorderApp
 *
 * DawCastRecorder のメインアプリケーションクラス。
 * - バックグラウンドで動作（Dock アイコン不要）
 * - IPCServer を起動してプラグインからの指示を待ち受ける
 * - ScreenRecorder / FFmpegMuxer / AudioReceiver を管理する
 */
class RecorderApp : public juce::JUCEApplication {
public:
  RecorderApp();
  ~RecorderApp() override;

  const juce::String getApplicationName() override { return "DawCastRecorder"; }    // NOSONAR - must match JUCE base class signature
  const juce::String getApplicationVersion() override { return "0.1.0"; }           // NOSONAR - must match JUCE base class signature
  bool moreThanOneInstanceAllowed() override { return false; }

  void initialise(const juce::String &commandLine) override;
  void shutdown() override;
  void systemRequestedQuit() override;

private:
  /** IPC コマンドを処理する（メッセージスレッドで呼ばれる）。 */
  void handleCommand(const juce::String &jsonStr);

  /** IPC "start" コマンドのエントリポイント。
   *  captureMode=="region" の場合は RegionSelectorWindow を表示してから
   *  beginCapture() を呼ぶ（非同期）。それ以外は直接 beginCapture() を呼ぶ。 */
  void startCapture(const juce::var &json);

  /** RegionSelectorWindow
   * の選択完了コールバックから呼ばれる（またはdisplay/application時に直接呼ばれる）。
   *  regionX/Y/W/H はすべて 0 のとき EntireDisplay または Application
   * モードとして無視する。 */
  void beginCapture(const juce::var &json, int regionX, int regionY,
                    int regionW, int regionH);

  void stopCapture();

  // ─── コンポーネント ───────────────────────────────────────────
  std::unique_ptr<IPCServer> ipcServer;
  std::unique_ptr<ScreenRecorder> screenRecorder;
  std::unique_ptr<AudioReceiver> audioReceiver;
  std::unique_ptr<FFmpegMuxer> ffmpegMuxer;
  std::unique_ptr<OutputManager> outputManager;
  std::unique_ptr<RegionSelectorWindow> regionSelector;
  std::unique_ptr<RecordingBorderWindow> recordingBorder;

  // ─── 音声ポンプスレッド ───────────────────────────────────────
  struct AudioPumpThread;
  std::unique_ptr<AudioPumpThread> audioPump;

  // ─── 状態 ─────────────────────────────────────────────────────
  std::atomic<bool> recordingActive{false};
  juce::File currentOutputFile;
  juce::CriticalSection muxerLock; ///< frameCallback とポンプスレッドの排他

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecorderApp)
};
