#import <React/RCTViewManager.h>

@interface RCT_EXTERN_MODULE(MobileInkBackgroundViewManager, RCTViewManager)

RCT_EXPORT_VIEW_PROPERTY(backgroundType, NSString)
RCT_EXPORT_VIEW_PROPERTY(pdfBackgroundUri, NSString)

@end
