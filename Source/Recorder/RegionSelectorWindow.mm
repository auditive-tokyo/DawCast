#import "RegionSelectorWindow.h"
#import <AppKit/AppKit.h>
#import <functional>
#import <juce_events/juce_events.h>

// ─── スナップターゲット ───────────────────────────────────────────────
struct SnapTarget {
  double ratioWH;    // 幅/高さ
  const char *label; // 表示ラベル
};
static const SnapTarget kSnapTargets[] = {
    {16.0 / 9.0, "16:9"},
    {1.0, "1:1"},
    {9.0 / 16.0, "9:16"},
};
static const NSUInteger kSnapTargetCount = 3;
static const double kSnapThreshold = 0.10; // ±10% の相対許容誤差

// ─── NSWindow サブクラス ─────────────────────────────────────────────
// NSWindowStyleMaskBorderless はデフォルト canBecomeKeyWindow = NO のため
// View がキーイベントを受け取れない。サブクラスでオーバーライドする。
@interface DawCastSelectionWindow : NSWindow
@end
@implementation DawCastSelectionWindow
- (BOOL)canBecomeKeyWindow {
  return YES;
}
- (BOOL)canBecomeMainWindow {
  return NO;
}
@end

// ─── Selection View ──────────────────────────────────────────────────
@interface DawCastSelectionView : NSView
@property(nonatomic, assign) NSPoint startPt;
@property(nonatomic, assign) NSPoint endPt;
@property(nonatomic, assign) BOOL dragging;
// スナップ中の比率 (0 = スナップなし)
@property(nonatomic, assign) double snappedRatioWH;
@property(nonatomic, copy) NSString *snapLabel;
// コールバック
@property(nonatomic, copy) void (^onSelect)(NSRect);
@property(nonatomic, copy) void (^onCancel)(void);
@end

@implementation DawCastSelectionView

- (BOOL)isFlipped {
  return YES;
}
- (BOOL)acceptsFirstResponder {
  return YES;
}

// ─── ドラッグ座標から生の矩形を返す ──────────────────────────────────
- (NSRect)rawRect {
  CGFloat x = std::min(self.startPt.x, self.endPt.x);
  CGFloat y = std::min(self.startPt.y, self.endPt.y);
  CGFloat w = std::abs(self.endPt.x - self.startPt.x);
  CGFloat h = std::abs(self.endPt.y - self.startPt.y);
  return NSMakeRect(x, y, w, h);
}

// ─── スナップ適用後の矩形 ─────────────────────────────────────────────
// スナップ時: 幅を固定し、高さを比率に合わせて調整する。
// startPt から endPt へのドラッグ方向を保持しつつ origin を補正する。
- (NSRect)selectionRect {
  NSRect raw = [self rawRect];
  if (self.snappedRatioWH <= 0.0 || raw.size.width < 1.0)
    return raw;

  CGFloat w = raw.size.width;
  CGFloat h = w / self.snappedRatioWH;

  // Y 方向のドラッグ向きに合わせて origin を調整
  CGFloat originY = (self.endPt.y >= self.startPt.y)
                        ? raw.origin.y
                        : raw.origin.y + raw.size.height - h;
  return NSMakeRect(raw.origin.x, originY, w, h);
}

