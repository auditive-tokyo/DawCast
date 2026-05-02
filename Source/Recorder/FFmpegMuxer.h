#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

/**
 * FFmpegMuxer
 *
 * 音声（PCM）と映像（H.264 / ProRes）を mux して動画ファイルに書き出す。
 * VideoToolbox によるハードウェアエンコードを使用。
 */
class FFmpegMuxer {
public:
  FFmpegMuxer();
  ~FFmpegMuxer();

  struct Settings {
    juce::File outputFile;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    double sampleRate = 48000.0;
    int numChannels = 2;
    bool useProRes = false; // false = H.264
  };

  bool open(const Settings &settings);
  void close();

  /** 映像フレームを追加する（CMSampleBuffer のラッパーを追加予定）。 */
  void writeVideoFrame(const void *pixelData, int stride,
                       double timestampSeconds);

  /** 音声サンプルを追加する。 */
  void writeAudioSamples(const juce::AudioBuffer<float> &buffer,
                         double timestampSeconds);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FFmpegMuxer)
};
