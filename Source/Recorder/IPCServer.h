#pragma once
#include <functional>
#include <juce_core/juce_core.h>

/**
 * IPCServer
 *
 * プラグイン（IPCClient）からの JUCE IPC 接続を localhost:9527 で待ち受ける。
 * コマンド受信時は CommandCallback を呼ぶ。
 * sendStatus() でプラグインへステータスを返信する。
 *
 * 同時接続は 1 本のみ想定（プラグイン 1 インスタンス : 録画アプリ 1 プロセス）。
 */
class IPCServer
{
public:
    /** JSON コマンド文字列を受け取るコールバック。
     *  例: {"cmd":"start","sampleRate":48000,"captureMode":"display",...} */
    using CommandCallback = std::function<void(const juce::String& jsonCommand)>;

    IPCServer();
    ~IPCServer();

    /** 指定ポートでの待ち受けを開始する。 */
    bool start (int port = 9527);

    void stop();

    /** 接続中のプラグインへステータス JSON を送信する。
     *  例: sendStatus(R"({"status":"recording"})") */
    void sendStatus (const juce::String& jsonStatus);

    /** コマンド受信コールバックを設定する。start() より前に呼ぶこと。 */
    void setCommandCallback (CommandCallback callback);

    /** プラグイン切断時コールバックを設定する。 */
    void setDisconnectCallback (std::function<void()> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IPCServer)
};
