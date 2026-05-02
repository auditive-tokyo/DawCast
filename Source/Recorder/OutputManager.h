#pragma once
#include <juce_core/juce_core.h>

/**
 * OutputManager
 *
 * 録画ファイルの保存先・命名規則を管理する。
 * デフォルト保存先: ~/Movies/DawCast/
 */
class OutputManager {
public:
  OutputManager();

  /** 録画ファイルの保存先ディレクトリを設定する。 */
  void setOutputDirectory(const juce::File &dir);

  juce::File getOutputDirectory() const noexcept;

  /**
   * 次の録画ファイルパスを生成して返す。
   * 例: ~/Movies/DawCast/DawCast_2026-05-01_143022.mp4
   */
  juce::File generateOutputFile(bool proRes = false) const;

private:
  juce::File outputDirectory;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputManager)
};
