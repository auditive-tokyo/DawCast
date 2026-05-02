#pragma once
#include <atomic>
#include <cstdint>

/**
 * DawCastShm — プラグインと録画アプリが共有するリングバッファのレイアウト定義。
 *
 * POSIX 共有メモリ（shm_open / mmap）上に DawCastShm::Layout を配置して
 * 両プロセスが同じメモリを参照する。
 *
 * 所有権:
 *   プラグイン側（AudioRingBuffer）  : O_CREAT|O_RDWR で作成、shm_unlink で削除
 *   録画アプリ側（AudioReceiver）    : O_RDONLY で開く（readonly mmap）
 *
 * writePos のみを共有。readPos は各プロセスがローカルに保持する。
 * arm64 上の std::atomic<int> は常に lock-free（ハードウェア保証）。
 */
namespace DawCastShm {

constexpr char kName[] = "dawcast_audio_shm";
constexpr int kNumChannels = 2;
constexpr int kCapacitySamples = 65536; // ~1.4 秒 @ 48kHz

/**
 * 共有メモリ上に配置されるリングバッファのレイアウト。
 * 各フィールドを 64
 * バイトキャッシュラインに隔離してフォルスシェアリングを防ぐ。
 *
 * recordingStartMachTime:
 *   録画開始時に Plugin（AudioRingBuffer::setRecordingStart()）が
 *   mach_absolute_time() の値を書き込む。
 *   Recorder（AudioReceiver）が読み出し、映像と同じ物理クロックで
 *   音声 PTS を計算することで長時間録画でも A/V ズレが蓄積しない。
 *   0 = 録画未開始。
 */
struct Layout {
  std::atomic<int> writePos{0};
  char _pad1[60]; // writePos を 64 バイトキャッシュラインに収める

  // 録画開始時の mach_absolute_time（ティック）。
  // arm64: uint64_t の std::atomic は常に lock-free。
  std::atomic<uint64_t> recordingStartMachTime{0};
  char _pad2[56]; // recordingStartMachTime(8) + _pad2(56) = 64 バイト

  float samples[kNumChannels][kCapacitySamples];
};

} // namespace DawCastShm
