#import "ScreenRecorder.h"
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <atomic>

// ─── Impl ──────────────────────────────────────────────────────────
// .mm 内でのみ定義。ObjC クラスはこの型を直接参照しない。

@class DawCastStreamOutput;
@class DawCastStreamDelegate;

struct ScreenRecorder::Impl {
  std::atomic<bool> recording{false};
  ScreenRecorder::FrameCallback frameCallback;
  SCStream *stream = nil;                  // ARC 管理
  DawCastStreamOutput *streamOutput = nil; // ARC 管理（stream と同寿命）
  DawCastStreamDelegate *streamDelegate = nil; // ARC 管理
};

// ─── SCStreamOutput デリゲート ─────────────────────────────────────
// Impl を直接参照せず、block（ARC）経由でコールバックを受け取る。

@interface DawCastStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, copy) void (^handler)
    (CMSampleBufferRef buf, double timestamp);
@end

@implementation DawCastStreamOutput
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen)
    return;
  // SCK は Blank / Suspended / Started
  // など有効画素がないフレームを配信することがある。 SCStreamFrameInfoStatus が
  // SCFrameStatusComplete(0) または SCFrameStatusIdle(1)
  // のフレームのみエンコーダに渡す。
  NSNumber *statusNum = (__bridge NSNumber *)CMGetAttachment(
      sampleBuffer, (__bridge CFStringRef)SCStreamFrameInfoStatus, nil);
  if (statusNum != nil) {
    int status = statusNum.intValue;
    // 0=Complete, 1=Idle.
    // それ以外（2=Blank、3=Suspended、4=Started、5=Stopped）はスキップ
    if (status != 0 && status != 1)
      return;
  }
  if (self.handler) {
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    self.handler(sampleBuffer, CMTimeGetSeconds(pts));
  }
}
@end

// ─── SCStreamDelegate（エラー監視） ───────────────────────────────

@interface DawCastStreamDelegate : NSObject <SCStreamDelegate>
@property(nonatomic, copy) void (^onStop)(NSError *error);
@end

@implementation DawCastStreamDelegate
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  if (self.onStop)
    self.onStop(error);
}
@end

// ─── ScreenRecorder C++ 実装 ──────────────────────────────────────

ScreenRecorder::ScreenRecorder() : impl(std::make_unique<Impl>()) {}

ScreenRecorder::~ScreenRecorder() { stopRecording(); }

void ScreenRecorder::setFrameCallback(FrameCallback callback) {
  impl->frameCallback = std::move(callback);
}

