import UIKit
import MetalKit
import React

enum PencilStrokeInputNormalizer {
  static let perpendicularAltitude = Float.pi / 2
  private static let minimumPencilPressure: Float = 0.015
  private static let pencilPressureExponent: Float = 0.88

  static func isPencil(_ touch: UITouch) -> Bool {
    touch.type == .pencil
  }

  static func normalizePressure(rawNormalizedPressure: Float, isPencil: Bool) -> Float {
    if isPencil {
      let clamped = max(0.0, min(1.0, rawNormalizedPressure.isFinite ? rawNormalizedPressure : 1.0))
      return max(minimumPencilPressure, powf(clamped, pencilPressureExponent))
    }

    return max(0.1, rawNormalizedPressure)
  }

  static func normalizeAltitude(_ rawAltitude: Float, isPencil: Bool) -> Float {
    guard isPencil else {
      return perpendicularAltitude
    }

    return max(0.0, min(perpendicularAltitude, rawAltitude))
  }

  static func normalizedPressure(for touch: UITouch) -> Float {
    let maxForce = touch.maximumPossibleForce
    let rawNormalizedPressure: Float
    if maxForce > 0 {
      rawNormalizedPressure = Float(touch.force / maxForce)
    } else {
      rawNormalizedPressure = 1.0
    }

    return normalizePressure(rawNormalizedPressure: rawNormalizedPressure, isPencil: isPencil(touch))
  }

  static func azimuthAndAltitude(for touch: UITouch, in view: UIView) -> (azimuth: Float, altitude: Float) {
    guard isPencil(touch) else {
      return (0.0, perpendicularAltitude)
    }

    let altitude = normalizeAltitude(Float(touch.altitudeAngle), isPencil: true)
    let azimuthAngle = Float(touch.azimuthAngle(in: view))
    return (azimuthAngle, altitude)
  }
}

// MARK: - MobileInkCanvasView

@objc(MobileInkCanvasView)
class MobileInkCanvasView: MTKView {
    var drawingEngine: OpaquePointer?
    private var commandQueue: MTLCommandQueue?
    private var ganeshMetalContext: OpaquePointer?
    var requestedRenderBackend: MobileInkRenderBackend = .ganesh
    var benchmarkRecorder = MobileInkBenchmarkRecorder()
    var isBenchmarkReplayRunning = false
    var benchmarkRunToken = UUID()
    private var useExperimentalGaneshBackend: Bool {
        requestedRenderBackend == .ganesh
    }
    var scaleX: CGFloat = 1.0
    var scaleY: CGFloat = 1.0
  private var pixelBuffer: UnsafeMutablePointer<UInt8>?
  private var pixelBufferLength: Int = 0
  private var pixelBytesPerRow: Int = 0
  var pixelWidth: Int32 = 0
  var pixelHeight: Int32 = 0
    private var enginePixelWidth: Int32 = 0
    private var enginePixelHeight: Int32 = 0

    // Store pending tool settings to apply when engine is ready
    private var pendingTool: String = "pen"
    private var pendingWidth: Float = 3.0
    private var pendingColor: UInt32 = 0xFF000000  // Black
    private var pendingEraserMode: String = "pixel"
    private var pendingBackgroundType: String = "plain"  // For PDF transparency
    private let holdToShapeDelay: TimeInterval = 0.30
    private let holdToShapeRetryDelay: TimeInterval = 0.06
    private var holdToShapeTimer: Timer?
    var isHoldToShapeStrokeActive = false

    // Eraser cursor
    private var eraserCursorLayer: CAShapeLayer?
    private var currentTouchLocation: CGPoint?

    // Selection state
    private var isSelectionMode: Bool = false
    private var isDraggingSelection: Bool = false
    private var isMovingSelection: Bool = false
    private var isTransformingSelection: Bool = false
    private var selectionTransformHandleIndex: Int32 = -1
    private var hasSelectionMoveDelta: Bool = false
    private var lastDragPoint: CGPoint?
    private let selectionToolbar = UIStackView()
    private let selectionToolbarHeight: CGFloat = 44
    private let selectionToolbarButtonWidth: CGFloat = 48
    private let selectionToolbarMargin: CGFloat = 8

    // Text mode state
    private var isTextMode: Bool = false

    // Drawing policy: "default", "anyinput", or "pencilonly"
    // When "pencilonly", only Apple Pencil touches are processed for drawing
    @objc var drawingPolicy: String = "default"
    @objc var renderBackend: String = MobileInkRenderBackend.ganesh.rawValue {
        didSet {
            let normalizedBackend = MobileInkRenderBackend(rawValue: renderBackend.lowercased()) ?? .ganesh
            requestedRenderBackend = normalizedBackend

            if normalizedBackend == .cpu {
                if pixelWidth > 0 && pixelHeight > 0 {
                    allocatePixelBuffer(width: Int(pixelWidth), height: Int(pixelHeight))
                }
            } else {
                pixelBuffer?.deallocate()
                pixelBuffer = nil
                pixelBufferLength = 0
                pixelBytesPerRow = 0
            }

            requestDisplay(forceWhenSuspended: true)
        }
    }
    @objc var renderSuspended: Bool = false {
        didSet {
            if oldValue != renderSuspended && !renderSuspended {
                requestDisplay(forceWhenSuspended: true)
            }
        }
    }

    @objc var onDrawingChange: RCTDirectEventBlock?
    @objc var onDrawingBegin: RCTDirectEventBlock?
    @objc var onInkSelectionChange: RCTDirectEventBlock?
    @objc var onPencilDoubleTap: RCTDirectEventBlock?
    private var pencilDoubleTapSequence: Int = 0
    private var pendingPresentedLoadCallbacks: [RCTResponseSenderBlock] = []
    private var hasDeferredPresentation = false
    private var deferredBase64Payload: String?

    // Reference to background view for compositing during export
    weak var backgroundView: MobileInkBackgroundView?

    private func shouldIgnoreTouchForDrawingPolicy(_ touch: UITouch) -> Bool {
        // Selection is deliberately finger-addressable even when drawing
        // is Apple-Pencil-only. Otherwise users can draw with Pencil but
        // are forced to tap selected shapes/strokes with Pencil too.
        drawingPolicy == "pencilonly" && touch.type != .pencil && !isSelectionMode && !isMovingSelection && !isTransformingSelection
    }

    // Page dimensions (like PencilKit's pageWidth/pageHeight)
    // These are calculated from bounds.width to fit the screen
    private var pageWidth: CGFloat = 0
    private var pageHeight: CGFloat = 0

