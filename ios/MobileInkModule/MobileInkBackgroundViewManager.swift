import UIKit
import React

class BackgroundPreviewContainerView: UIView {
  private let backgroundView = MobileInkBackgroundView(frame: .zero)

  override init(frame: CGRect) {
    super.init(frame: frame)

    backgroundView.frame = bounds
    backgroundView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    addSubview(backgroundView)
  }

  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }

  @objc var backgroundType: String? {
    didSet {
      backgroundView.backgroundType = backgroundType ?? "plain"
      backgroundView.setNeedsDisplay()
    }
  }

  @objc var pdfBackgroundUri: String? {
    didSet {
      backgroundView.loadPDFBackground(uri: pdfBackgroundUri)
      backgroundView.setNeedsDisplay()
    }
  }
}

@objc(MobileInkBackgroundViewManager)
class MobileInkBackgroundViewManager: RCTViewManager {
  override func view() -> UIView! {
    let containerView = BackgroundPreviewContainerView(frame: .zero)
    containerView.clipsToBounds = true
    return containerView
  }

  override class func requiresMainQueueSetup() -> Bool {
    return true
  }
}
