#pragma once

#ifdef __cplusplus
#include <functional>
#include <memory>

/**
 * RegionSelectorWindow
 *
 * 全画面半透明オーバーレイ NSWindow を表示してユーザーに録画矩形を選ばせる。
 * macOS 専用（Objective-C++ / ARC 実装: RegionSelectorWindow.mm）。
 *
 * ドラッグ中に 16:9 / 1:1 / 9:16 に近い比率になると自動でスナップする。
 * スナップ時は枠が実線に変わり比率ラベルを表示する。
 *
 * 座標はポイント単位（SCStreamConfiguration.sourceRect と同一系）。
 * 原点はディスプレイの左上。
 */
class RegionSelectorWindow {
public:
  /**
   * 選択完了コールバック。
   * - width > 0 && height > 0: 選択完了（録画開始に進む）
   * - width == 0 || height == 0: キャンセル
   */
  using CompletionCallback =
      std::function<void(int x, int y, int width, int height)>;

  RegionSelectorWindow();
  ~RegionSelectorWindow();

  /**
   * オーバーレイを表示して矩形選択を待つ。
   * callback はメッセージスレッドで呼ばれる。
   * ドラッグ中に 16:9 / 1:1 / 9:16 に近づくと自動スナップする。
   */
  void show(CompletionCallback callback);

  /** オーバーレイを強制的に閉じる（外部からキャンセルする場合）。 */
  void dismiss();

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

#endif // __cplusplus
