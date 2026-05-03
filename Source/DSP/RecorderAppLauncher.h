#pragma once
#include <juce_core/juce_core.h>

/**
 * RecorderAppLauncher
 *
 * DawCastRecorder.app をバックグラウンドで起動・終了する。
 * 起動時に OS から空きポートを取得し、Recorder に --port の引数で渡す。
 * VST3 / AU を同時にロードしてもポートが衝突しない。
 */
class RecorderAppLauncher
{
public:
    RecorderAppLauncher() = default;
    ~RecorderAppLauncher();

    /**
     * DawCastRecorder.app を起動する。
     * 起動前に OS から空きポートを取得し、--port で Recorder に渡す。
     */
    bool launch();

    /**
     * 内部 running フラグをリセットして launch() を再実行する。
     * macOS が TCC 権限変更時に Recorder を kill した際の自動再起動用。
     */
    bool relaunch();

    /** 録画アプリに終了を要求する。 */
    void quit();

    bool isRunning() const noexcept;

    /** 現在 Recorder がリッスン中のポート。launch() 前は 0。 */
    int currentPort() const noexcept { return port; }

private:
    juce::ChildProcess recorderProcess;
    bool running = false;
    int  port    = 0;

    juce::File findRecorderApp() const;

    /** OS にバインドさせて割り当てられた空きポートを返す。 */
    static int pickFreePort() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecorderAppLauncher)
};