bool ScreenRecorder::startRecording(CaptureMode mode, int fps, int width,
                                    int height, ApplicationTarget appTarget,
                                    RegionTarget region) {
  if (impl->recording.load())
    return true;

  Impl *implPtr = impl.get();

  [SCShareableContent
      getShareableContentExcludingDesktopWindows:NO
                             onScreenWindowsOnly:YES
                               completionHandler:^(SCShareableContent *content,
                                                   NSError *error) {
                                 if (error != nil ||
                                     content.displays.count == 0) {
                                   juce::Logger::writeToLog(
                                       "ScreenRecorder: SCShareableContent "
                                       "failed - "
                                       "check Screen Recording permission in "
                                       "System Settings. error=" +
                                       juce::String(
                                           error ? error.localizedDescription
                                                       .UTF8String
                                                 : "no displays found"));
                                   return;
                                 }

                                 SCDisplay *display =
                                     content.displays.firstObject;

                                 // ─── SCContentFilter を mode に応じて構築
                                 // ─────────────────────────
                                 SCContentFilter *filter = nil;

                                 if (mode ==
                                     ScreenRecorder::CaptureMode::Application) {
                                   NSString *targetBundleID = [NSString
                                       stringWithUTF8String:appTarget.bundleID
                                                                .c_str()];
                                   pid_t targetPID = (pid_t)appTarget.pid;
                                   SCRunningApplication *targetApp = nil;

                                   for (SCRunningApplication *app in content
                                            .applications) {
                                     bool matchByBundle =
                                         (targetBundleID.length > 0 &&
                                          [app.bundleIdentifier
                                              isEqualToString:targetBundleID]);
                                     bool matchByPID =
                                         (targetBundleID.length == 0 &&
                                          app.processID == targetPID);

                                     if (matchByBundle || matchByPID) {
                                       targetApp = app;
                                       break;
                                     }
                                   }

                                   if (targetApp != nil) {
                                     filter = [[SCContentFilter alloc]
                                               initWithDisplay:display
                                         includingApplications:@[ targetApp ]
                                              exceptingWindows:@[]];
                                     juce::Logger::writeToLog(
                                         "ScreenRecorder: capturing app: " +
                                         juce::String(targetApp.bundleIdentifier
                                                          .UTF8String));
                                   } else {
                                     juce::Logger::writeToLog(
                                         "ScreenRecorder: target app not found "
                                         "(bundleID='" +
                                         juce::String(
                                             appTarget.bundleID.c_str()) +
                                         "' pid=" +
                                         juce::String(appTarget.pid) +
                                         "), falling back to EntireDisplay");
                                     filter = [[SCContentFilter alloc]
                                          initWithDisplay:display
                                         excludingWindows:@[]];
                                   }
                                 } else {
                                   // EntireDisplay / CustomRegion
                                   // どちらも全画面フィルタ CustomRegion
                                   // はこの後 config.sourceRect
                                   // でトリミングする
                                   filter = [[SCContentFilter alloc]
                                        initWithDisplay:display
                                       excludingWindows:@[]];
                                 }

                                 SCStreamConfiguration *config =
                                     [[SCStreamConfiguration alloc] init];
                                 // display.width/height は論理ピクセル（ポイント）。
                                 // Retina では backingScaleFactor=2 なので物理ピクセルに変換する。
                                 CGFloat scale = 1.0;
                                 for (NSScreen *screen in [NSScreen screens]) {
                                   NSNumber *sid = screen.deviceDescription[@"NSScreenNumber"];
                                   if (sid && (CGDirectDisplayID)sid.unsignedIntValue == display.displayID) {
                                     scale = screen.backingScaleFactor;
                                     break;
                                   }
                                 }
                                 config.width = (width > 0)
                                                    ? (size_t)width
                                                    : (size_t)(display.width * scale);
                                 config.height = (height > 0)
                                                     ? (size_t)height
                                                     : (size_t)(display.height * scale);
                                 config.minimumFrameInterval =
                                     CMTimeMake(1, fps > 0 ? fps : 60);
                                 config.pixelFormat = kCVPixelFormatType_32BGRA;
                                 config.showsCursor = YES;

                                 // CustomRegion:
                                 // SCStreamConfiguration.sourceRect
                                 // でトリミング（macOS 14.2+） UI 選択フローは
                                 // RecorderApp 統合後に実装予定
                                 if (mode == ScreenRecorder::CaptureMode::
                                                 CustomRegion &&
                                     region.width > 0 && region.height > 0) {
                                   if (@available(macOS 14.2, *)) {
                                     config.sourceRect = CGRectMake(
                                         region.x, region.y, region.width,
                                         region.height);
                                     if (width == 0)
                                       config.width = (size_t)(region.width * scale);
                                     if (height == 0)
                                       config.height = (size_t)(region.height * scale);
                                     juce::Logger::writeToLog(
                                         "ScreenRecorder: custom region (" +
                                         juce::String(region.x) + "," +
                                         juce::String(region.y) + " " +
                                         juce::String(region.width) + "x" +
                                         juce::String(region.height) + ")");
                                   } else {
                                     juce::Logger::writeToLog(
                                         "ScreenRecorder: CustomRegion "
                                         "requires macOS 14.2+,"
                                         " falling back to full display");
                                   }
                                 }

                                 // フレームコールバックを ObjC block
                                 // に変換（Impl へのアクセスは block の中だけ）
                                 // output / delegate を Impl に保持 → ARC
                                 // による早期解放を防ぐ
                                 DawCastStreamOutput *output =
                                     [[DawCastStreamOutput alloc] init];
                                 output.handler =
                                     ^(CMSampleBufferRef buf, double ts) {
                                       if (!implPtr->recording.load())
                                         return;
                                       if (implPtr->frameCallback)
                                         implPtr->frameCallback(buf, ts);
                                     };
                                 implPtr->streamOutput = output;

                                 DawCastStreamDelegate *delegate =
                                     [[DawCastStreamDelegate alloc] init];
                                 delegate.onStop = ^(NSError *stopError) {
                                   juce::Logger::writeToLog(
                                       "ScreenRecorder: stream stopped" +
                                       juce::String(
                                           stopError
                                               ? " with error: " +
                                                     juce::String(
                                                         stopError
                                                             .localizedDescription
                                                             .UTF8String)
                                               : ""));
                                   implPtr->recording.store(false);
                                   implPtr->stream = nil;
                                   implPtr->streamOutput = nil;
                                   implPtr->streamDelegate = nil;
                                 };
                                 implPtr->streamDelegate = delegate;

                                 SCStream *stream =
                                     [[SCStream alloc] initWithFilter:filter
                                                        configuration:config
                                                             delegate:delegate];

                                 NSError *addErr = nil;
                                 BOOL added = [stream
                                        addStreamOutput:output
                                                   type:SCStreamOutputTypeScreen
                                     sampleHandlerQueue:
                                         dispatch_get_global_queue(
                                             QOS_CLASS_USER_INTERACTIVE, 0)
                                                  error:&addErr];
                                 if (!added || addErr != nil) {
                                   juce::Logger::writeToLog(
                                       "ScreenRecorder: addStreamOutput failed "
                                       "- " +
                                       juce::String(addErr.localizedDescription
                                                        .UTF8String));
                                   return;
                                 }

                                 implPtr->stream = stream;

                                 [stream startCaptureWithCompletionHandler:^(
                                             NSError *startError) {
                                   if (startError != nil) {
                                     juce::Logger::writeToLog(
                                         "ScreenRecorder: startCapture failed "
                                         "- " +
                                         juce::String(
                                             startError.localizedDescription
                                                 .UTF8String));
                                     implPtr->recording.store(false);
                                     implPtr->stream = nil;
                                   } else {
                                     juce::Logger::writeToLog(
                                         "ScreenRecorder: capture started (" +
                                         juce::String((int)config.width) + "x" +
                                         juce::String((int)config.height) +
                                         " @" + juce::String(fps) + "fps)");
                                     implPtr->recording.store(true);
                                   }
                                 }];
                               }];

  return true; // 非同期開始中。isRecording() == true になれば完了
}

void ScreenRecorder::stopRecording() {
  if (!impl->recording.load())
    return;

  SCStream *stream = impl->stream;
  Impl *implPtr = impl.get();

  [stream stopCaptureWithCompletionHandler:^(NSError *error) {
    if (error != nil) {
      juce::Logger::writeToLog(
          "ScreenRecorder: stopCapture error - " +
          juce::String(error.localizedDescription.UTF8String));
    }
    juce::Logger::writeToLog("ScreenRecorder: capture stopped");
    implPtr->recording.store(false);
    implPtr->stream = nil;
    implPtr->streamOutput = nil;
    implPtr->streamDelegate = nil;
  }];
}

bool ScreenRecorder::isRecording() const noexcept {
  return impl->recording.load();
}

bool ScreenRecorder::hasScreenRecordingPermission() noexcept {
  // CGRequestScreenCaptureAccess() は:
  //   YES  = 既に権限あり
  //   NO   = 権限なし。未決なら許可ダイアログを表示、
  //          拒否済みなら System Settings へ誘導する（ブロックしない）
  // CGPreflightScreenCaptureAccess() はダイアログを出さないため使用しない。
  return CGRequestScreenCaptureAccess() == YES;
}
