#include "AudioReceiver.h"
#include "SharedAudio.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ─── Impl ─────────────────────────────────────────────────────────────────

struct AudioReceiver::Impl {
  int shmFd = -1;
  void *shmMapPtr = nullptr;
  const DawCastShm::Layout *layout = nullptr;

  // 各プロセスローカルの readPos。shm には書き込まない。
  int readPos = 0;
};

// ─── コンストラクタ / デストラクタ ─────────────────────────────────────────

AudioReceiver::AudioReceiver() : impl(std::make_unique<Impl>()) {}
AudioReceiver::~AudioReceiver() { close(); }

// ─── open / close ──────────────────────────────────────────────────────────

bool AudioReceiver::open(const juce::String & /*shmName*/) {
  const size_t size = sizeof(DawCastShm::Layout);

  // プラグインが O_CREAT で作成済みの shm を読み取り専用で開く
  int fd = shm_open(DawCastShm::kName, O_RDONLY, 0);
  if (fd < 0) {
    juce::Logger::writeToLog(
        "AudioReceiver: shm_open failed: " + juce::String(strerror(errno)) +
        " (plugin may not have started shared memory yet)");
    return false;
  }

  // PROT_READ のみ — recorder は writePos を書かない（readPos はローカル管理）
  void *ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    juce::Logger::writeToLog("AudioReceiver: mmap failed: " +
                             juce::String(strerror(errno)));
    ::close(fd);
    return false;
  }

  impl->shmFd = fd;
  impl->shmMapPtr = ptr;
  impl->layout = static_cast<const DawCastShm::Layout *>(ptr);

  // 現在の writePos から読み始める（接続前のデータはスキップ）
  impl->readPos = impl->layout->writePos.load(std::memory_order_acquire);

  juce::Logger::writeToLog("AudioReceiver: shared memory opened, readPos=" +
                           juce::String(impl->readPos));
  return true;
}

void AudioReceiver::close() {
  if (impl->shmMapPtr != nullptr) {
    munmap(impl->shmMapPtr, sizeof(DawCastShm::Layout));
    impl->shmMapPtr = nullptr;
  }
  if (impl->shmFd >= 0) {
    ::close(impl->shmFd);
    impl->shmFd = -1;
  }
  impl->layout = nullptr;
  impl->readPos = 0;
}

// ─── read ──────────────────────────────────────────────────────────────────

int AudioReceiver::read(juce::AudioBuffer<float> &dest, int numSamples) {
  const auto *layout = impl->layout;
  if (layout == nullptr)
    return 0;

  const int wp = layout->writePos.load(std::memory_order_acquire);
  const int rp = impl->readPos;
  const int avail =
      (wp - rp + DawCastShm::kCapacitySamples) % DawCastShm::kCapacitySamples;
  const int toRead = juce::jmin(numSamples, avail);

  if (toRead == 0)
    return 0;

  const int numCh =
      juce::jmin((int)DawCastShm::kNumChannels, dest.getNumChannels());
  for (int ch = 0; ch < numCh; ++ch) {
    float *dst = dest.getWritePointer(ch);
    for (int i = 0; i < toRead; ++i)
      dst[i] = layout->samples[ch][(rp + i) % DawCastShm::kCapacitySamples];
  }

  impl->readPos = (rp + toRead) % DawCastShm::kCapacitySamples;
  return toRead;
}

double AudioReceiver::recordingStartSecs() const noexcept {
  const auto *layout = impl->layout;
  if (layout == nullptr)
    return 0.0;

  const uint64_t ticks =
      layout->recordingStartMachTime.load(std::memory_order_acquire);
  if (ticks == 0)
    return 0.0;

  // mach_absolute_time ティックを秒に変換。
  // SCKの CMSampleBufferGetPresentationTimeStamp
  // も同じ変換を内部で行っているため 直接比較できる。
  static mach_timebase_info_data_t tbInfo{0, 0};
  if (tbInfo.denom == 0)
    mach_timebase_info(&tbInfo);

  const double nanos = static_cast<double>(ticks) * tbInfo.numer / tbInfo.denom;
  return nanos * 1e-9;
}