    override init(frame: CGRect, device: MTLDevice?) {
        let metalDevice = device ?? MTLCreateSystemDefaultDevice()
        super.init(frame: frame, device: metalDevice)

        self.isOpaque = false // Transparent to show background underneath
        self.backgroundColor = .clear
        self.framebufferOnly = false
        self.enableSetNeedsDisplay = true
        self.isPaused = true
        self.preferredFramesPerSecond = 120

        // Configure pixel format
        self.colorPixelFormat = .bgra8Unorm
        self.clearColor = MTLClearColorMake(0, 0, 0, 0) // Transparent

        // Disable automatic resize to have more control
        self.autoResizeDrawable = true

        // Configure Metal layer for transparency and low latency
        if let metalLayer = self.layer as? CAMetalLayer {
            metalLayer.isOpaque = false // Transparent to composite over background
            metalLayer.pixelFormat = .bgra8Unorm
            metalLayer.framebufferOnly = false
            // Allow 3 drawables for smooth triple-buffering without blocking
            metalLayer.maximumDrawableCount = 3
            // Don't synchronize with Core Animation - present immediately for lowest latency
            metalLayer.presentsWithTransaction = false
        }

        print("🔧 Metal layer configured: isOpaque=\(self.layer.isOpaque), pixelFormat=\(self.colorPixelFormat.rawValue), backgroundColor=\(String(describing: self.backgroundColor))")

        commandQueue = metalDevice?.makeCommandQueue()
        delegate = self

        // Drawing engine will be initialized when we get valid dimensions
        // This is done in mtkView(_:drawableSizeWillChange:)

        // Enable touch handling
        isUserInteractionEnabled = true
        isMultipleTouchEnabled = false

        // Setup eraser cursor
        setupEraserCursor()
        setupSelectionToolbar()

        if #available(iOS 12.1, *) {
            let pencilInteraction = UIPencilInteraction()
            pencilInteraction.delegate = self
            addInteraction(pencilInteraction)
        }
    }

    required init(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    @objc func simulatePencilDoubleTapForTesting() {
        emitPencilDoubleTapEvent()
    }

    /// Set true by releaseHeavyNativeState. Used to short-circuit any
    /// post-release reallocation (mtkView delegate methods can fire
    /// even after we've been removed from the window if the layout
    /// pass is still in flight). Without this guard, allocatePixelBuffer
    /// would re-create the 13 MB buffer right after we just freed it,
    /// effectively undoing the release. Once true, never flips back --
    /// a released view is dead.
    var isReleased: Bool = false

    deinit {
        releaseHeavyNativeState()
    }

    /// Release the multi-MB pixel buffer + the C++ drawing engine + the
    /// background view's PDF page cache + queued JS callbacks. Called
    /// only for final teardown: deinit (ARC released us), didMoveToWindow
    /// when window becomes nil, removeFromSuperview, AND the explicit
    /// JS-bridge call from the fixed engine pool's final unmount.
    /// All four converge here; the function is idempotent (every field
    /// is nil-checked / no-op-on-second-call) so multiple firings are
    /// safe. We need this many entry points because Instruments
    /// confirmed no single hook was reliable on its own -- something
    /// in the RN/MTKView chain retains views past their React unmount
    /// for unpredictable amounts of time.
    func releaseHeavyNativeState() {
        if isReleased { return }
        isReleased = true

        benchmarkRunToken = UUID()
        isBenchmarkReplayRunning = false
        holdToShapeTimer?.invalidate()
        holdToShapeTimer = nil
        pixelBuffer?.deallocate()
        pixelBuffer = nil
        pixelBufferLength = 0
        pixelBytesPerRow = 0
        if let engine = drawingEngine {
            destroyDrawingEngine(engine)
            drawingEngine = nil
        }
        if let context = ganeshMetalContext {
            destroyGaneshMetalContext(context)
            ganeshMetalContext = nil
        }
        // Drop any queued JS callbacks waiting for a Metal frame that
        // will never come now that we're off-screen. Without this they
        // pin the bridge callback closures until the MTKView eventually
        // releases (sometime later, never on a heavy session).
        pendingPresentedLoadCallbacks.removeAll()
        deferredBase64Payload = nil
        hasDeferredPresentation = false
        onDrawingChange = nil
        onDrawingBegin = nil
        onInkSelectionChange = nil
        onPencilDoubleTap = nil
        delegate = nil
        commandQueue = nil
        isUserInteractionEnabled = false
        eraserCursorLayer?.removeFromSuperlayer()
        eraserCursorLayer = nil
        selectionToolbar.isHidden = true
        selectionToolbar.removeFromSuperview()
        currentTouchLocation = nil
        isTransformingSelection = false
        selectionTransformHandleIndex = -1
        hasSelectionMoveDelta = false
        lastDragPoint = nil
        // The background view holds a PDFKit document + a page cache
        // (up to 3 rendered CGPDFPage at ~10 MB each). When THIS view
        // gets stuck in the retain-cycle limbo, its sibling
        // backgroundView is dragged along with it via RN's container
        // hierarchy, so the cache outlives its usefulness too. Tell
        // it to drop everything now.
        backgroundView?.unloadPDF()
    }

    override func removeFromSuperview() {
        // Defer release because UIKit fires removeFromSuperview during
        // sibling reordering and layout passes too -- NOT only on
        // permanent teardown. Releasing immediately killed live slots
        // that were briefly detached, leaving broken-but-retained
        // views in retain-cycle limbo while React mounted fresh
        // replacements (Instruments showed 13.28 MiB persistent jump
        // 10 -> 18, doubling the per-mount leak rate). The 300ms
        // defer + superview-still-nil check filters out transient
        // detaches: if we got re-added quickly, we don't release.
        super.removeFromSuperview()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            guard let self = self, self.superview == nil, self.window == nil else { return }
            self.releaseHeavyNativeState()
        }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        // window == nil means we've been removed from any window. React
        // unmount funnels through here. Eagerly release the heavy native
        // state -- per-view this is ~13 MB pixel buffer + multi-MB
        // engine state (strokes + 20 history snapshots). Across ~14
        // leaked views that's >250 MB just on pixel buffers, plus the
        // ~500 MB of raster data the engine accumulates -- which is
        // exactly what the user was seeing in the Allocations Instrument.
        //
        // 200 ms defer filters out transient window detachments
        // (rotation, modal presentation, brief layout passes). If the
        // view re-attaches before the timer fires, the closure's
        // self.window check returns false and no release happens. RN's
        // unmount path leaves the view detached permanently, so the
        // closure fires on the leaked view and we reclaim the heavy
        // memory regardless of whether ARC ever releases the Swift
        // object itself.
        if window == nil {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
                guard let self = self, self.window == nil else { return }
                self.releaseHeavyNativeState()
            }
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateSelectionToolbarFrame()
    }

    func requestDisplay(forceWhenSuspended: Bool = false) {
        if renderSuspended && !forceWhenSuspended {
            return
        }

        // Only redraw the background view when explicitly forced (e.g.
        // unsuspending, theme change, PDF URI change). The background
        // doesn't change due to user touches, and on PDF backgrounds
        // a setNeedsDisplay triggers CGPDFPage.draw on the main
        // thread (~10-100ms per page) -- doing that on EVERY touch
        // event blocks stroke rendering and is the actual root cause
        // of the "drawing super laggy on PDF" bug. MobileInkBackgroundView
        // already calls setNeedsDisplay on its own property setters
        // when its inputs (URI, theme, page numbers) change, so we
        // don't need to mirror it here.
        if forceWhenSuspended {
            backgroundView?.setNeedsDisplay()
        }
        setNeedsDisplay()
    }

    private func setupSelectionToolbar() {
        selectionToolbar.axis = .horizontal
        selectionToolbar.alignment = .fill
        selectionToolbar.distribution = .fill
        selectionToolbar.spacing = 0
        selectionToolbar.isHidden = true
        selectionToolbar.isUserInteractionEnabled = true
        selectionToolbar.backgroundColor = UIColor.white.withAlphaComponent(0.96)
        selectionToolbar.layer.cornerRadius = 10
        selectionToolbar.layer.masksToBounds = false
        selectionToolbar.layer.shadowColor = UIColor.black.cgColor
        selectionToolbar.layer.shadowOpacity = 0.18
        selectionToolbar.layer.shadowRadius = 10
        selectionToolbar.layer.shadowOffset = CGSize(width: 0, height: 3)

        selectionToolbar.addArrangedSubview(
            makeSelectionToolbarButton(
                systemName: "doc.on.doc",
                fallbackTitle: "Copy",
                tintColor: UIColor(red: 0.22, green: 0.30, blue: 0.40, alpha: 1),
                action: #selector(handleNativeSelectionCopy)
            )
        )
        addSelectionToolbarDivider()
        selectionToolbar.addArrangedSubview(
            makeSelectionToolbarButton(
                systemName: "doc.on.clipboard",
                fallbackTitle: "Paste",
                tintColor: UIColor(red: 0.22, green: 0.30, blue: 0.40, alpha: 1),
                action: #selector(handleNativeSelectionPaste)
            )
        )
        addSelectionToolbarDivider()
        selectionToolbar.addArrangedSubview(
            makeSelectionToolbarButton(
                systemName: "trash",
                fallbackTitle: "Delete",
                tintColor: UIColor(red: 1.0, green: 0.23, blue: 0.19, alpha: 1),
                action: #selector(handleNativeSelectionDelete)
            )
        )

        addSubview(selectionToolbar)
    }

    private func makeSelectionToolbarButton(
        systemName: String,
        fallbackTitle: String,
        tintColor: UIColor,
        action: Selector
    ) -> UIButton {
        let button = UIButton(type: .system)
        button.tintColor = tintColor
        button.backgroundColor = .clear
        button.adjustsImageWhenHighlighted = true
        button.accessibilityLabel = fallbackTitle
        button.widthAnchor.constraint(equalToConstant: selectionToolbarButtonWidth).isActive = true
        button.heightAnchor.constraint(equalToConstant: selectionToolbarHeight).isActive = true

        if let image = UIImage(systemName: systemName) {
            button.setImage(image, for: .normal)
        } else {
            button.setTitle(fallbackTitle, for: .normal)
            button.titleLabel?.font = UIFont.systemFont(ofSize: 12, weight: .semibold)
        }

        button.addTarget(self, action: action, for: .touchUpInside)
        return button
    }

    private func addSelectionToolbarDivider() {
        let divider = UIView()
        divider.backgroundColor = UIColor(white: 0.88, alpha: 1)
        divider.widthAnchor.constraint(equalToConstant: 1.0 / max(1.0, UIScreen.main.scale)).isActive = true
        selectionToolbar.addArrangedSubview(divider)
    }

    private func selectionBoundsInView(engine: OpaquePointer) -> CGRect? {
        guard getSelectionCount(engine) > 0 else {
            return nil
        }

        var bounds: [Float] = [0, 0, 0, 0]
        getSelectionBounds(engine, &bounds)
        let rect = CGRect(
            x: CGFloat(bounds[0]) / scaleX,
            y: CGFloat(bounds[1]) / scaleY,
            width: CGFloat(bounds[2] - bounds[0]) / scaleX,
            height: CGFloat(bounds[3] - bounds[1]) / scaleY
        )

        guard rect.width > 0, rect.height > 0 else {
            return nil
        }

        return rect
    }

    private func updateSelectionToolbarFrame() {
        guard !isReleased, let engine = drawingEngine, let selectionRect = selectionBoundsInView(engine: engine) else {
            selectionToolbar.isHidden = true
            return
        }

        let toolbarWidth = selectionToolbarButtonWidth * 3 + 2.0 / max(1.0, UIScreen.main.scale)
        let minX = selectionToolbarMargin
        let maxX = max(minX, bounds.width - toolbarWidth - selectionToolbarMargin)
        let centeredX = selectionRect.midX - toolbarWidth / 2.0
        let toolbarX = min(max(centeredX, minX), maxX)

        let preferredY = selectionRect.minY - selectionToolbarHeight - 10
        let fallbackY = selectionRect.maxY + 10
        let rawY = preferredY >= selectionToolbarMargin ? preferredY : fallbackY
        let maxY = max(selectionToolbarMargin, bounds.height - selectionToolbarHeight - selectionToolbarMargin)
        let toolbarY = min(max(rawY, selectionToolbarMargin), maxY)

        selectionToolbar.frame = CGRect(
            x: toolbarX,
            y: toolbarY,
            width: toolbarWidth,
            height: selectionToolbarHeight
        )
        selectionToolbar.isHidden = false
        bringSubviewToFront(selectionToolbar)
    }

    @objc private func handleNativeSelectionCopy() {
        performCopy()
        updateSelectionToolbarFrame()
    }

    @objc private func handleNativeSelectionPaste() {
        performPaste()
        updateSelectionToolbarFrame()
    }

    @objc private func handleNativeSelectionDelete() {
        performDelete()
        updateSelectionToolbarFrame()
    }

    private func selectionBoundsContain(_ location: CGPoint, engine: OpaquePointer, padding: CGFloat) -> Bool {
        guard getSelectionCount(engine) > 0 else {
            return false
        }

        var bounds: [Float] = [0, 0, 0, 0]
        getSelectionBounds(engine, &bounds)
        let selectionRect = CGRect(
            x: CGFloat(bounds[0]) / scaleX - padding,
            y: CGFloat(bounds[1]) / scaleY - padding,
            width: CGFloat(bounds[2] - bounds[0]) / scaleX + padding * 2,
            height: CGFloat(bounds[3] - bounds[1]) / scaleY + padding * 2
        )

        return selectionRect.contains(location)
    }

    private func selectionHandleHitTest(_ location: CGPoint, engine: OpaquePointer) -> Int32? {
        guard getSelectionCount(engine) > 0 else {
            return nil
        }

        var bounds: [Float] = [0, 0, 0, 0]
        getSelectionBounds(engine, &bounds)
        let minX = CGFloat(bounds[0]) / scaleX
        let minY = CGFloat(bounds[1]) / scaleY
        let maxX = CGFloat(bounds[2]) / scaleX
        let maxY = CGFloat(bounds[3]) / scaleY
        guard maxX > minX, maxY > minY else {
            return nil
        }

        let centerX = (minX + maxX) * 0.5
        let centerY = (minY + maxY) * 0.5
        let handles: [(Int32, CGPoint)] = [
            (0, CGPoint(x: minX, y: minY)),
            (1, CGPoint(x: centerX, y: minY)),
            (2, CGPoint(x: maxX, y: minY)),
            (3, CGPoint(x: minX, y: centerY)),
            (4, CGPoint(x: maxX, y: centerY)),
            (5, CGPoint(x: minX, y: maxY)),
            (6, CGPoint(x: centerX, y: maxY)),
            (7, CGPoint(x: maxX, y: maxY)),
        ]
        let hitRadius: CGFloat = 28
        let hitRadiusSquared = hitRadius * hitRadius

        for (index, point) in handles {
            let dx = location.x - point.x
            let dy = location.y - point.y
            if dx * dx + dy * dy <= hitRadiusSquared {
                return index
            }
        }

        return nil
    }

    private func handleFingerSelectionTouch(
        engine: OpaquePointer,
        location: CGPoint,
        scaledX: Float,
        scaledY: Float
    ) -> Bool {
        guard !isTextMode else {
            return false
        }

        cancelHoldToShapePreview()
        isHoldToShapeStrokeActive = false
        updateEraserCursor(at: nil, width: pendingWidth)

        if let handleIndex = selectionHandleHitTest(location, engine: engine) {
            beginSelectionTransform(engine, handleIndex)
            isTransformingSelection = true
            selectionTransformHandleIndex = handleIndex
            hasSelectionMoveDelta = false
            lastDragPoint = location
            requestDisplay()
            return true
        }

        if selectionBoundsContain(location, engine: engine, padding: 24) {
            isMovingSelection = true
            hasSelectionMoveDelta = false
            lastDragPoint = location
            requestDisplay()
            return true
        }

        let hadSelection = getSelectionCount(engine) > 0
        if hadSelection {
            clearSelection(engine)
        }

        if selectShapeStrokeAt(engine, scaledX, scaledY) {
            isMovingSelection = true
            hasSelectionMoveDelta = false
            lastDragPoint = location
            requestDisplay()
            notifySelectionChange()
            return true
        }

        if hadSelection {
            requestDisplay()
            notifySelectionChange()
            return true
        }

        return false
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let engine = drawingEngine, let touch = touches.first else { return }

        let location = touch.location(in: self)
        currentTouchLocation = location
        let scaledX = Float(location.x * scaleX)
        let scaledY = Float(location.y * scaleY)

        if touch.type != .pencil && handleFingerSelectionTouch(engine: engine, location: location, scaledX: scaledX, scaledY: scaledY) {
            return
        }

        // Filter touches based on drawing policy
        // When "pencilonly", only process Apple Pencil touches
        if shouldIgnoreTouchForDrawingPolicy(touch) {
            return
        }

        // Update eraser cursor position
        updateEraserCursor(at: location, width: pendingWidth)

        onDrawingBegin?([
            "x": location.x,
            "y": location.y,
        ])

        // Don't process touches when in text mode
        if isTextMode {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            return
        }

        if isSelectionMode {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            // Check if we're tapping inside an existing selection
            let hasExistingSelection = getSelectionCount(engine) > 0
            var tappedInsideSelection = false

            if hasExistingSelection {
                var bounds: [Float] = [0, 0, 0, 0]
                getSelectionBounds(engine, &bounds)
                let selectionRect = CGRect(x: CGFloat(bounds[0]) / scaleX,
                                          y: CGFloat(bounds[1]) / scaleY,
                                          width: CGFloat(bounds[2] - bounds[0]) / scaleX,
                                          height: CGFloat(bounds[3] - bounds[1]) / scaleY)
                tappedInsideSelection = selectionRect.contains(location)
            }

            if tappedInsideSelection {
                // Start moving the existing selection
                isMovingSelection = true
                hasSelectionMoveDelta = false
                lastDragPoint = location
            } else {
                // Clear previous selection and start new selection drag
                clearSelection(engine)

                // Start selection by calling touchBegan (this initiates the selection path)
                let pressure = normalizedPressure(for: touch)
                let (azimuth, altitude) = extractAzimuthAndAltitude(for: touch)
                let isPencilInput = PencilStrokeInputNormalizer.isPencil(touch)
                let timestamp = Int64(touch.timestamp * 1000)  // Convert to milliseconds
                touchBegan(engine, scaledX, scaledY, pressure, azimuth, altitude, timestamp, isPencilInput)
                recordBenchmarkInputSample()

                isDraggingSelection = true
                lastDragPoint = location
            }
            requestDisplay()
        } else {
            // Normal drawing mode
            let pressure = normalizedPressure(for: touch)
            let (azimuth, altitude) = extractAzimuthAndAltitude(for: touch)
            let isPencilInput = PencilStrokeInputNormalizer.isPencil(touch)
            let timestamp = Int64(touch.timestamp * 1000)  // Convert to milliseconds
            touchBegan(engine, scaledX, scaledY, pressure, azimuth, altitude, timestamp, isPencilInput)
            recordBenchmarkInputSample()
            isHoldToShapeStrokeActive = true
            scheduleHoldToShapePreview(restart: true)
            requestDisplay()
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let engine = drawingEngine, let touch = touches.first else { return }

        // Don't process touches when in text mode
        if isTextMode {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            return
        }

        // Filter touches based on drawing policy
        if shouldIgnoreTouchForDrawingPolicy(touch) {
            return
        }

        // Update cursor for all touch modes (but skip during selection move for performance)
        let currentLocation = touch.location(in: self)
        currentTouchLocation = currentLocation

        if isTransformingSelection {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            updateSelectionTransform(
                engine,
                Float(currentLocation.x * scaleX),
                Float(currentLocation.y * scaleY)
            )
            hasSelectionMoveDelta = true
            lastDragPoint = currentLocation
            requestDisplay()
            notifySelectionChange()
            return
        }

        if isMovingSelection, let lastPoint = lastDragPoint {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            // Moving existing selection - optimized path
            let location = touch.location(in: self)
            let dx = Float((location.x - lastPoint.x) * scaleX)
            let dy = Float((location.y - lastPoint.y) * scaleY)

            if dx != 0 || dy != 0 {
                hasSelectionMoveDelta = true
                moveSelection(engine, dx, dy)
                notifySelectionChange()
            }
            lastDragPoint = location
            requestDisplay()
            return  // Skip eraser cursor update for performance
        }

        // Update eraser cursor only when not moving selection
        updateEraserCursor(at: currentLocation, width: pendingWidth)

        if isSelectionMode && isDraggingSelection {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            // In selection mode: continue selecting strokes (like eraser drag)
            if let coalesced = event?.coalescedTouches(for: touch), !coalesced.isEmpty {
                for sample in coalesced {
                    processTouchSample(engine: engine, touch: sample)
                }
            } else {
                processTouchSample(engine: engine, touch: touch)
            }
            requestDisplay()
        } else if !isSelectionMode {
            // PREDICTIVE TOUCH: Clear old predicted points before adding new actual data
            clearPredictedPoints(engine)

            // Normal drawing mode - process coalesced touches (actual data)
            if let coalesced = event?.coalescedTouches(for: touch), !coalesced.isEmpty {
                for sample in coalesced {
                    processTouchSample(engine: engine, touch: sample)
                }
            } else {
                processTouchSample(engine: engine, touch: touch)
            }

            // PREDICTIVE TOUCH: Add predicted points for Apple Pencil low-latency rendering
            // Only for Apple Pencil - finger touch doesn't have useful predictions
            if touch.type == .pencil, let predicted = event?.predictedTouches(for: touch) {
                for predictedTouch in predicted {
                    processPredictedTouchSample(engine: engine, touch: predictedTouch)
                }
            }

            scheduleHoldToShapePreview()
            requestDisplay()
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let engine = drawingEngine, let touch = touches.first else { return }

        // Don't process touches when in text mode
        if isTextMode {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            return
        }

        // Filter touches based on drawing policy
        if shouldIgnoreTouchForDrawingPolicy(touch) {
            return
        }

        // Hide eraser cursor when touch ends
        currentTouchLocation = nil
        updateEraserCursor(at: nil, width: pendingWidth)

        if isTransformingSelection {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            let didTransformSelection = hasSelectionMoveDelta
            if didTransformSelection {
                finalizeSelectionTransform(engine)
            } else {
                cancelSelectionTransform(engine)
            }
            isTransformingSelection = false
            selectionTransformHandleIndex = -1
            hasSelectionMoveDelta = false
            lastDragPoint = nil
            requestDisplay()
            notifySelectionChange()
            if didTransformSelection {
                onDrawingChange?([:])
            }
        } else if isMovingSelection {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            // Finished moving selection - finalize and update history
            // only when the touch actually moved. A finger tap that just
            // selects a stroke should not create a zero-distance undo entry.
            let didMoveSelection = hasSelectionMoveDelta
            if didMoveSelection {
                finalizeMove(engine)
            }
            isMovingSelection = false
            hasSelectionMoveDelta = false
            lastDragPoint = nil
            requestDisplay()
            notifySelectionChange()
            if didMoveSelection {
                onDrawingChange?([:])
            }
        } else if isSelectionMode && isDraggingSelection {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            // Finalize selection
            touchEnded(engine, currentUptimeTimestampMillis())
            isDraggingSelection = false
            lastDragPoint = nil
            requestDisplay()
            notifySelectionChange()
        } else if !isSelectionMode {
            cancelHoldToShapePreview()
            isHoldToShapeStrokeActive = false
            // Normal drawing mode
            // PREDICTIVE TOUCH: Clear predicted points before finalizing stroke
            // Final stroke should only contain actual touch data, not predictions
            clearPredictedPoints(engine)
            touchEnded(engine, currentUptimeTimestampMillis())
            recordBenchmarkInputSample()
            requestDisplay()
            onDrawingChange?([:])
        }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let engine = drawingEngine, let touch = touches.first else { return }

        // Filter touches based on drawing policy
        if shouldIgnoreTouchForDrawingPolicy(touch) {
            return
        }

        // Hide eraser cursor when touch cancelled
        cancelHoldToShapePreview()
        isHoldToShapeStrokeActive = false
        currentTouchLocation = nil
        updateEraserCursor(at: nil, width: pendingWidth)

        if isTransformingSelection {
            cancelSelectionTransform(engine)
            isTransformingSelection = false
            selectionTransformHandleIndex = -1
            hasSelectionMoveDelta = false
            lastDragPoint = nil
            requestDisplay()
            notifySelectionChange()
        } else if isSelectionMode || isMovingSelection {
            // Cancel selection drag or move
            isDraggingSelection = false
            isMovingSelection = false
            hasSelectionMoveDelta = false
            lastDragPoint = nil
        } else {
            // Normal drawing mode
            clearPredictedPoints(engine)  // Clear predictions before cancelling
            touchEnded(engine, 0)
            requestDisplay()
            onDrawingChange?([:])
        }
    }

    @objc func clear() {
        guard let engine = drawingEngine else { return }
        clearCanvas(engine)
        requestDisplay()
        onDrawingChange?([:])
    }

    @objc func undo() {
        guard let engine = drawingEngine else { return }
        undoStroke(engine)
        requestDisplay()
        onDrawingChange?([:])
    }

    @objc func redo() {
        guard let engine = drawingEngine else { return }
        redoStroke(engine)
        requestDisplay()
        onDrawingChange?([:])
    }

    // MARK: - Eraser Cursor

    private func setupEraserCursor() {
        if eraserCursorLayer == nil {
            let cursorLayer = CAShapeLayer()
            cursorLayer.fillColor = UIColor.clear.cgColor
            cursorLayer.strokeColor = UIColor.systemGray.cgColor
            cursorLayer.lineWidth = 2.0
            cursorLayer.isHidden = true
            layer.addSublayer(cursorLayer)
            eraserCursorLayer = cursorLayer
        }
    }

    private func updateEraserCursor(at location: CGPoint?, width: Float) {
        guard let cursorLayer = eraserCursorLayer else { return }

        if let location = location, pendingTool == "eraser" && pendingEraserMode == "pixel" {
            // Show cursor with circle matching eraser size
            // Width is in canvas coordinates, need to convert to view coordinates
            let radiusInViewCoords = CGFloat(width) / (2.0 * scaleX)
            let circlePath = UIBezierPath(arcCenter: location,
                                         radius: radiusInViewCoords,
                                         startAngle: 0,
                                         endAngle: .pi * 2,
                                         clockwise: true)
            cursorLayer.path = circlePath.cgPath
            cursorLayer.isHidden = false
        } else {
            // Hide cursor for non-eraser tools
            cursorLayer.isHidden = true
        }
    }

    private func emitPencilDoubleTapEvent() {
        pencilDoubleTapSequence += 1
        onPencilDoubleTap?([
            "sequence": pencilDoubleTapSequence,
            "timestamp": Date().timeIntervalSince1970 * 1000,
        ])
    }

    @objc func setTool(_ toolType: String, width: Float, color: UIColor, eraserMode: String = "pixel") {
        cancelHoldToShapePreview()

        // Convert UIColor to UInt32 (ARGB format)
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        color.getRed(&red, green: &green, blue: &blue, alpha: &alpha)

        let a = UInt32(alpha * 255) << 24
        let r = UInt32(red * 255) << 16
        let g = UInt32(green * 255) << 8
        let b = UInt32(blue * 255)
        let argb = a | r | g | b

        // Store settings for when engine is ready
        pendingTool = toolType
        pendingWidth = width
        pendingColor = argb
        pendingEraserMode = eraserMode
        isHoldToShapeStrokeActive = false

        syncInteractionModeFlags()
        resetTransientInteractionState()

        if !isSelectionMode, let engine = drawingEngine {
            // Clear selection when switching to non-selection tool
            clearSelection(engine)
            notifySelectionChange()
        }

        // Update eraser cursor visibility when tool changes
        if toolType == "eraser" && eraserMode == "pixel" {
            updateEraserCursor(at: currentTouchLocation, width: width)
        } else {
            updateEraserCursor(at: nil, width: width)
        }

        // If engine exists, apply immediately
        if let engine = drawingEngine {
            toolType.withCString { toolPtr in
                pendingEraserMode.withCString { eraserPtr in
                    nativeSetToolWithParams(engine, toolPtr, width, argb, eraserPtr)
                }
            }
        }
    }

    /// Set background type on C++ engine so it knows whether to clear with transparent (for PDF) or white
    func setEngineBackgroundType(_ backgroundType: String) {
        // Always store as pending (applied when engine is created, or re-applied if engine restarts)
        pendingBackgroundType = backgroundType

        guard let engine = drawingEngine else {
            print("⏳ [MobileInkCanvasView] Engine not ready, stored pending background type: \(backgroundType)")
            return
        }

        backgroundType.withCString { typePtr in
            nativeSetBackgroundType(engine, typePtr)
        }
        print("✅ [MobileInkCanvasView] Set engine background type to: \(backgroundType)")
        requestDisplay(forceWhenSuspended: true)
    }

    @objc func getBase64Data(_ callback: @escaping RCTResponseSenderBlock) {
        guard let engine = drawingEngine else {
            callback(["Engine not initialized", NSNull()])
            return
        }

        var size: Int32 = 0
        guard let dataPtr = serializeDrawing(engine, &size), size > 0 else {
            // Empty drawing - return empty JSON
            let emptyJson = "{\"pages\":{}}"
            callback([NSNull(), emptyJson])
            return
        }

        // Convert to Data
        let data = Data(bytes: dataPtr, count: Int(size))
        freeSerializedData(dataPtr)

        // Encode as base64
        let base64String = data.base64EncodedString()

        // Wrap in PencilKit-compatible JSON format
        let jsonDict: [String: Any] = [
            "pages": [
                "0": base64String
            ]
        ]

        do {
            let jsonData = try JSONSerialization.data(withJSONObject: jsonDict, options: [])
            if let jsonString = String(data: jsonData, encoding: .utf8) {
                callback([NSNull(), jsonString])
            } else {
                callback(["Failed to encode JSON string", NSNull()])
            }
        } catch {
            callback([error.localizedDescription, NSNull()])
        }
    }

    @objc func loadBase64Data(_ base64String: String, callback: @escaping RCTResponseSenderBlock) {
        deferredBase64Payload = nil
        if let errorMessage = applyBase64Data(base64String) {
            callback([errorMessage, NSNull()])
            return
        }

        hasDeferredPresentation = false
        enqueueLoadPresentationCallback(callback)
    }

    @objc func stageBase64Data(_ base64String: String, callback: @escaping RCTResponseSenderBlock) {
        deferredBase64Payload = base64String
        hasDeferredPresentation = true
        callback([NSNull(), true])
    }

    @objc func presentDeferredLoad(_ callback: @escaping RCTResponseSenderBlock) {
        if !hasDeferredPresentation {
            callback([NSNull(), true])
            return
        }

        let pendingPayload = deferredBase64Payload
        deferredBase64Payload = nil
        hasDeferredPresentation = false

        if let pendingPayload {
            if let errorMessage = applyBase64Data(pendingPayload) {
                callback([errorMessage, NSNull()])
                return
            }
        }

        enqueueLoadPresentationCallback(callback)
    }

    private func applyBase64Data(_ base64String: String) -> String? {
        guard let engine = drawingEngine else {
            return "Engine not initialized"
        }

        if base64String.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            clearCanvas(engine)
            syncInteractionModeFlags()
            resetTransientInteractionState()
            applyPendingTool(to: engine)
            notifySelectionChange()
            return nil
        }

        guard let jsonData = base64String.data(using: .utf8) else {
            return "Invalid JSON string"
        }

        do {
            guard let jsonDict = try JSONSerialization.jsonObject(with: jsonData, options: []) as? [String: Any],
                  let pages = jsonDict["pages"] as? [String: String] else {
                return "Invalid drawing payload JSON"
            }

            guard let page0Base64 = pages["0"] else {
                clearCanvas(engine)
                syncInteractionModeFlags()
                resetTransientInteractionState()
                applyPendingTool(to: engine)
                notifySelectionChange()
                return nil
            }

            guard let binaryData = Data(base64Encoded: page0Base64) else {
                return "Failed to decode base64 data"
            }

            if !restoreRawDrawingData(binaryData, into: engine) {
                return "Failed to deserialize drawing payload"
            }

            syncInteractionModeFlags()
            resetTransientInteractionState()
            applyPendingTool(to: engine)
            notifySelectionChange()

            return nil
        } catch {
            return error.localizedDescription
        }
    }

    private func enqueueLoadPresentationCallback(_ callback: @escaping RCTResponseSenderBlock) {
        pendingPresentedLoadCallbacks.append(callback)
        requestDisplay(forceWhenSuspended: true)
    }

    private func syncInteractionModeFlags() {
        isSelectionMode = (pendingTool == "select")
        isTextMode = (pendingTool == "text")
    }

    func resetTransientInteractionState() {
        isDraggingSelection = false
        isMovingSelection = false
        isTransformingSelection = false
        selectionTransformHandleIndex = -1
        hasSelectionMoveDelta = false
        isHoldToShapeStrokeActive = false
        lastDragPoint = nil
    }

    @objc func getBase64PngData(_ scale: CGFloat, callback: @escaping RCTResponseSenderBlock) {
        print("📸 [Export] getBase64PngData called with scale: \(scale)")
        print("📸 [Export] pageWidth: \(pageWidth), pageHeight: \(pageHeight)")
        print("📸 [Export] backgroundView: \(backgroundView != nil ? "exists" : "nil")")

        guard let engine = drawingEngine else {
            callback(["Engine not initialized", NSNull()])
            return
        }

        // Get Skia snapshot
        guard let skImagePtr = makeSnapshot(engine) else {
            callback(["Failed to create snapshot", NSNull()])
            return
        }
        defer { releaseSkImage(skImagePtr) }

        // Convert Skia image to UIImage with background composited
        guard let uiImage = skImageToUIImage(skImagePtr, scale: scale) else {
            callback(["Failed to convert image", NSNull()])
            return
        }

        print("📸 [Export] Generated UIImage size: \(uiImage.size.width) × \(uiImage.size.height), scale: \(uiImage.scale)")
        print("📸 [Export] Actual pixel dimensions: \(Int(uiImage.size.width * uiImage.scale)) × \(Int(uiImage.size.height * uiImage.scale))")

        // Encode as PNG
        guard let pngData = uiImage.pngData() else {
            callback(["Failed to encode PNG", NSNull()])
            return
        }

        // Return as data URI
        let base64String = pngData.base64EncodedString()
        let dataUri = "data:image/png;base64,\(base64String)"
        callback([NSNull(), dataUri])
    }

    @objc func getBase64JpegData(_ scale: CGFloat, compression: CGFloat, callback: @escaping RCTResponseSenderBlock) {
        guard let engine = drawingEngine else {
            callback(["Engine not initialized", NSNull()])
            return
        }

        // Get Skia snapshot
        guard let skImagePtr = makeSnapshot(engine) else {
            callback(["Failed to create snapshot", NSNull()])
            return
        }
        defer { releaseSkImage(skImagePtr) }

        // Convert Skia image to UIImage with background composited
        guard let uiImage = skImageToUIImage(skImagePtr, scale: scale) else {
            callback(["Failed to convert image", NSNull()])
            return
        }

        // Encode as JPEG
        guard let jpegData = uiImage.jpegData(compressionQuality: compression) else {
            callback(["Failed to encode JPEG", NSNull()])
            return
        }

        // Return as data URI
        let base64String = jpegData.base64EncodedString()
        let dataUri = "data:image/jpeg;base64,\(base64String)"
        callback([NSNull(), dataUri])
    }

    // Helper to convert Skia image to UIImage with background composited
    // Copied from PencilKit's getBase64PngDataForPage approach
    private func skImageToUIImage(_ skImagePtr: OpaquePointer, scale: CGFloat) -> UIImage? {
        let actualScale = scale > 0 ? scale : UIScreen.main.scale

        // Use actual page dimensions (screen-based), exactly like PencilKit
        let pageBounds = CGRect(x: 0, y: 0, width: pageWidth, height: pageHeight)

        print("📸 [Composite] Exporting at page dimensions: \(pageWidth) × \(pageHeight), scale: \(actualScale)")

        // Create a graphics context to composite background + drawing (PencilKit line 490)
        let size = CGSize(width: pageWidth * actualScale, height: pageHeight * actualScale)
        print("📸 [Composite] Context size: \(size.width) × \(size.height)")

        UIGraphicsBeginImageContextWithOptions(size, true, 1.0)
        guard let context = UIGraphicsGetCurrentContext() else {
            return nil
        }

        // Scale the context (PencilKit line 498)
        context.scaleBy(x: actualScale, y: actualScale)

        // Draw background directly into context (PencilKit line 500-509)
        // This matches PencilKit's drawBackground(in: context, rect: pageBounds, type: backgroundType)
        if let bgView = backgroundView {
            print("📸 [Composite] Drawing background into context with drawIntoContext method")
            bgView.drawIntoContext(context, rect: pageBounds)
        } else {
            print("📸 [Composite] No backgroundView, filling with white")
            context.setFillColor(UIColor.white.cgColor)
            context.fill(pageBounds)
        }

        // Draw the Skia drawing on top (PencilKit line 512-515)
        // For Skia, render at screen scale for quality, then draw scaled
        guard let engine = drawingEngine else {
            UIGraphicsEndImageContext()
            return nil
        }

        // Render strokes at screen scale for quality (prevents dotted lines at low scales)
        let drawingImage = renderEngineToUIImage(engine, scale: actualScale)

        // The image is rendered at minRenderScale but we draw it at page bounds
        // The context scaling handles the final output size
        drawingImage.draw(in: pageBounds)

        // Get the composite image (PencilKit line 518)
        guard let compositeImage = UIGraphicsGetImageFromCurrentImageContext() else {
            UIGraphicsEndImageContext()
            return nil
        }
        UIGraphicsEndImageContext()

        return compositeImage
    }

    // Render engine to UIImage at specified scale (like PencilKit's image(from:scale:))
    private func renderEngineToUIImage(_ engine: OpaquePointer, scale: CGFloat) -> UIImage {
        // Always render at minimum 2x to preserve stroke quality, then scale down if needed
        // This prevents thin strokes from appearing dotted at low scales
        let minRenderScale = max(scale, UIScreen.main.scale)

        // Calculate render dimensions at the higher scale
        let renderWidth = Int(pageWidth * minRenderScale)
        let renderHeight = Int(pageHeight * minRenderScale)
        let bytesPerRow = renderWidth * 4
        let bufferLength = bytesPerRow * renderHeight

        // Allocate temporary buffer for rendering
        guard let renderBuffer = malloc(bufferLength) else {
            return UIImage() // Return empty image on allocation failure
        }
        defer { free(renderBuffer) }

        // Clear buffer to transparent
        memset(renderBuffer, 0, bufferLength)

        // Create Skia canvas wrapping the render buffer
        let renderCanvas = createSkiaCanvas(
            renderBuffer,
            Int32(renderWidth),
            Int32(renderHeight),
            Int32(bytesPerRow)
        )

        if let renderCanvas = renderCanvas {
            // Engine stores strokes in pixel coordinates, but we're exporting at page dimensions
            // Scale to convert: pixels → page → export scale (using minRenderScale for quality)
            let pixelToPage = Float(pageWidth) / Float(pixelWidth)  // e.g. 788 / 1576 = 0.5
            let totalScale = pixelToPage * Float(minRenderScale)  // Use higher scale for quality
            scaleSkiaCanvas(renderCanvas, totalScale, totalScale)

            // Render engine strokes (in pixel coords) to the scaled canvas
            renderToCanvas(engine, renderCanvas)
            destroySkiaCanvas(renderCanvas)
        }

        // Convert buffer to UIImage
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue:
            CGImageAlphaInfo.premultipliedFirst.rawValue |
            CGBitmapInfo.byteOrder32Little.rawValue
        )

        guard let dataProvider = CGDataProvider(
            data: Data(bytes: renderBuffer, count: bufferLength) as CFData
        ),
        let cgImage = CGImage(
            width: renderWidth,
            height: renderHeight,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: bytesPerRow,
            space: colorSpace,
            bitmapInfo: bitmapInfo,
            provider: dataProvider,
            decode: nil,
            shouldInterpolate: true,
            intent: .defaultIntent
        ) else {
            return UIImage()
        }

        return UIImage(cgImage: cgImage)
    }

    // Selection operations
    @objc func performCopy() {
        guard let engine = drawingEngine else { return }
        copySelection(engine)
        updateSelectionToolbarFrame()
    }

    @objc func performPaste() {
        guard let engine = drawingEngine else { return }
        // Paste with a slight offset (50 points down and right)
        pasteSelection(engine, 50.0, 50.0)
        requestDisplay()
        notifySelectionChange()
        onDrawingChange?([:])
    }

    @objc func performDelete() {
        guard let engine = drawingEngine else { return }
        deleteSelection(engine)
        requestDisplay()
        notifySelectionChange()
        onDrawingChange?([:])
    }

    func notifySelectionChange() {
        guard let engine = drawingEngine else { return }
        let count = Int(getSelectionCount(engine))
        var payload: [String: Any] = ["count": count]
        if count > 0 {
            var bounds: [Float] = [0, 0, 0, 0]
            getSelectionBounds(engine, &bounds)
            payload["bounds"] = [
                "x": CGFloat(bounds[0]) / scaleX,
                "y": CGFloat(bounds[1]) / scaleY,
                "width": CGFloat(bounds[2] - bounds[0]) / scaleX,
                "height": CGFloat(bounds[3] - bounds[1]) / scaleY,
            ]
        } else {
            payload["bounds"] = NSNull()
        }
        updateSelectionToolbarFrame()
        onInkSelectionChange?(payload)
    }

    private func serializedDrawingData() -> Data? {
        guard let engine = drawingEngine else {
            return nil
        }

        var size: Int32 = 0
        guard let dataPtr = serializeDrawing(engine, &size), size > 0 else {
            return nil
        }
        defer {
            freeSerializedData(dataPtr)
        }

        return Data(bytes: dataPtr, count: Int(size))
    }

    private func applyPendingBackgroundType(to engine: OpaquePointer) {
        pendingBackgroundType.withCString { typePtr in
            nativeSetBackgroundType(engine, typePtr)
        }
    }

    private func applyPendingTool(to engine: OpaquePointer) {
        pendingTool.withCString { toolPtr in
            pendingEraserMode.withCString { eraserPtr in
                nativeSetToolWithParams(engine, toolPtr, pendingWidth, pendingColor, eraserPtr)
            }
        }
    }

    @discardableResult
    private func restoreRawDrawingData(_ drawingData: Data, into engine: OpaquePointer) -> Bool {
        let didDeserialize = drawingData.withUnsafeBytes { (ptr: UnsafeRawBufferPointer) -> Bool in
            guard let baseAddress = ptr.baseAddress else {
                return false
            }

            let uint8Ptr = baseAddress.assumingMemoryBound(to: UInt8.self)
            return deserializeDrawing(engine, uint8Ptr, Int32(drawingData.count))
        }

        guard didDeserialize else {
            return false
        }

        syncInteractionModeFlags()
        resetTransientInteractionState()
        applyPendingTool(to: engine)
        notifySelectionChange()
        requestDisplay(forceWhenSuspended: true)
        return true
    }

    private func configureDrawingEngine(
        width: Int32,
        height: Int32,
        preserveExistingDrawing: Bool
    ) {
        let preservedDrawingData = preserveExistingDrawing ? serializedDrawingData() : nil

        if let engine = drawingEngine {
            destroyDrawingEngine(engine)
            drawingEngine = nil
        }

        drawingEngine = createDrawingEngine(width, height)
        enginePixelWidth = width
        enginePixelHeight = height

        guard let engine = drawingEngine else {
            return
        }

        applyPendingBackgroundType(to: engine)

        if let preservedDrawingData, !preservedDrawingData.isEmpty {
            if !restoreRawDrawingData(preservedDrawingData, into: engine) {
                print("⚠️ Failed to restore drawing payload after resizing engine")
                syncInteractionModeFlags()
                resetTransientInteractionState()
                applyPendingTool(to: engine)
                notifySelectionChange()
            }
        } else {
            syncInteractionModeFlags()
            resetTransientInteractionState()
            applyPendingTool(to: engine)
            notifySelectionChange()
            requestDisplay(forceWhenSuspended: true)
        }
    }

    #if DEBUG
    func injectStrokeForTesting(points: [CGPoint]) {
        guard let engine = drawingEngine, let firstPoint = points.first else {
            return
        }

        touchBegan(
            engine,
            Float(firstPoint.x * scaleX),
            Float(firstPoint.y * scaleY),
            1.0,
            0.0,
            PencilStrokeInputNormalizer.perpendicularAltitude,
            0,
            false
        )

        for (index, point) in points.dropFirst().enumerated() {
            touchMoved(
                engine,
                Float(point.x * scaleX),
                Float(point.y * scaleY),
                1.0,
                0.0,
                PencilStrokeInputNormalizer.perpendicularAltitude,
                Int64((index + 1) * 16),
                false
            )
        }

        touchEnded(engine, Int64(points.count * 16))
        requestDisplay(forceWhenSuspended: true)
    }

    func serializedDrawingDataForTesting() -> Data? {
        serializedDrawingData()
    }

    @discardableResult
    func loadRawDrawingDataForTesting(_ drawingData: Data) -> Bool {
        guard let engine = drawingEngine else {
            return false
        }

        return restoreRawDrawingData(drawingData, into: engine)
    }
    #endif
}