// ─── 描画 ─────────────────────────────────────────────────────────────
- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;

  // 半透明オーバーレイ
  [[NSColor colorWithCalibratedWhite:0.0 alpha:0.35] setFill];
  NSRectFill(self.bounds);

  if (self.dragging) {
    NSRect sel = [self selectionRect];
    BOOL hasSelection = (sel.size.width >= 2.0 && sel.size.height >= 2.0);

    if (hasSelection) {
      // punch-through (選択内は透明)
      [NSGraphicsContext currentContext].compositingOperation =
          NSCompositingOperationClear;
      [[NSColor clearColor] setFill];
      NSRectFill(sel);
      [NSGraphicsContext currentContext].compositingOperation =
          NSCompositingOperationSourceOver;

      // ボーダー: スナップ時は実線、通常は点線
      NSBezierPath *path =
          [NSBezierPath bezierPathWithRect:NSInsetRect(sel, 1.0, 1.0)];
      path.lineWidth = 2.0;
      if (self.snappedRatioWH > 0.0) {
        // スナップ中: シアン実線
        [[NSColor colorWithCalibratedRed:0.0 green:0.9 blue:0.9
                                   alpha:1.0] setStroke];
      } else {
        // 通常: 白点線
        [[NSColor whiteColor] setStroke];
        CGFloat dash[2] = {8.0, 4.0};
        [path setLineDash:dash count:2 phase:0.0];
      }
      [path stroke];

      // ラベル領域
      NSDictionary *textAttrs = @{
        NSFontAttributeName : [NSFont boldSystemFontOfSize:13],
        NSForegroundColorAttributeName : [NSColor whiteColor],
        NSBackgroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.0
                                                                     alpha:0.55]
      };

      // サイズ表示
      NSString *sizeStr = [NSString
          stringWithFormat:@"%.0f × %.0f", sel.size.width, sel.size.height];
      NSSize ts = [sizeStr sizeWithAttributes:textAttrs];
      NSPoint tp = NSMakePoint(NSMidX(sel) - ts.width * 0.5, NSMaxY(sel) + 6.0);
      if (tp.y + ts.height > self.bounds.size.height - 4.0)
        tp.y = sel.origin.y - ts.height - 6.0;
      [sizeStr drawAtPoint:tp withAttributes:textAttrs];

      // スナップ比率バッジ（枠の上部中央）
      if (self.snappedRatioWH > 0.0 && self.snapLabel) {
        NSDictionary *badgeAttrs = @{
          NSFontAttributeName : [NSFont boldSystemFontOfSize:15],
          NSForegroundColorAttributeName : [NSColor blackColor],
          NSBackgroundColorAttributeName : [NSColor colorWithCalibratedRed:0.0
                                                                     green:0.9
                                                                      blue:0.9
                                                                     alpha:1.0]
        };
        NSString *badge = [NSString stringWithFormat:@"  %@  ", self.snapLabel];
        NSSize bs = [badge sizeWithAttributes:badgeAttrs];
        NSPoint bp = NSMakePoint(NSMidX(sel) - bs.width * 0.5,
                                 sel.origin.y - bs.height - 4.0);
        if (bp.y < 4.0)
          bp.y = NSMaxY(sel) + 4.0;
        [badge drawAtPoint:bp withAttributes:badgeAttrs];
      }
    }
  }

  // ガイドメッセージ
  NSString *hint =
      @"Drag to select   |   Snap: 16:9 / 1:1 / 9:16   |   Esc to cancel";
  NSDictionary *hintAttrs = @{
    NSFontAttributeName : [NSFont systemFontOfSize:16.0],
    NSForegroundColorAttributeName : [NSColor whiteColor],
    NSBackgroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.0
                                                                 alpha:0.5]
  };
  NSSize hs = [hint sizeWithAttributes:hintAttrs];
  NSPoint hp = NSMakePoint((self.bounds.size.width - hs.width) * 0.5,
                           self.bounds.size.height - hs.height - 20.0);
  [hint drawAtPoint:hp withAttributes:hintAttrs];
}

