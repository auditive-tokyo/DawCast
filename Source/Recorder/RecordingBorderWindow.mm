#import "RecordingBorderWindow.h"
#import <AppKit/AppKit.h>

// ─── レイアウト定数 ───────────────────────────────────────────────────
static const CGFloat kBorderInset = 4.0;   // 枠線の太さ（録画領域の外側）
static const CGFloat kBadgeHeight = 28.0;  // REC バッジの高さ（録画領域の上側）
static const CGFloat kBadgePadding = 10.0; // バッジ内の左右パディング
static const NSTimeInterval kBlinkInterval = 0.6; // ● の点滅周期（秒）

// ─── 描画ビュー ───────────────────────────────────────────────────────
// isFlipped = YES で CG 同様に左上原点を採用。
// 描画領域:
//   - 上側 0..kBadgeHeight   : REC バッジ領域
//   - 中央 kBadgeHeight..    : 枠線（中央 sourceRect は透明・punch-through）
@interface DawCastBorderView : NSView
@property(nonatomic, assign) BOOL dotVisible;
@property(nonatomic, assign) NSRect recordingRectInView; // 録画領域（ビュー内座標）
@end

@implementation DawCastBorderView

- (BOOL)isFlipped {
  return YES;
}

// クリックを下のウィンドウに通すため、ヒットテストを常に nil 返す。
// 加えて NSWindow.ignoresMouseEvents = YES を設定するため二重防御。
- (NSView *)hitTest:(NSPoint)point {
  (void)point;
  return nil;
}

- (void)drawRect:(NSRect)dirtyRect {
  (void)dirtyRect;

  NSRect rec = self.recordingRectInView;
  if (rec.size.width < 1.0 || rec.size.height < 1.0)
    return;

  // ── 枠線（録画領域の外側を 1 px ずらして描画）──────────────────
  // NSBezierPath はパスを stroke すると線幅の半分が両側に広がるので、
  // 録画領域に半分かぶせず外側だけに乗せるため、領域を inset する。
  NSColor *red = [NSColor colorWithCalibratedRed:0.95
                                           green:0.15
                                            blue:0.15
                                           alpha:0.95];
  NSRect borderRect =
      NSInsetRect(rec, -kBorderInset * 0.5, -kBorderInset * 0.5);
  NSBezierPath *path = [NSBezierPath bezierPathWithRect:borderRect];
  path.lineWidth = kBorderInset;
  [red setStroke];
  [path stroke];

  // ── REC バッジ（録画領域の上側、外側に配置）────────────────────
  NSDictionary *textAttrs = @{
    NSFontAttributeName : [NSFont boldSystemFontOfSize:13.0],
    NSForegroundColorAttributeName : [NSColor whiteColor],
  };
  NSString *label = @"REC";
  NSSize labelSize = [label sizeWithAttributes:textAttrs];

  // バッジサイズ: ● + space + REC
  CGFloat dotDiam = labelSize.height * 0.7;
  CGFloat innerSpacing = 6.0;
  CGFloat badgeWidth =
      dotDiam + innerSpacing + labelSize.width + kBadgePadding * 2.0;
  CGFloat badgeHeight = kBadgeHeight - 4.0; // 上下に 2px 余白

  NSRect badge = NSMakeRect(rec.origin.x, rec.origin.y - kBadgeHeight,
                            badgeWidth, badgeHeight);

  // 角丸黒背景
  NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:badge
                                                     xRadius:4.0
                                                     yRadius:4.0];
  [[NSColor colorWithCalibratedWhite:0.0 alpha:0.85] setFill];
  [bg fill];

  // 点滅する赤丸
  if (self.dotVisible) {
    CGFloat dotX = badge.origin.x + kBadgePadding;
    CGFloat dotY = badge.origin.y + (badgeHeight - dotDiam) * 0.5;
    NSRect dotRect = NSMakeRect(dotX, dotY, dotDiam, dotDiam);
    NSBezierPath *dot = [NSBezierPath bezierPathWithOvalInRect:dotRect];
    [red setFill];
    [dot fill];
  }

  // "REC" テキスト
  CGFloat textX =
      badge.origin.x + kBadgePadding + dotDiam + innerSpacing;
  CGFloat textY = badge.origin.y + (badgeHeight - labelSize.height) * 0.5;
  [label drawAtPoint:NSMakePoint(textX, textY) withAttributes:textAttrs];
}