@available(iOS 12.1, *)
extension MobileInkCanvasView: UIPencilInteractionDelegate {
    func pencilInteractionDidTap(_ interaction: UIPencilInteraction) {
        emitPencilDoubleTapEvent()
    }

    @available(iOS 17.5, *)
    func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveTap tap: UIPencilInteraction.Tap) {
        emitPencilDoubleTapEvent()
    }
}

extension MobileInkCanvasView: MTKViewDelegate {
    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // If we've already released (slot unmount), refuse to
        // re-allocate the 13 MB pixel buffer / re-create the engine.
        // MTKView can fire this delegate method during the layout pass
        // even after we've been removed from the window hierarchy --
        // that's exactly the path that resurrected leaked buffers
        // before this guard.
        if isReleased { return }

        // Calculate page dimensions from bounds (for export)
        if bounds.width > 0 && bounds.height > 0 {
            pageWidth = bounds.width
            pageHeight = pageWidth * (11.0 / 8.5) // US Letter aspect ratio
        }

        // Calculate scale from view bounds to drawable (pixel) coordinates for touch input
        if bounds.width > 0 && bounds.height > 0 {
            scaleX = size.width / bounds.width
            scaleY = size.height / bounds.height
        }

        if size.width > 0 && size.height > 0 {
            pixelWidth = Int32(size.width)
            pixelHeight = Int32(size.height)
            if useExperimentalGaneshBackend {
                pixelBuffer?.deallocate()
                pixelBuffer = nil
                pixelBufferLength = 0
                pixelBytesPerRow = 0
            } else {
                allocatePixelBuffer(width: Int(size.width), height: Int(size.height))
            }
        }