// ─── マウスイベント ───────────────────────────────────────────────────
- (void)mouseDown:(NSEvent *)event {
  NSPoint pt = [self convertPoint:event.locationInWindow fromView:nil];
  self.startPt = pt;
  self.endPt = pt;
  self.dragging = YES;
  self.snappedRatioWH = 0.0;
  self.snapLabel = nil;
  [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)event {
  self.endPt = [self convertPoint:event.locationInWindow fromView:nil];

  // スナップ判定
  NSRect raw = [self rawRect];
  self.snappedRatioWH = 0.0;
  self.snapLabel = nil;
  if (raw.size.width >= 4.0 && raw.size.height >= 4.0) {
    const double ratio = raw.size.width / raw.size.height;
    for (NSUInteger i = 0; i < kSnapTargetCount; ++i) {
      const double target = kSnapTargets[i].ratioWH;
      if (std::abs(ratio - target) / target < kSnapThreshold) {
        self.snappedRatioWH = target;
        self.snapLabel = [NSString stringWithUTF8String:kSnapTargets[i].label];
        break;
      }
    }
  }

  [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)event {
  self.endPt = [self convertPoint:event.locationInWindow fromView:nil];
  self.dragging = NO;
  NSRect sel = [self selectionRect];
  if (sel.size.width >= 20.0 && sel.size.height >= 20.0) {
    if (self.onSelect)
      self.onSelect(sel);
  } else {
    if (self.onCancel)
      self.onCancel();
  }
}

- (void)keyDown:(NSEvent *)event {
  if (event.keyCode == 53) { // ESC
    self.dragging = NO;
    if (self.onCancel)
      self.onCancel();
  } else {
    [super keyDown:event];
  }
}

@end

// ─── Impl ─────────────────────────────────────────────────────────────

struct RegionSelectorWindow::Impl {
  DawCastSelectionWindow *window = nil;
  DawCastSelectionView *view = nil;
  CompletionCallback callback;
};

// ─── RegionSelectorWindow ─────────────────────────────────────────────

RegionSelectorWindow::RegionSelectorWindow() : impl(std::make_unique<Impl>()) {}
RegionSelectorWindow::~RegionSelectorWindow() { dismiss(); }

void RegionSelectorWindow::show(CompletionCallback callback) {
  dismiss();
  impl->callback = std::move(callback);

  NSScreen *screen = [NSScreen mainScreen];
  NSRect screenFrame = screen.frame;

  DawCastSelectionWindow *win = [[DawCastSelectionWindow alloc]
      initWithContentRect:screenFrame
                styleMask:NSWindowStyleMaskBorderless
                  backing:NSBackingStoreBuffered
                    defer:NO];
  win.opaque = NO;
  win.hasShadow = NO;
  win.backgroundColor = [NSColor clearColor];
  win.level = NSScreenSaverWindowLevel;
  win.ignoresMouseEvents = NO;
  win.releasedWhenClosed = NO;

  DawCastSelectionView *view = [[DawCastSelectionView alloc]
      initWithFrame:NSMakeRect(0, 0, screenFrame.size.width,
                               screenFrame.size.height)];

  __weak DawCastSelectionWindow *weakWin = win;
  RegionSelectorWindow::Impl *pImpl = impl.get();

  view.onSelect = ^(NSRect sel) {
    CompletionCallback cb = pImpl->callback;
    [weakWin orderOut:nil];
    // 座標を偶数にアラインメント（H.264 の制約）
    int rx = (int)sel.origin.x & ~1;
    int ry = (int)sel.origin.y & ~1;
    int rw = (int)sel.size.width & ~1;
    int rh = (int)sel.size.height & ~1;
    juce::MessageManager::callAsync([cb, rx, ry, rw, rh] {
      if (cb)
        cb(rx, ry, rw, rh);
    });
  };

  view.onCancel = ^{
    CompletionCallback cb = pImpl->callback;
    [weakWin orderOut:nil];
    juce::MessageManager::callAsync([cb] {
      if (cb)
        cb(0, 0, 0, 0);
    });
  };

  [win setContentView:view];
  [NSApp activateIgnoringOtherApps:YES];
  [win makeKeyAndOrderFront:nil];
  [win makeFirstResponder:view];

  impl->window = win;
  impl->view = view;
}

void RegionSelectorWindow::dismiss() {
  if (impl->window) {
    [impl->window orderOut:nil];
    impl->window = nil;
    impl->view = nil;
  }
  impl->callback = nullptr;
}
