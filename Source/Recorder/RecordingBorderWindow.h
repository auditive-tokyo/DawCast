#pragma once

#ifdef __cplusplus
#include <memory>

/**
 * RecordingBorderWindow
 *
 * 録画中に対象領域の周囲を赤枠で囲み、左上に点滅する「● REC」インジケータを
 * 表示する半透明オーバーレイウィンドウ（macOS / Objective-C++ / ARC 実装）。
 *
 * 枠線とインジケータは SCStreamConfiguration.sourceRect の **外側** に
 * 配置されるため、録画ファイルには映り込まない。
 *
 * 座標は CG 系（左上原点・ポイント単位）で受け取る。
 * SCStreamConfiguration.sourceRect と同じ座標系。
 */
class RecordingBorderWindow {
public:
  RecordingBorderWindow();
  ~RecordingBorderWindow();

  /**
   * 録画領域の周囲に枠を表示する。
   * x/y/width/height は録画される領域そのもの（CG 系・ポイント単位）。
   * 枠線とインジケータはこの領域の外側に描画される。
   */
  void show(int x, int y, int width, int height);

  /** 枠を非表示にする。 */
  void hide();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

#endif // __cplusplus