        if pixelWidth > 0 && pixelHeight > 0 {
            if drawingEngine == nil {
                configureDrawingEngine(
                    width: pixelWidth,
                    height: pixelHeight,
                    preserveExistingDrawing: false
                )
                print("✅ Drawing engine created with size: \(pixelWidth)x\(pixelHeight) (page: \(pageWidth)x\(pageHeight))")
            } else if enginePixelWidth != pixelWidth || enginePixelHeight != pixelHeight {
                configureDrawingEngine(
                    width: pixelWidth,
                    height: pixelHeight,
                    preserveExistingDrawing: true
                )
                print("✅ Drawing engine resized to: \(pixelWidth)x\(pixelHeight) (page: \(pageWidth)x\(pageHeight))")
            }
        }

        requestDisplay(forceWhenSuspended: true)
    }


    private func ensureGaneshMetalContext() -> OpaquePointer? {
        if let context = ganeshMetalContext {
            return context
        }

        guard let device = device, let commandQueue = commandQueue else {
            return nil
        }

        let devicePtr = Unmanaged.passUnretained(device).toOpaque()
        let queuePtr = Unmanaged.passUnretained(commandQueue).toOpaque()
        ganeshMetalContext = createGaneshMetalContext(devicePtr, queuePtr)
        if ganeshMetalContext != nil {
            print("[MobileInk][Ganesh] Experimental Ganesh/Metal renderer enabled")
        }
        return ganeshMetalContext
    }

    private func attachPresentedLoadCallbacks(to commandBuffer: MTLCommandBuffer) {
        guard !pendingPresentedLoadCallbacks.isEmpty else {
            return
        }

        let loadCallbacks = pendingPresentedLoadCallbacks
        pendingPresentedLoadCallbacks.removeAll()
        commandBuffer.addCompletedHandler { _ in
            DispatchQueue.main.async {
                loadCallbacks.forEach { callback in
                    callback([NSNull(), true])
                }
            }
        }
    }

    func draw(in view: MTKView) {
        guard let engine = drawingEngine else {
            return
        }

        let width = pixelWidth
        let height = pixelHeight

        if useExperimentalGaneshBackend,
           let commandQueue = commandQueue,
           let ganeshContext = ensureGaneshMetalContext(),
           let freshDrawable = view.currentDrawable {
            let texturePtr = Unmanaged.passUnretained(freshDrawable.texture).toOpaque()
            let renderStart = CACurrentMediaTime()
            if renderToGaneshMetalTexture(engine, ganeshContext, texturePtr, width, height) {
                let renderDurationMs = (CACurrentMediaTime() - renderStart) * 1000.0
                guard let commandBuffer = commandQueue.makeCommandBuffer() else {
                    return
                }
                attachPresentedLoadCallbacks(to: commandBuffer)
                attachBenchmarkFrameCallback(
                    to: commandBuffer,
                    backend: .ganesh,
                    didFallbackFromGanesh: false,
                    renderDurationMs: renderDurationMs
                )
                commandBuffer.present(freshDrawable)
                commandBuffer.commit()
                return
            }
        }

        if useExperimentalGaneshBackend && pixelBuffer == nil && width > 0 && height > 0 {
            allocatePixelBuffer(width: Int(width), height: Int(height))
        }

        guard let commandBuffer = commandQueue?.makeCommandBuffer(),
              let buffer = pixelBuffer else {
            return
        }

        let renderStart = CACurrentMediaTime()
        let bytesPerRow = pixelBytesPerRow

        // Clear pixel buffer to transparent (background view underneath will show through)
        memset(buffer, 0, pixelBufferLength)

        // Render Skia strokes to pixel buffer (no scaling - engine already in pixel coords)
        let canvas = createSkiaCanvas(UnsafeMutableRawPointer(buffer), width, height, Int32(bytesPerRow))
        if let canvas = canvas {
            renderToCanvas(engine, canvas)
            destroySkiaCanvas(canvas)
        }

        // OPTIMIZATION: Acquire drawable as late as possible (after CPU rendering is done)
        // This minimizes drawable hold time per Apple's Metal best practices
        guard let freshDrawable = view.currentDrawable else { return }

        // Copy pixel buffer to drawable texture
        let region = MTLRegionMake2D(0, 0, Int(width), Int(height))
        freshDrawable.texture.replace(
            region: region,
            mipmapLevel: 0,
            withBytes: UnsafeRawPointer(buffer),
            bytesPerRow: bytesPerRow
        )
        let renderDurationMs = (CACurrentMediaTime() - renderStart) * 1000.0

        attachPresentedLoadCallbacks(to: commandBuffer)
        attachBenchmarkFrameCallback(
            to: commandBuffer,
            backend: .cpu,
            didFallbackFromGanesh: useExperimentalGaneshBackend,
            renderDurationMs: renderDurationMs
        )

        commandBuffer.present(freshDrawable)
        commandBuffer.commit()
    }
}

