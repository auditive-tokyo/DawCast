#pragma once
#include "SharedAudio.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

/**
 * AudioRingBuffer
 *
 * processBlock() スレッド（リアルタイム）から音声バッファを書き込む。
 * POSIX 共有メモリ（shm_open / mmap）を使って録画アプリ側 AudioReceiver と
 * 直接メモリを共有する。
 *
 * スレッドセーフ: 1ライター（processBlock）/ 1リーダー（AudioReceiver）の
 * ロックフリー実装。writePos のみを共有し readPos はプロセスローカル。
 *
 * initSharedMemory() を呼ぶ前はローカルバッファにフォールバックするため
 * ユニットテストは共有メモリなしで動作する。
 */
class AudioRingBuffer {
public:
  static constexpr int kNumChannels = DawCastShm::kNumChannels;
  static constexpr int kCapacitySamples = DawCastShm::kCapacitySamples;

  AudioRingBuffer();
  ~AudioRingBuffer();

  /**
   * POSIX 共有メモリを作成してバッファを切り替える。
   * prepareToPlay() から呼ぶ。失敗してもローカルバッファで動作継続。
   * @return true = 共有メモリ確保成功
   */
  bool initSharedMemory();

  /** processBlock() から呼び出す（リアルタイムスレッド）。 */
  void write(const juce::AudioBuffer<float> &source, int numSamples);

  /**
   * 録画開始時に呼ぶ。共有メモリに mach_absolute_time() を書き込み、
   * Recorder 側が映像と同一クロックで音声 PTS を計算できるようにする。
   * リアルタイムスレッドから呼んでも安全（atomic store のみ）。
   */
  void setRecordingStart() noexcept;

  /**
   * 録画終了時に呼ぶ。recordingStartMachTime を 0 にリセットする。
   */
  void clearRecordingStart() noexcept;

  /**
   * 読み取り。本番では AudioReceiver 経由で shm を直接読む。
   * このメソッドはユニットテスト用。
   * @return 実際に読み出したサンプル数
   */
  int read(juce::AudioBuffer<float> &dest, int numSamples);

  int availableSamples() const noexcept;
  void reset() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioRingBuffer)
};
