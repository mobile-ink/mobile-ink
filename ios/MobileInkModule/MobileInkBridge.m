#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>

@interface RCT_EXTERN_MODULE(MobileInkBridge, RCTEventEmitter)

RCT_EXTERN_METHOD(getBase64Data:(nonnull NSNumber *)viewTag
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(loadBase64Data:(nonnull NSNumber *)viewTag
                  base64String:(NSString *)base64String
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

// Native-side persistence: serialize the engine to base64 and write to disk
// directly in Swift. The body string never crosses the JS<->native bridge,
// which is a major drawing-time win on heavy notebooks where the body is
// multi-MB and the bridge cost dominates per-save latency.
RCT_EXTERN_METHOD(persistEngineToFile:(nonnull NSNumber *)viewTag
                  filePath:(NSString *)filePath
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

// Inverse: read a body file directly in Swift and feed it into the engine
// without ever crossing the bridge as a JS string. Returns true on success.
RCT_EXTERN_METHOD(loadEngineFromFile:(nonnull NSNumber *)viewTag
                  filePath:(NSString *)filePath
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

// Body file native read+parse: read body from disk and JSON-parse in C++,
// return the structured result. Bypasses the multi-MB Hermes JSON.parse
// which dominates file-open latency on heavy notebooks.
RCT_EXTERN_METHOD(readBodyFileParsed:(NSString *)bodyPath
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

// Full-notebook native autosave: read existing body, replace ONLY the
// visible window's per-page data with the engine's current state, write
// atomically. Body bytes never cross the bridge. JS sends just visible
// page IDs + lightweight metadata.
RCT_EXTERN_METHOD(persistFullNotebookToFile:(nonnull NSNumber *)viewTag
                  visiblePageIds:(NSArray *)visiblePageIds
                  pagesMetadata:(NSArray *)pagesMetadata
                  originalCanvasWidth:(nullable NSNumber *)originalCanvasWidth
                  pageHeight:(CGFloat)pageHeight
                  bodyPath:(NSString *)bodyPath
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

// Full-notebook native load: parse body file in native, load the visible
// window's pages into the engine, return JUST the slim metadata array
// to JS (no per-page data field, which stays in native or on disk).
RCT_EXTERN_METHOD(loadNotebookForVisibleWindow:(nonnull NSNumber *)viewTag
                  bodyPath:(NSString *)bodyPath
                  visiblePageIds:(NSArray *)visiblePageIds
                  pageHeight:(CGFloat)pageHeight
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(getBase64PngData:(nonnull NSNumber *)viewTag
                  scale:(CGFloat)scale
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(getBase64JpegData:(nonnull NSNumber *)viewTag
                  scale:(CGFloat)scale
                  compression:(CGFloat)compression
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(batchExportPages:(NSArray *)pagesDataArray
                  backgroundTypes:(NSArray *)backgroundTypes
                  width:(NSInteger)width
                  height:(NSInteger)height
                  scale:(CGFloat)scale
                  pdfBackgroundUri:(NSString *)pdfBackgroundUri
                  pageIndices:(NSArray *)pageIndices
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(composeContinuousWindow:(NSArray *)pagePayloads
                  pageHeight:(CGFloat)pageHeight
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

RCT_EXTERN_METHOD(decomposeContinuousWindow:(NSString *)windowPayload
                  pageCount:(NSInteger)pageCount
                  pageHeight:(CGFloat)pageHeight
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

@end
