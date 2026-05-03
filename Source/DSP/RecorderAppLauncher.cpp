#include "RecorderAppLauncher.h"
#include <CoreFoundation/CoreFoundation.h>
#include <array>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

RecorderAppLauncher::~RecorderAppLauncher() { quit(); }

int RecorderAppLauncher::pickFreePort() noexcept {
  // port 0 でバインド→ OS が空きポートを割り当てる→取得してクローズ。
  // 小さな TOCTOU レースがあるがローカル IPC 用途では許容範囲。
  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return 9527; // フォールバック

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  // NOSONAR - POSIX socket API idiom: sockaddr_in* ↔ sockaddr* aliasing is guaranteed by spec
  auto *const sockAddr = reinterpret_cast<struct sockaddr *>(&addr); // NOSONAR

  if (::bind(sock, sockAddr, sizeof(addr)) < 0) {
    ::close(sock);
    return 9527;
  }

  socklen_t len = sizeof(addr);
  ::getsockname(sock, sockAddr, &len);
  const int assignedPort = ntohs(addr.sin_port);
  ::close(sock);
  return assignedPort;
}

bool RecorderAppLauncher::launch() {
  if (running)
    return true;

  const juce::File app = findRecorderApp();
  if (!app.isDirectory()) // .app バンドルはディレクトリ
  {
    jassertfalse; // DawCastRecorder.app が見つからない
    return false;
  }

  // "open --background" で起動することで DawCastRecorder を独立したアプリとして
  // macOS TCC が認識する（Ableton ではなく DawCastRecorder
  // 自身に許可が紐づく）。 終了は IPC "quit"
  // コマンドで行う（RecorderAppLauncher::quit() 参照）。
  port = pickFreePort();
  const juce::StringArray args{
      "/usr/bin/open", "--background", app.getFullPathName(),
      "--args",        "--port",       juce::String(port)};
  running = recorderProcess.start(args);
  return running;
}

void RecorderAppLauncher::quit() {
  if (!running)
    return;
  recorderProcess.kill();
  running = false;
}

bool RecorderAppLauncher::relaunch() {
  // 内部フラグを強制リセットして launch() を再実行する。
  // open --background は即時に戻るため ChildProcess は既に終了済み、
  // kill() でクリーンアップしてから launch() し直す。
  recorderProcess.kill();
  running = false;
  return launch();
}

bool RecorderAppLauncher::isRunning() const noexcept { return running; }

juce::File RecorderAppLauncher::findRecorderApp() const {
  // ── 優先: CFBundle API でプラグインバンドルを特定 ────────────
  // DAW にロードされた VST3/AU バンドル（com.auditive.dawcast）を探す。
  if (CFBundleRef bundle =
          CFBundleGetBundleWithIdentifier(CFSTR("com.auditive.dawcast"))) {
    if (CFURLRef url = CFBundleCopyBundleURL(bundle)) {
      std::array<UInt8, PATH_MAX> path{};
      const bool ok = CFURLGetFileSystemRepresentation(
          url, true, path.data(), static_cast<CFIndex>(path.size()));
      CFRelease(url);

      if (ok)
        return juce::File(juce::String::fromUTF8(reinterpret_cast<const char *>(
                              path.data()))) // NOSONAR - UInt8→char* read-only
                                             // reinterpret is safe
            .getChildFile("Contents/Resources/"
                          "DawCastRecorder.app");
    }
  }

  // ── フォールバック: 実行ファイルの隣を探す（開発ビルド用） ───
  // ビルドディレクトリ: build/DawCastPlugin_artefacts/Debug/VST3/
  //   DawCast.vst3/Contents/MacOS/DawCast
  //   → ../../Resources/DawCastRecorder.app
  return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
      .getParentDirectory()
      .getParentDirectory()
      .getChildFile("Resources/DawCastRecorder.app");
}