extension MobileInkCanvasView {
  private func canPreviewHoldToShape() -> Bool {
    !isSelectionMode
      && !isTextMode
      && (pendingTool == "pen"
        || pendingTool == "pencil"
        || pendingTool == "marker"
        || pendingTool == "highlighter"
        || pendingTool == "crayon"
        || pendingTool == "calligraphy")
  }

  func currentUptimeTimestampMillis() -> Int64 {
    Int64(ProcessInfo.processInfo.systemUptime * 1000)
  }

  private func scheduleHoldToShapePreview(
    restart: Bool = false,
    delay: TimeInterval? = nil
  ) {
    if restart {
      cancelHoldToShapePreview()
    }

    guard holdToShapeTimer == nil, isHoldToShapeStrokeActive, canPreviewHoldToShape() else {
      return
    }

    let timer = Timer(timeInterval: delay ?? holdToShapeDelay, repeats: false) { [weak self] _ in
      self?.showHoldToShapePreview()
    }
    holdToShapeTimer = timer
    RunLoop.main.add(timer, forMode: .common)
  }

  func cancelHoldToShapePreview() {
    holdToShapeTimer?.invalidate()
    holdToShapeTimer = nil
  }

  private func showHoldToShapePreview() {
    holdToShapeTimer = nil

    guard canPreviewHoldToShape(), let engine = drawingEngine else {
      return
    }

    clearPredictedPoints(engine)
    if updateHoldShapePreview(engine, currentUptimeTimestampMillis()) {
      requestDisplay()
    } else if isHoldToShapeStrokeActive {
      scheduleHoldToShapePreview(delay: holdToShapeRetryDelay)
    }
  }

