#pragma once
#include "PluginProcessor.h"
#include <juce_audio_processors/juce_audio_processors.h>

class DawCastEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
  explicit DawCastEditor(DawCastProcessor &);
  ~DawCastEditor() override;

  void paint(juce::Graphics &) override;
  void resized() override;

private:
  void timerCallback() override;

  /** 録画経過時間を "HH:MM:SS" 形式の文字列に変換する。 */
  static juce::String formatElapsed(juce::int64 elapsedMs) noexcept;

  DawCastProcessor &processorRef;

  juce::TextButton recButton{juce::String::fromUTF8("● REC")};
  juce::TextButton stopButton{juce::String::fromUTF8("■ STOP")};
  juce::Label timeLabel;
  juce::Label statusLabel;
  juce::ComboBox captureModeBox;

  // 保存先フォルダ選択 UI
  juce::ComboBox pathPresetBox;
  juce::TextButton browseButton{"Browse..."};
  juce::Label pathLabel;
  std::unique_ptr<juce::FileChooser> fileChooser;

  // 出力フォーマット選択 UI
  juce::ComboBox outputFormatBox;

  /** プリセット ID (1=Movies, 2=Desktop, 3=Downloads) に対応するフォルダパス。
   */
  static juce::File pathForPreset(int presetId) noexcept;

  /** pathLabel の表示をプロセッサーの outputPath で更新する。 */
  void updatePathDisplay();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DawCastEditor)
};
