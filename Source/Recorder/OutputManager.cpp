#include "OutputManager.h"

OutputManager::OutputManager() {
  outputDirectory =
      juce::File::getSpecialLocation(juce::File::userMoviesDirectory)
          .getChildFile("DawCast");
  outputDirectory.createDirectory();
}

void OutputManager::setOutputDirectory(const juce::File &dir) {
  outputDirectory = dir;
  outputDirectory.createDirectory();
}

juce::File OutputManager::getOutputDirectory() const noexcept {
  return outputDirectory;
}

juce::File OutputManager::generateOutputFile(bool proRes) const {
  const juce::Time now = juce::Time::getCurrentTime();
  const juce::String timestamp = juce::String::formatted(
      "%04d-%02d-%02d_%02d%02d%02d", now.getYear(), now.getMonth() + 1,
      now.getDayOfMonth(), now.getHours(), now.getMinutes(), now.getSeconds());

  const juce::String ext = proRes ? "mov" : "mp4";
  return outputDirectory.getChildFile("DawCast_" + timestamp + "." + ext);
}
