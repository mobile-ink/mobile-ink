import UIKit
import MetalKit
import React

// Container view that forwards properties to drawing view and background view
class DrawingContainerView: UIView, UIPencilInteractionDelegate {
  private var drawingView: MobileInkCanvasView?
  private var backgroundView: MobileInkBackgroundView?

  override init(frame: CGRect) {
    super.init(frame: frame)

    if #available(iOS 12.1, *) {
      let pencilInteraction = UIPencilInteraction()
      pencilInteraction.delegate = self
      addInteraction(pencilInteraction)
    }
  }

  required init?(coder: NSCoder) {
    fatalError("init(coder:) has not been implemented")
  }

  override func layoutSubviews() {
    super.layoutSubviews()
    backgroundView?.setNeedsDisplay()
    drawingView?.requestDisplay(forceWhenSuspended: true)
  }

  // Event handlers - forward to drawing view
  @objc var onDrawingChange: RCTDirectEventBlock? {
    didSet {
      drawingView?.onDrawingChange = onDrawingChange
    }
  }

  @objc var onDrawingBegin: RCTDirectEventBlock? {
    didSet {
      drawingView?.onDrawingBegin = onDrawingBegin
    }
  }

  @objc var onInkSelectionChange: RCTDirectEventBlock? {
    didSet {
      drawingView?.onInkSelectionChange = onInkSelectionChange
    }
  }

  @objc var onPencilDoubleTap: RCTDirectEventBlock? {
    didSet {
      drawingView?.onPencilDoubleTap = onPencilDoubleTap
    }
  }

  // Background properties - forward to background view AND drawing view engine
  @objc var backgroundType: String? {
    didSet {
      if let type = backgroundType {
        // Update MobileInkBackgroundView (renders the actual background)
        backgroundView?.backgroundType = type
        backgroundView?.setNeedsDisplay()
        // Update MobileInkCanvasView's C++ engine (so it clears with transparent for PDF)
        drawingView?.setEngineBackgroundType(type)
      }
    }
  }

  @objc var pdfBackgroundUri: String? {
    didSet {
      print("[Container] pdfBackgroundUri set: \(pdfBackgroundUri?.prefix(80) ?? "nil"), backgroundView: \(backgroundView != nil ? "exists" : "nil")")
      if let bgView = backgroundView {
        bgView.loadPDFBackground(uri: pdfBackgroundUri)
        bgView.setNeedsDisplay()
      }
    }
  }

  @objc var renderSuspended: Bool = false {
    didSet {
      drawingView?.renderSuspended = renderSuspended
    }
  }

  @objc var renderBackend: String? {
    didSet {
      if let backend = renderBackend {
        drawingView?.renderBackend = backend
      }
    }
  }

  // Drawing policy - controls whether fingers or only Apple Pencil can draw
  @objc var drawingPolicy: String? {
    didSet {
      if let policy = drawingPolicy {
        drawingView?.drawingPolicy = policy
      }
    }
  }

  func setDrawingView(_ view: MobileInkCanvasView) {
    self.drawingView = view
    // Forward any properties that were set before the drawing view was added
    if let handler = onDrawingChange {
      view.onDrawingChange = handler
    }
    if let handler = onDrawingBegin {
      view.onDrawingBegin = handler
    }
    if let handler = onInkSelectionChange {
      view.onInkSelectionChange = handler
    }
    if let handler = onPencilDoubleTap {
      view.onPencilDoubleTap = handler
    }
    // Forward background type to engine (so it knows to clear with transparent for PDF)
    if let type = backgroundType {
      view.setEngineBackgroundType(type)
    }
    // Forward drawing policy
    if let policy = drawingPolicy {
      view.drawingPolicy = policy
    }
    if let backend = renderBackend {
      view.renderBackend = backend
    }
    view.renderSuspended = renderSuspended
  }

  func setMobileInkBackgroundView(_ view: MobileInkBackgroundView) {
    self.backgroundView = view
    // Forward any properties that were set before the background view was added
    if let type = backgroundType {
      view.backgroundType = type
      view.setNeedsDisplay()
    }
    if let uri = pdfBackgroundUri {
      view.loadPDFBackground(uri: uri)
      view.setNeedsDisplay()
    }
  }

  @available(iOS 12.1, *)
  func pencilInteractionDidTap(_ interaction: UIPencilInteraction) {
    drawingView?.simulatePencilDoubleTapForTesting()
  }

  @available(iOS 17.5, *)
  func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveTap tap: UIPencilInteraction.Tap) {
    drawingView?.simulatePencilDoubleTapForTesting()
  }
}

@objc(MobileInkCanvasViewManager)
class MobileInkCanvasViewManager: RCTViewManager {

