#include "AudioRingBuffer.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ─── Impl ─────────────────────────────────────────────────────────────────

struct AudioRingBuffer::Impl {
  // 共有メモリ状態（shmFd >= 0 の場合のみ有効）
  int shmFd = -1;
  void *shmMapPtr = nullptr;

  // 有効なレイアウトポインタ（shm-backed または localLayout）
  DawCastShm::Layout *layout = nullptr;

  // ローカルフォールバック（initSharedMemory() 前 / shm 失敗時）
  std::unique_ptr<DawCastShm::Layout> localLayout;

  // ローカル readPos（AudioRingBuffer::read() 用、テストのみで使用）
  int localReadPos = 0;
};

// ─── コンストラクタ / デストラクタ ─────────────────────────────────────────

AudioRingBuffer::AudioRingBuffer() : impl(std::make_unique<Impl>()) {
  // ローカルバッファをフォールバックとして確保。initSharedMemory()
  // が呼ばれると解放される。
  impl->localLayout = std::make_unique<DawCastShm::Layout>();
  impl->layout = impl->localLayout.get();
}

AudioRingBuffer::~AudioRingBuffer() {
  if (impl->shmMapPtr != nullptr) {
    munmap(impl->shmMapPtr, sizeof(DawCastShm::Layout));
    impl->shmMapPtr = nullptr;
  }
  if (impl->shmFd >= 0) {
    ::close(impl->shmFd);
    shm_unlink(DawCastShm::kName); // 作成者がリンクを解除する
    impl->shmFd = -1;
  }
}

// ─── 共有メモリ初期化 ───────────────────────────────────────────────────────

bool AudioRingBuffer::initSharedMemory() {
  const size_t size = sizeof(DawCastShm::Layout);

  int fd = shm_open(DawCastShm::kName, O_CREAT | O_RDWR, 0600);
  if (fd < 0) {
    juce::Logger::writeToLog("AudioRingBuffer: shm_open failed: " +
                             juce::String(strerror(errno)));
    return false;
  }

  if (ftruncate(fd, (off_t)size) < 0) {
    juce::Logger::writeToLog("AudioRingBuffer: ftruncate failed: " +
                             juce::String(strerror(errno)));
    ::close(fd);
    return false;
  }

  void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    juce::Logger::writeToLog("AudioRingBuffer: mmap failed: " +
                             juce::String(strerror(errno)));
    ::close(fd);
    return false;
  }

  // placement-new でレイアウトを初期化（writePos=0、samples=0）
  new (ptr) DawCastShm::Layout{};

  impl->shmFd = fd;
  impl->shmMapPtr = ptr;
  impl->layout = static_cast<DawCastShm::Layout *>(ptr);
  impl->localLayout.reset(); // shm が有効になったのでローカルバッファを解放
  impl->localReadPos = 0;

  juce::Logger::writeToLog("AudioRingBuffer: shared memory created (" +
                           juce::String((int)size) + " bytes)");
  return true;
}

// ─── リングバッファ操作 ─────────────────────────────────────────────────────

void AudioRingBuffer::write(const juce::AudioBuffer<float> &source,
                            int numSamples) {
  auto *layout = impl->layout;
  if (layout == nullptr)
    return;

  const int wp = layout->writePos.load(std::memory_order_relaxed);

  for (int ch = 0; ch < DawCastShm::kNumChannels; ++ch) {
    const float *src = source.getReadPointer(ch);
    for (int i = 0; i < numSamples; ++i)
      layout->samples[ch][(wp + i) % DawCastShm::kCapacitySamples] = src[i];
  }

  layout->writePos.store((wp + numSamples) % DawCastShm::kCapacitySamples,
                         std::memory_order_release);
}

int AudioRingBuffer::read(juce::AudioBuffer<float> &dest, int numSamples) {
  auto *layout = impl->layout;
  if (layout == nullptr)
    return 0;

  const int avail = availableSamples();
  const int toRead = juce::jmin(numSamples, avail);
  const int rp = impl->localReadPos;

  for (int ch = 0; ch < DawCastShm::kNumChannels; ++ch) {
    float *dst = dest.getWritePointer(ch);
    for (int i = 0; i < toRead; ++i)
      dst[i] = layout->samples[ch][(rp + i) % DawCastShm::kCapacitySamples];
  }

  impl->localReadPos = (rp + toRead) % DawCastShm::kCapacitySamples;
  return toRead;
}

int AudioRingBuffer::availableSamples() const noexcept {
  auto *layout = impl->layout;
  if (layout == nullptr)
    return 0;

  const int wp = layout->writePos.load(std::memory_order_acquire);
  const int rp = impl->localReadPos;
  return (wp - rp + DawCastShm::kCapacitySamples) %
         DawCastShm::kCapacitySamples;
}

void AudioRingBuffer::reset() noexcept {
  auto *layout = impl->layout;
  if (layout == nullptr)
    return;

  layout->writePos.store(0, std::memory_order_relaxed);
  impl->localReadPos = 0;
}

void AudioRingBuffer::setRecordingStart() noexcept {
  auto *layout = impl->layout;
  if (layout == nullptr)
    return;
  // mach_absolute_time() は SCK の CMSampleBuffer タイムスタンプと同じ
  // 物理クロック（arm64 ではディスプレイ・音声ハードウェア共通）。
  // これを共有メモリに書くことで Recorder 側が映像と同一原点で
  // 音声 PTS を計算できる。
  layout->recordingStartMachTime.store(mach_absolute_time(),
                                       std::memory_order_release);
}

void AudioRingBuffer::clearRecordingStart() noexcept {
  auto *layout = impl->layout;
  if (layout == nullptr)
    return;
  layout->recordingStartMachTime.store(0, std::memory_order_release);
}
