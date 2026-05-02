#pragma once
#include <juce_core/juce_core.h>
#include <CoreMedia/CoreMedia.h>
#include <functional>
#include <string>

/**
 * Application モード用のアプリ指定。
 * bundleID が非空なら bundleID 優先。空なら pid を使う。
 */
struct ScreenRecorderApplicationTarget
{
    std::string bundleID; ///< 例: "com.ableton.live", "com.apple.logic10"
    int         pid = 0;  ///< bundleID が空の場合に使用
};

/**
 * CustomRegion モード用の矩形（ディスプレイ座標、pt 単位）。
 */
struct ScreenRecorderRegionTarget
{
    int x = 0, y = 0, width = 0, height = 0;
};

/**
 * ScreenRecorder
 *
 * ScreenCaptureKit を使って画面を録画する。
 * macOS 12.3+ / arm64 専用。実装は ScreenRecorder.mm（Objective-C++ / ARC）。
 *
 * キャプチャしたフレームは FrameCallback で通知する。
 * RecorderApp が callback を FFmpegMuxer::writeVideoFrame() に接続する。
 *
 * 注意: startRecording() は非同期。SCShareableContent の権限取得が完了するまで
 *       isRecording() は false を返す。
 */
class ScreenRecorder
{
public:
    using ApplicationTarget = ScreenRecorderApplicationTarget;
    using RegionTarget      = ScreenRecorderRegionTarget;

    /**
     * キャプチャ対象の種類。
     * IPC の start コマンドの "captureMode" フィールドに対応する。
     */
    enum class CaptureMode
    {
        /**
         * メインディスプレイ全体。デフォルト。
         * IPC: "display"
         */
        EntireDisplay,

        /**
         * 指定アプリケーション（+ そのプロセス内プラグインウィンドウ）のみ。
         * DAW を PID またはバンドル ID で指定する。
         * IPC: "application"
         */
        Application,

        /**
         * ユーザー指定の矩形領域。
         * SCStreamConfiguration.sourceRect に CGRect を渡してトリミング（macOS 14.2+）。
         * IPC: "region"
         * （未実装 — RecorderApp の UI 選択フロー完成後に対応予定）
         */
        CustomRegion,
    };

    /**
     * フレームコールバック。
     * ScreenCaptureKit のキャプチャスレッド（非リアルタイム）から呼ばれる。
     * sampleBuffer の lifetime はコールバック内のみ保証。保持する場合は CFRetain する。
     */
    using FrameCallback = std::function<void(CMSampleBufferRef sampleBuffer, double timestampSeconds)>;

    ScreenRecorder();
    ~ScreenRecorder();

    /**
     * フレーム受信コールバックを設定する。startRecording() より前に呼ぶこと。
     */
    void setFrameCallback (FrameCallback callback);

    /**
     * 録画を開始する。
     *
     * @param mode      キャプチャモード（デフォルト: EntireDisplay）
     * @param fps       目標フレームレート（デフォルト 60）
     * @param width     出力幅 px（0 = キャプチャ解像度をそのまま使用）
     * @param height    出力高さ px（0 = キャプチャ解像度をそのまま使用）
     * @param appTarget Application モード時のアプリ指定
     * @param region    CustomRegion モード時の矩形指定
     * @return false = Screen Recording 権限がない、ディスプレイが見つからない等
     */
    bool startRecording (CaptureMode       mode      = CaptureMode::EntireDisplay,
                         int               fps       = 60,
                         int               width     = 0,
                         int               height    = 0,
                         ApplicationTarget appTarget = {},
                         RegionTarget      region    = {});

    void stopRecording();

    bool isRecording() const noexcept;

    /**
     * Screen Recording の TCC 権限があるかチェックする。
     * 権限がない場合は macOS の許可ダイアログ（または System Settings へ誘導）を出す。
     * YES = 既に権限あり。NO = ダイアログを出した（=今回の REC はキャンセル、再試行が必要）。
     */
    static bool hasScreenRecordingPermission() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScreenRecorder)
};