  override func view() -> UIView! {
    // Create container view
    let containerView = DrawingContainerView(frame: .zero)
    containerView.clipsToBounds = true

    // Create and add background view (opaque, underneath)
    let backgroundView = MobileInkBackgroundView(frame: .zero)
    backgroundView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    containerView.addSubview(backgroundView)

    // Create and add drawing view on top (transparent Metal view)
    let drawingView = MobileInkCanvasView(frame: .zero, device: MTLCreateSystemDefaultDevice())
    drawingView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    drawingView.backgroundView = backgroundView // Set reference for export
    containerView.addSubview(drawingView)

    // Set up property forwarding
    containerView.setMobileInkBackgroundView(backgroundView)
    containerView.setDrawingView(drawingView)

    return containerView
  }

  override class func requiresMainQueueSetup() -> Bool {
    return true
  }

  // Helper to find MobileInkCanvasView in container
  private func findDrawingView(_ container: UIView) -> MobileInkCanvasView? {
    for subview in container.subviews {
      if let drawingView = subview as? MobileInkCanvasView {
        return drawingView
      }
    }
    return nil
  }

  // MARK: - Exposed Methods

  @objc func clear(_ node: NSNumber) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.clear()
      }
    }
  }

  @objc func undo(_ node: NSNumber) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.undo()
      }
    }
  }

  @objc func redo(_ node: NSNumber) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.redo()
      }
    }
  }

  @objc func setTool(_ node: NSNumber, toolType: String, width: CGFloat, color: String, eraserMode: String) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        let uiColor = UIColor(hex: color) ?? .black
        view.setTool(toolType, width: Float(width), color: uiColor, eraserMode: eraserMode)
      }
    }
  }

  @objc func getBase64Data(_ node: NSNumber, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.getBase64Data(callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func loadBase64Data(_ node: NSNumber, base64String: String, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.loadBase64Data(base64String, callback: callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func stageBase64Data(_ node: NSNumber, base64String: String, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.stageBase64Data(base64String, callback: callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func presentDeferredLoad(_ node: NSNumber, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.presentDeferredLoad(callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func getBase64PngData(_ node: NSNumber, scale: CGFloat, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.getBase64PngData(scale, callback: callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func getBase64JpegData(_ node: NSNumber, scale: CGFloat, compression: CGFloat, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.getBase64JpegData(scale, compression: compression, callback: callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  // Selection operations
  @objc func performCopy(_ node: NSNumber) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.performCopy()
      }
    }
  }

  @objc func performPaste(_ node: NSNumber) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.performPaste()
      }
    }
  }

  @objc func performDelete(_ node: NSNumber) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.performDelete()
      }
    }
  }

  @objc func simulatePencilDoubleTap(_ node: NSNumber, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.simulatePencilDoubleTapForTesting()
        callback([NSNull(), true])
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func runBenchmark(_ node: NSNumber, options: NSDictionary, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.runBenchmark(options, callback: callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func startBenchmarkRecording(_ node: NSNumber, options: NSDictionary, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.startBenchmarkRecording(options, callback: callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  @objc func stopBenchmarkRecording(_ node: NSNumber, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.stopBenchmarkRecording(callback)
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }

  /// Explicit eager-release call from JS. The slot's React unmount path
  /// invokes this via the bridge before letting React tear down the view,
  /// so the heavy native state (13 MiB pixel buffer, multi-MB engine
  /// strokes + history) is reclaimed deterministically. Necessary because
  /// something in the RN/MTKView chain holds the view past unmount and
  /// neither deinit nor didMoveToWindow fires reliably for every leaked
  /// instance -- Instruments confirmed pixel buffers kept piling up
  /// across scrolls even with the didMoveToWindow path in place.
  @objc func releaseEngine(_ node: NSNumber, callback: @escaping RCTResponseSenderBlock) {
    DispatchQueue.main.async {
      if let container = self.bridge.uiManager.view(forReactTag: node),
         let view = self.findDrawingView(container) {
        view.releaseHeavyNativeState()
        callback([NSNull(), true])
      } else {
        callback(["View not found", NSNull()])
      }
    }
  }
}

// Helper extension for hex color conversion
extension UIColor {
  convenience init?(hex: String) {
    var hexSanitized = hex.trimmingCharacters(in: .whitespacesAndNewlines)
    hexSanitized = hexSanitized.replacingOccurrences(of: "#", with: "")

    var rgb: UInt64 = 0

    guard Scanner(string: hexSanitized).scanHexInt64(&rgb) else { return nil }

    if hexSanitized.count == 6 {
      self.init(
        red: CGFloat((rgb & 0xFF0000) >> 16) / 255.0,
        green: CGFloat((rgb & 0x00FF00) >> 8) / 255.0,
        blue: CGFloat(rgb & 0x0000FF) / 255.0,
        alpha: 1.0
      )
    } else if hexSanitized.count == 8 {
      self.init(
        red: CGFloat((rgb & 0xFF000000) >> 24) / 255.0,
        green: CGFloat((rgb & 0x00FF0000) >> 16) / 255.0,
        blue: CGFloat((rgb & 0x0000FF00) >> 8) / 255.0,
        alpha: CGFloat(rgb & 0x000000FF) / 255.0
      )
    } else {
      return nil
    }
  }
}
