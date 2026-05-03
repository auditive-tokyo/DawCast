#include "IPCServer.h"
#include <juce_events/juce_events.h>

// ─── 前方宣言 ─────────────────────────────────────────────────────
class DawCastConnectionServer;

// ─── サーバー側接続クラス ─────────────────────────────────────────
// プラグイン 1 接続につき 1 インスタンス生成される。
// DawCastConnectionServer（= InterprocessConnectionServer）が所有・破棄する。

class DawCastServerConn final : public juce::InterprocessConnection {
public:
  explicit DawCastServerConn(DawCastConnectionServer &ownerIn)
      : juce::InterprocessConnection(false), owner(ownerIn) {}

  // IMPORTANT: デストラクタで disconnect() を必ず呼ぶ（JUCE の要件）
  ~DawCastServerConn() override { disconnect(); }

  void connectionMade() override;
  void connectionLost() override;
  void messageReceived(const juce::MemoryBlock &message) override;

  DawCastConnectionServer &owner;
};

// ─── 接続サーバークラス ───────────────────────────────────────────

class DawCastConnectionServer final
    : public juce::InterprocessConnectionServer {
public:
  /** クライアント接続時に呼ばれる JUCE ファクトリメソッド。
   *  返したポインタは InterprocessConnectionServer の内部 OwnedArray
   * が所有する。 */
  juce::InterprocessConnection *createConnectionObject() override {
    return new DawCastServerConn(*this); // NOSONAR - ownership transferred to JUCE's internal OwnedArray
  }

  // ─── DawCastServerConn から呼ばれるコールバック ─────────────

  void onConnectionMade(DawCastServerConn *conn) {
    juce::ScopedLock lock(connLock);
    currentConn = conn;
    juce::Logger::writeToLog("IPCServer: plugin connected");
    // プラグインに準備完了を通知（REC ボタンのグレイアウト解除に使われる）
    juce::String ready = R"({"status":"ready"})";
    juce::MemoryBlock block(ready.toRawUTF8(), ready.getNumBytesAsUTF8());
    conn->sendMessage(block);
  }

  void onConnectionLost(const DawCastServerConn *conn) {
    juce::ScopedLock lock(connLock);
    if (currentConn == conn)
      currentConn = nullptr;
    juce::Logger::writeToLog("IPCServer: plugin disconnected");
    // 切断通知: コールバック起動前に conn の参照をクリアしたから安全
    if (disconnectCallback)
      disconnectCallback();
  }

  void onMessage(const juce::String &json) const {
    juce::Logger::writeToLog("IPCServer: command received: " + json);
    if (commandCallback)
      commandCallback(json);
  }

  // ─── プラグインへのステータス送信 ────────────────────────────

  void sendStatus(const juce::String &json) {
    juce::ScopedLock lock(connLock);
    if (currentConn != nullptr && currentConn->isConnected()) {
      juce::MemoryBlock block(json.toRawUTF8(), json.getNumBytesAsUTF8());
      currentConn->sendMessage(block);
    }
  }

  void setCommandCallback(IPCServer::CommandCallback cb) {
    commandCallback = std::move(cb);
  }

  void setDisconnectCallback(std::function<void()> cb) {
    disconnectCallback = std::move(cb);
  }

private:
  IPCServer::CommandCallback commandCallback;
  std::function<void()> disconnectCallback;
  juce::CriticalSection connLock;
  DawCastServerConn *currentConn =
      nullptr; ///< weak ptr（所有は InterprocessConnectionServer）
};

// ─── DawCastServerConn メソッド定義（DawCastConnectionServer 完全定義後） ──

void DawCastServerConn::connectionMade() { owner.onConnectionMade(this); }

void DawCastServerConn::connectionLost() { owner.onConnectionLost(this); }

void DawCastServerConn::messageReceived(const juce::MemoryBlock &message) {
  juce::String json(static_cast<const char *>(message.getData()),
                    message.getSize());
  owner.onMessage(json);
}

// ─── Impl ────────────────────────────────────────────────────────

struct IPCServer::Impl {
  DawCastConnectionServer server;
};

// ─── IPCServer 実装 ──────────────────────────────────────────────

IPCServer::IPCServer() : impl(std::make_unique<Impl>()) {}
IPCServer::~IPCServer() { stop(); }

bool IPCServer::start(int port) {
  const bool ok = impl->server.beginWaitingForSocket(port, "127.0.0.1");
  if (ok)
    juce::Logger::writeToLog("IPCServer: listening on port " +
                             juce::String(port));
  else
    juce::Logger::writeToLog("IPCServer: failed to bind port " +
                             juce::String(port));
  return ok;
}

void IPCServer::stop() { impl->server.stop(); }

void IPCServer::sendStatus(const juce::String &jsonStatus) {
  impl->server.sendStatus(jsonStatus);
}

void IPCServer::setCommandCallback(CommandCallback callback) {
  impl->server.setCommandCallback(std::move(callback));
}

void IPCServer::setDisconnectCallback(std::function<void()> callback) {
  impl->server.setDisconnectCallback(std::move(callback));
}
