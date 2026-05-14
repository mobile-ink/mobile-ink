#import <React/RCTViewManager.h>
#import <React/RCTBridgeModule.h>

@interface RCT_EXTERN_MODULE(MobileInkCanvasViewManager, RCTViewManager)

RCT_EXPORT_VIEW_PROPERTY(onDrawingChange, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onDrawingBegin, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onInkSelectionChange, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(onPencilDoubleTap, RCTDirectEventBlock)
RCT_EXPORT_VIEW_PROPERTY(backgroundType, NSString)
RCT_EXPORT_VIEW_PROPERTY(pdfBackgroundUri, NSString)
RCT_EXPORT_VIEW_PROPERTY(renderSuspended, BOOL)
RCT_EXPORT_VIEW_PROPERTY(renderBackend, NSString)
RCT_EXPORT_VIEW_PROPERTY(drawingPolicy, NSString)

RCT_EXTERN_METHOD(clear:(nonnull NSNumber *)node)
RCT_EXTERN_METHOD(undo:(nonnull NSNumber *)node)
RCT_EXTERN_METHOD(redo:(nonnull NSNumber *)node)

RCT_EXTERN_METHOD(setTool:(nonnull NSNumber *)node
                  toolType:(NSString *)toolType
                  width:(CGFloat)width
                  color:(NSString *)color
                  eraserMode:(NSString *)eraserMode)

RCT_EXTERN_METHOD(getBase64Data:(nonnull NSNumber *)node
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(loadBase64Data:(nonnull NSNumber *)node
                  base64String:(NSString *)base64String
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(getBase64PngData:(nonnull NSNumber *)node
                  scale:(CGFloat)scale
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(getBase64JpegData:(nonnull NSNumber *)node
                  scale:(CGFloat)scale
                  compression:(CGFloat)compression
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(performCopy:(nonnull NSNumber *)node)
RCT_EXTERN_METHOD(performPaste:(nonnull NSNumber *)node)
RCT_EXTERN_METHOD(performDelete:(nonnull NSNumber *)node)
RCT_EXTERN_METHOD(simulatePencilDoubleTap:(nonnull NSNumber *)node
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(runBenchmark:(nonnull NSNumber *)node
                  options:(NSDictionary *)options
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(startBenchmarkRecording:(nonnull NSNumber *)node
                  options:(NSDictionary *)options
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(stopBenchmarkRecording:(nonnull NSNumber *)node
                  callback:(RCTResponseSenderBlock)callback)

RCT_EXTERN_METHOD(releaseEngine:(nonnull NSNumber *)node
                  callback:(RCTResponseSenderBlock)callback)

@end
