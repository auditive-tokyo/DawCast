#include "IPCClient.h"
#include <juce_events/juce_events.h>

// ─── JUCE IPC 接続クラス ─────────────────────────────────────────
// juce::InterprocessConnection: TCP
// ソケット上でフレーム付きバイナリメッセージを送受信する。
// callbacksOnMessageThread=false
// にして接続スレッドで直接コールバックを受け取る。

class DawCastClientConnection final : public juce::InterprocessConnection {
public:
  DawCastClientConnection() : juce::InterprocessConnection(false) {}

  // IMPORTANT: デストラクタで disconnect() を必ず呼ぶ（JUCE の要件）
  ~DawCastClientConnection() override { disconnect(); }

  std::function<void(const juce::String &)> onMessage;
  std::function<void()> onDisconnect;

  void connectionMade() override {
    juce::Logger::writeToLog("IPCClient: connected to DawCastRecorder");
  }

  void connectionLost() override {
    juce::Logger::writeToLog("IPCClient: connection to DawCastRecorder lost");
    if (onDisconnect)
      onDisconnect();
  }

  void messageReceived(const juce::MemoryBlock &message) override {
    juce::String json(static_cast<const char *>(message.getData()),
                      message.getSize());
    juce::Logger::writeToLog("IPCClient: status received: " + json);
    if (onMessage)
      onMessage(json);
  }
};

// ─── Impl ────────────────────────────────────────────────────────

struct IPCClient::Impl {
  DawCastClientConnection connection;
};

// ─── 内部ヘルパー ─────────────────────────────────────────────────

static void sendJsonMessage(juce::InterprocessConnection &conn,
                            const juce::var &obj) {
  const juce::String json = juce::JSON::toString(obj, true); // true = 1行
  juce::MemoryBlock block(json.toRawUTF8(), json.getNumBytesAsUTF8());
  conn.sendMessage(block);
}

// ─── IPCClient 実装 ──────────────────────────────────────────────

IPCClient::IPCClient() : impl(std::make_unique<Impl>()) {}
IPCClient::~IPCClient() { disconnect(); }

bool IPCClient::connect(int port) {
  return impl->connection.connectToSocket("127.0.0.1", port, 2000);
}

void IPCClient::disconnect() { impl->connection.disconnect(); }
bool IPCClient::isConnected() const noexcept {
  return impl->connection.isConnected();
}

void IPCClient::setStatusCallback(StatusCallback callback) {
  impl->connection.onMessage = std::move(callback);
}

void IPCClient::setDisconnectCallback(DisconnectCallback callback) {
  impl->connection.onDisconnect = std::move(callback);
}

void IPCClient::sendStart(const StartParams &params) {
  if (!impl->connection.isConnected())
    return;

  auto *obj = new juce::DynamicObject(); // NOSONAR - juce::var takes ownership via reference counting
  obj->setProperty("cmd", juce::var("start"));
  obj->setProperty("timestamp", juce::var(juce::Time::currentTimeMillis()));
  obj->setProperty("sampleRate", juce::var(params.sampleRate));
  obj->setProperty("captureMode", juce::var(params.captureMode));

  if (params.captureMode == "application" &&
      params.applicationBundleId.isNotEmpty())
    obj->setProperty("applicationBundleId",
                     juce::var(params.applicationBundleId));

  if (params.captureMode == "region" && params.regionWidth > 0) {
    obj->setProperty("regionX", juce::var(params.regionX));
    obj->setProperty("regionY", juce::var(params.regionY));
    obj->setProperty("regionWidth", juce::var(params.regionWidth));
    obj->setProperty("regionHeight", juce::var(params.regionHeight));
  }

  if (params.outputPath.isNotEmpty())
    obj->setProperty("outputPath", juce::var(params.outputPath));

  obj->setProperty("useProRes", juce::var(params.useProRes));

  if (params.outputWidth > 0 && params.outputHeight > 0) {
    obj->setProperty("outputWidth", juce::var(params.outputWidth));
    obj->setProperty("outputHeight", juce::var(params.outputHeight));
  }

  sendJsonMessage(impl->connection, juce::var(obj));
}

void IPCClient::sendStop() {
  if (!impl->connection.isConnected())
    return;

  auto *obj = new juce::DynamicObject(); // NOSONAR - juce::var takes ownership via reference counting
  obj->setProperty("cmd", juce::var("stop"));
  sendJsonMessage(impl->connection, juce::var(obj));
}

void IPCClient::sendQuit() {
  if (!impl->connection.isConnected())
    return;

  auto *obj = new juce::DynamicObject(); // NOSONAR - juce::var takes ownership via reference counting
  obj->setProperty("cmd", juce::var("quit"));
  sendJsonMessage(impl->connection, juce::var(obj));
}
