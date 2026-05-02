#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

/**
 * AudioReceiver
 *
 * 共有メモリ（リングバッファ）からプラグインが書き込んだ音声データを読み出す。
 */
class AudioReceiver {
public:
  AudioReceiver();
  ~AudioReceiver();

  bool open(const juce::String &shmName);
  void close();

  /** 利用可能なサンプルを dest に読み出す。戻り値は読み出したサンプル数。 */
  int read(juce::AudioBuffer<float> &dest, int numSamples);

  /**
   * 録画開始時の mach_absolute_time を秒に変換して返す。
   * Plugin 側が setRecordingStart() を呼んでいない場合は 0.0。
   * 映像の CMSampleBuffer タイムスタンプ（= mach_absolute_time ベース）と
   * 同一クロックなので、この値を音声 PTS の原点に使うと A/V ズレがゼロになる。
   */
  double recordingStartSecs() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioReceiver)
};