@end

// ─── Impl ─────────────────────────────────────────────────────────────

struct RecordingBorderWindow::Impl {
  NSWindow *window;
  DawCastBorderView *view;
  NSTimer *blinkTimer;
  Impl() : window(nil), view(nil), blinkTimer(nil) {}
};

// ─── RecordingBorderWindow ────────────────────────────────────────────

RecordingBorderWindow::RecordingBorderWindow()
    : impl(new Impl()) {}

RecordingBorderWindow::~RecordingBorderWindow() { hide(); }

void RecordingBorderWindow::show(int x, int y, int width, int height) {
  hide(); // 既存があれば閉じる

  if (width <= 0 || height <= 0)
    return;

  NSScreen *screen = [NSScreen mainScreen];
  if (!screen)
    return;
  NSRect screenFrame = screen.frame;

  // CG 系（左上原点）→ AppKit 系（左下原点）変換。
  // ウィンドウは録画領域 + 全周 inset + 上側 badgeHeight を覆う。
  CGFloat winOriginX_cg = (CGFloat)x - kBorderInset;
  CGFloat winOriginY_cg = (CGFloat)y - kBadgeHeight;
  CGFloat winWidth = (CGFloat)width + kBorderInset * 2.0;
  CGFloat winHeight = (CGFloat)height + kBadgeHeight + kBorderInset;

  NSRect winFrame =
      NSMakeRect(winOriginX_cg,
                 screenFrame.size.height - winOriginY_cg - winHeight, winWidth,
                 winHeight);

  NSWindow *win = [[NSWindow alloc] initWithContentRect:winFrame
                                              styleMask:NSWindowStyleMaskBorderless
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
  win.opaque = NO;
  win.hasShadow = NO;
  win.backgroundColor = [NSColor clearColor];
  win.level = NSStatusWindowLevel; // 通常ウィンドウより上、メニューバー未満
  win.ignoresMouseEvents = YES;    // 下のアプリにマウスを通す
  win.releasedWhenClosed = NO;
  // 全 Space で表示し続ける
  win.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                           NSWindowCollectionBehaviorStationary |
                           NSWindowCollectionBehaviorIgnoresCycle;

  DawCastBorderView *view = [[DawCastBorderView alloc]
      initWithFrame:NSMakeRect(0, 0, winWidth, winHeight)];
  view.dotVisible = YES;
  // ビュー内座標における録画領域（左上原点・isFlipped=YES）
  view.recordingRectInView =
      NSMakeRect(kBorderInset, kBadgeHeight, (CGFloat)width, (CGFloat)height);

  [win setContentView:view];
  [win orderFrontRegardless]; // フォーカスを奪わずに表示

  // 点滅タイマー（メインスレッド）
  __weak DawCastBorderView *weakView = view;
  NSTimer *timer = [NSTimer
      scheduledTimerWithTimeInterval:kBlinkInterval
                             repeats:YES
                               block:^(NSTimer *t) {
                                 (void)t;
                                 DawCastBorderView *strong = weakView;
                                 if (!strong) return;
                                 strong.dotVisible = !strong.dotVisible;
                                 [strong setNeedsDisplay:YES];
                               }];
  // モーダルパネル中もタイマーを継続させる
  [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];

  impl->window = win;
  impl->view = view;
  impl->blinkTimer = timer;
}

void RecordingBorderWindow::hide() {
  if (impl->blinkTimer) {
    [impl->blinkTimer invalidate];
    impl->blinkTimer = nil;
  }
  if (impl->window) {
    [impl->window orderOut:nil];
    impl->window = nil;
    impl->view = nil;
  }
}
