#pragma once
#include <functional>
#include <juce_core/juce_core.h>

/**
 * IPCClient
 *
 * 録画アプリ（DawCastRecorder）への JUCE IPC 通信を担当する。
 * localhost:9527 で待ち受ける IPCServer に TCP で接続し、JSON
 * メッセージを送受信する。
 *
 * スレッド安全性: connect() / disconnect() / sendStart() / sendStop() は
 * 任意のスレッドから呼び出し可能。
 */
class IPCClient {
public:
  /**
   * start コマンドのパラメータ。
   * captureMode は ScreenRecorder::CaptureMode に対応する文字列表現:
   *   "display"     — メインディスプレイ全体
   *   "application" — 指定アプリのウィンドウのみ
   *   "region"      — ユーザー指定の矩形領域（RecorderApp 統合後に対応）
   */
  struct StartParams {
    double sampleRate = 48000.0;
    juce::String captureMode =
        "display"; // "display" | "application" | "region"
    juce::String
        applicationBundleId; // Application モード用 (例: "com.ableton.live")
    int regionX = 0, regionY = 0;
    int regionWidth = 0, regionHeight = 0; // Region モード用
    juce::String outputPath; // 出力ディレクトリ（空 = Recorder デフォルト使用）
  };

  /** 録画アプリからのステータス通知 JSON を受け取るコールバック。
   *  例: {"status":"recording"} / {"status":"done","path":"/Movies/..."} */
  using StatusCallback = std::function<void(const juce::String &jsonStatus)>;

  /** Recorder との IPC 接続が失われた際のコールバック。
   *  IPC スレッドから呼ばれるため MessageManager::callAsync 経由で使うこと。 */
  using DisconnectCallback = std::function<void()>;

  IPCClient();
  ~IPCClient();

  /** localhost:指定ポートへの接続を試みる。2 秒タイムアウト。 */
  bool connect(int port);

  void disconnect();
  bool isConnected() const noexcept;

  /** start コマンドを JSON で送信する。 */
  void sendStart(const StartParams &params);

  /** {"cmd":"stop"} を送信する。 */
  void sendStop();

  /** {"cmd":"quit"} を送信する。Recorder プロセスを正常終了させる。 */
  void sendQuit();

  /** 録画アプリからのステータス通知コールバックを設定する。 */
  void setStatusCallback(StatusCallback callback);

  /** IPC 接続失殌時コールバックを設定する。 */
  void setDisconnectCallback(DisconnectCallback callback);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IPCClient)
};