  private func normalizedPressure(for touch: UITouch) -> Float {
    PencilStrokeInputNormalizer.normalizedPressure(for: touch)
  }

  private func extractAzimuthAndAltitude(for touch: UITouch) -> (azimuth: Float, altitude: Float) {
    PencilStrokeInputNormalizer.azimuthAndAltitude(for: touch, in: self)
  }

  private func processTouchSample(engine: OpaquePointer, touch: UITouch) {
    let location = touch.location(in: self)
    let pressure = normalizedPressure(for: touch)
    let (azimuth, altitude) = extractAzimuthAndAltitude(for: touch)
    let isPencilInput = PencilStrokeInputNormalizer.isPencil(touch)
    let scaledX = Float(location.x * scaleX)
    let scaledY = Float(location.y * scaleY)
    let timestamp = Int64(touch.timestamp * 1000)  // Convert to milliseconds
    touchMoved(engine, scaledX, scaledY, pressure, azimuth, altitude, timestamp, isPencilInput)
    recordBenchmarkInputSample()
  }

  // PREDICTIVE TOUCH: Process predicted touch samples for Apple Pencil low-latency rendering
  // These points are rendered temporarily and replaced by actual data on next touch event
  private func processPredictedTouchSample(engine: OpaquePointer, touch: UITouch) {
    let location = touch.location(in: self)
    let pressure = normalizedPressure(for: touch)
    let (azimuth, altitude) = extractAzimuthAndAltitude(for: touch)
    let isPencilInput = PencilStrokeInputNormalizer.isPencil(touch)
    let scaledX = Float(location.x * scaleX)
    let scaledY = Float(location.y * scaleY)
    let timestamp = Int64(touch.timestamp * 1000)
    addPredictedPoint(engine, scaledX, scaledY, pressure, azimuth, altitude, timestamp, isPencilInput)
  }

  private func allocatePixelBuffer(width: Int, height: Int) {
    let bytesPerPixel = 4
    let bytesPerRow = width * bytesPerPixel
    let length = bytesPerRow * height

    if width <= 0 || height <= 0 || length <= 0 {
      pixelBuffer?.deallocate()
      pixelBuffer = nil
      pixelBufferLength = 0
      pixelBytesPerRow = 0
      return
    }

    if length != pixelBufferLength {
      pixelBuffer?.deallocate()
      pixelBuffer = UnsafeMutablePointer<UInt8>.allocate(capacity: length)
      pixelBufferLength = length
    }
    pixelBytesPerRow = bytesPerRow
  }

}
