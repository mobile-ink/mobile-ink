import Metal
import QuartzCore
import React
import UIKit

extension MobileInkCanvasView {
  func attachBenchmarkFrameCallback(
    to commandBuffer: MTLCommandBuffer,
    backend: MobileInkRenderBackend,
    didFallbackFromGanesh: Bool,
    renderDurationMs: Double
  ) {
    guard benchmarkRecorder.isRunning else {
      return
    }

    let maximumFramesPerSecond = max(1, UIScreen.main.maximumFramesPerSecond)
    commandBuffer.addCompletedHandler { [weak self] _ in
      let completedAt = CACurrentMediaTime()
      DispatchQueue.main.async {
        self?.benchmarkRecorder.recordPresentedFrame(
          backend: backend,
          didFallbackFromGanesh: didFallbackFromGanesh,
          renderDurationMs: renderDurationMs,
          completedAt: completedAt,
          maximumFramesPerSecond: maximumFramesPerSecond
        )
      }
    }
  }

  func recordBenchmarkInputSample() {
    benchmarkRecorder.recordInputSample()
  }

  @objc func runBenchmark(_ options: NSDictionary, callback: @escaping RCTResponseSenderBlock) {
    guard !isReleased else {
      callback(["Canvas has already been released", NSNull()])
      return
    }

    guard drawingEngine != nil, pixelWidth > 0, pixelHeight > 0, bounds.width > 0, bounds.height > 0 else {
      callback(["Canvas is not ready for benchmarking", NSNull()])
      return
    }

    guard !isBenchmarkReplayRunning else {
      callback(["Benchmark is already running", NSNull()])
      return
    }

    let scenario = benchmarkStringOption(options, key: "scenario", defaultValue: "custom")
    let backendName = benchmarkStringOption(options, key: "backend", defaultValue: renderBackend)
    let backend = MobileInkRenderBackend(rawValue: backendName.lowercased()) ?? requestedRenderBackend
    let toolType = benchmarkStringOption(options, key: "toolType", defaultValue: "pen")
    let workload = benchmarkStringOption(
      options,
      key: "workload",
      defaultValue: toolType == "eraser" ? "erase" : "draw"
    )
    let strokeCount = benchmarkIntOption(options, key: "strokeCount", defaultValue: 120, minimum: 1, maximum: 2_000)
    let pointsPerStroke = benchmarkIntOption(options, key: "pointsPerStroke", defaultValue: 28, minimum: 2, maximum: 240)
    let pointIntervalMs = benchmarkDoubleOption(options, key: "pointIntervalMs", defaultValue: 8, minimum: 1, maximum: 100)
    let strokeGapMs = benchmarkDoubleOption(options, key: "strokeGapMs", defaultValue: 18, minimum: 0, maximum: 1_000)
    let settleMs = benchmarkDoubleOption(options, key: "settleMs", defaultValue: 500, minimum: 0, maximum: 5_000)
    let strokeWidth = CGFloat(benchmarkDoubleOption(options, key: "strokeWidth", defaultValue: 3, minimum: 0.25, maximum: 96))
    let seedStrokeCount = benchmarkIntOption(
      options,
      key: "seedStrokeCount",
      defaultValue: max(16, min(160, strokeCount / 2)),
      minimum: 1,
      maximum: 1_000
    )
    let moveStepCount = benchmarkIntOption(options, key: "moveStepCount", defaultValue: 80, minimum: 1, maximum: 1_000)
    let moveDeltaX = benchmarkDoubleOption(options, key: "moveDeltaX", defaultValue: 1.5, minimum: -64, maximum: 64)
    let moveDeltaY = benchmarkDoubleOption(options, key: "moveDeltaY", defaultValue: 0.75, minimum: -64, maximum: 64)
    let eraserMode = benchmarkStringOption(options, key: "eraserMode", defaultValue: "pixel")
    let toolColor = benchmarkColorOption(
      options,
      defaultValue: benchmarkDefaultToolColor(toolType)
    )
    let shouldClearCanvas = benchmarkBoolOption(options, key: "clearCanvas", defaultValue: true)

    renderBackend = backend.rawValue

    guard let engine = drawingEngine else {
      callback(["Engine became unavailable before benchmark start", NSNull()])
      return
    }

    if shouldClearCanvas {
      clearPredictedPoints(engine)
      clearCanvas(engine)
    }
    resetTransientInteractionState()

    if workload == "erase" {
      setTool("pen", width: Float(max(2, strokeWidth)), color: .black, eraserMode: "pixel")
      benchmarkSeedStrokes(
        engine,
        strokeCount: seedStrokeCount,
        pointsPerStroke: pointsPerStroke
      )
      setTool("eraser", width: Float(max(strokeWidth, 32)), color: .white, eraserMode: eraserMode)
    } else if workload == "selectionMove" {
      setTool("pen", width: Float(max(2, strokeWidth)), color: .black, eraserMode: "pixel")
      benchmarkSeedStrokes(
        engine,
        strokeCount: seedStrokeCount,
        pointsPerStroke: pointsPerStroke
      )
      setTool("select", width: Float(strokeWidth), color: .black, eraserMode: "pixel")
    } else {
      setTool(toolType, width: Float(strokeWidth), color: toolColor, eraserMode: eraserMode)
    }

    requestDisplay(forceWhenSuspended: true)

    isBenchmarkReplayRunning = true
    let token = UUID()
    benchmarkRunToken = token
    benchmarkRecorder.start(scenario: scenario, requestedBackend: backend.rawValue)

    var strokeIndex = 0
    var pointIndex = 0
    var didComplete = false

    func complete(_ error: String?, _ result: [String: Any]?) {
      guard !didComplete else { return }
      didComplete = true
      isBenchmarkReplayRunning = false
      isHoldToShapeStrokeActive = false
      cancelHoldToShapePreview()

      if let error {
        if benchmarkRecorder.isRunning {
          _ = benchmarkRecorder.finish()
        }
        callback([error, NSNull()])
      } else {
        onDrawingChange?([:])
        callback([NSNull(), result ?? benchmarkRecorder.finish()])
      }
    }

    if workload == "selectionMove" {
      benchmarkRecorder.recordSyntheticStroke()
      benchmarkRecorder.recordSyntheticPoint()
      recordBenchmarkInputSample()

      let selectionPoint = benchmarkReplayPoint(
        strokeIndex: 0,
        pointIndex: max(1, pointsPerStroke / 2),
        pointsPerStroke: pointsPerStroke
      )
      clearSelection(engine)
      _ = selectStrokeAt(
        engine,
        Float(selectionPoint.x * scaleX),
        Float(selectionPoint.y * scaleY)
      )
      notifySelectionChange()
      requestDisplay()

      var moveIndex = 0
      var replayNextMove: (() -> Void)?
      replayNextMove = { [weak self] in
        guard let self = self else { return }
        guard token == self.benchmarkRunToken, self.isBenchmarkReplayRunning else { return }
        guard let engine = self.drawingEngine else {
          complete("Engine became unavailable during selection benchmark", nil)
          return
        }

        if moveIndex >= moveStepCount {
          finalizeMove(engine)
          self.requestDisplay()
          DispatchQueue.main.asyncAfter(deadline: .now() + settleMs / 1_000.0) { [weak self] in
            guard let self = self else { return }
            guard token == self.benchmarkRunToken else { return }
            complete(nil, self.benchmarkRecorder.finish())
          }
          return
        }

        self.benchmarkRecorder.recordSyntheticPoint()
        self.recordBenchmarkInputSample()
        moveSelection(
          engine,
          Float(moveDeltaX) * Float(self.scaleX),
          Float(moveDeltaY) * Float(self.scaleY)
        )
        self.notifySelectionChange()
        self.requestDisplay()
        moveIndex += 1

        DispatchQueue.main.asyncAfter(deadline: .now() + pointIntervalMs / 1_000.0) {
          replayNextMove?()
        }
      }

      DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
        replayNextMove?()
      }
      return
    }

    var replayNextPoint: (() -> Void)?
    replayNextPoint = { [weak self] in
      guard let self = self else { return }
      guard token == self.benchmarkRunToken, self.isBenchmarkReplayRunning else { return }
      guard let engine = self.drawingEngine else {
        complete("Engine became unavailable during benchmark", nil)
        return
      }

      if strokeIndex >= strokeCount {
        DispatchQueue.main.asyncAfter(deadline: .now() + settleMs / 1_000.0) { [weak self] in
          guard let self = self else { return }
          guard token == self.benchmarkRunToken else { return }
          complete(nil, self.benchmarkRecorder.finish())
        }
        return
      }

      if pointIndex == 0 {
        let point = self.benchmarkReplayPoint(
          strokeIndex: strokeIndex,
          pointIndex: pointIndex,
          pointsPerStroke: pointsPerStroke
        )
        self.benchmarkRecorder.recordSyntheticStroke()
        self.benchmarkRecorder.recordSyntheticPoint()
        self.recordBenchmarkInputSample()
        touchBegan(
          engine,
          Float(point.x * self.scaleX),
          Float(point.y * self.scaleY),
          1.0,
          0.0,
          PencilStrokeInputNormalizer.perpendicularAltitude,
          self.currentUptimeTimestampMillis(),
          true
        )
        self.requestDisplay()
        pointIndex += 1
      } else if pointIndex < pointsPerStroke {
        let point = self.benchmarkReplayPoint(
          strokeIndex: strokeIndex,
          pointIndex: pointIndex,
          pointsPerStroke: pointsPerStroke
        )
        self.benchmarkRecorder.recordSyntheticPoint()
        self.recordBenchmarkInputSample()
        touchMoved(
          engine,
          Float(point.x * self.scaleX),
          Float(point.y * self.scaleY),
          1.0,
          0.0,
          PencilStrokeInputNormalizer.perpendicularAltitude,
          self.currentUptimeTimestampMillis(),
          true
        )
        self.requestDisplay()
        pointIndex += 1
      } else {
        clearPredictedPoints(engine)
        self.recordBenchmarkInputSample()
        touchEnded(engine, self.currentUptimeTimestampMillis())
        self.requestDisplay()
        strokeIndex += 1
        pointIndex = 0
        DispatchQueue.main.asyncAfter(deadline: .now() + strokeGapMs / 1_000.0) {
          replayNextPoint?()
        }
        return
      }

      DispatchQueue.main.asyncAfter(deadline: .now() + pointIntervalMs / 1_000.0) {
        replayNextPoint?()
      }
    }

    DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
      replayNextPoint?()
    }
  }

  @objc func startBenchmarkRecording(_ options: NSDictionary, callback: @escaping RCTResponseSenderBlock) {
    guard !isReleased else {
      callback(["Canvas has already been released", NSNull()])
      return
    }

    guard drawingEngine != nil, pixelWidth > 0, pixelHeight > 0, bounds.width > 0, bounds.height > 0 else {
      callback(["Canvas is not ready for benchmarking", NSNull()])
      return
    }

    guard !isBenchmarkReplayRunning else {
      callback(["Synthetic benchmark is already running", NSNull()])
      return
    }

    guard !benchmarkRecorder.isRunning else {
      callback(["Benchmark recording is already running", NSNull()])
      return
    }

    let scenario = benchmarkStringOption(options, key: "scenario", defaultValue: "manual")
    let backendName = benchmarkStringOption(options, key: "backend", defaultValue: renderBackend)
    let backend = MobileInkRenderBackend(rawValue: backendName.lowercased()) ?? requestedRenderBackend
    let shouldClearCanvas = (options["clearCanvas"] as? NSNumber)?.boolValue ?? false

    renderBackend = backend.rawValue

    if shouldClearCanvas, let engine = drawingEngine {
      clearPredictedPoints(engine)
      clearCanvas(engine)
      resetTransientInteractionState()
      requestDisplay(forceWhenSuspended: true)
    }

    benchmarkRecorder.start(scenario: scenario, requestedBackend: backend.rawValue)
    callback([NSNull(), true])
  }

  @objc func stopBenchmarkRecording(_ callback: @escaping RCTResponseSenderBlock) {
    guard !isBenchmarkReplayRunning else {
      callback(["Synthetic benchmark is still running", NSNull()])
      return
    }

    guard benchmarkRecorder.isRunning else {
      callback(["Benchmark recording is not running", NSNull()])
      return
    }

    callback([NSNull(), benchmarkRecorder.finish()])
  }

  private func benchmarkReplayPoint(
    strokeIndex: Int,
    pointIndex: Int,
    pointsPerStroke: Int
  ) -> CGPoint {
    let inset: CGFloat = 36
    let availableWidth = max(1, bounds.width - inset * 2)
    let availableHeight = max(1, bounds.height - inset * 2)
    let rowSpacing: CGFloat = 28
    let rowCount = max(1, Int(availableHeight / rowSpacing))
    let row = strokeIndex % rowCount
    let wrap = strokeIndex / rowCount
    let denominator = max(1, pointsPerStroke - 1)
    let progress = CGFloat(pointIndex) / CGFloat(denominator)
    let baselineY = inset + CGFloat(row) * rowSpacing + CGFloat(wrap % 4) * 3
    let wave = sin(progress * .pi * 4 + CGFloat(strokeIndex) * 0.37) * 8
    let x = inset + progress * availableWidth
    let y = min(max(inset, baselineY + wave), bounds.height - inset)
    return CGPoint(x: x, y: y)
  }

  private func benchmarkSeedStrokes(
    _ engine: OpaquePointer,
    strokeCount: Int,
    pointsPerStroke: Int
  ) {
    for strokeIndex in 0..<strokeCount {
      for pointIndex in 0..<pointsPerStroke {
        let point = benchmarkReplayPoint(
          strokeIndex: strokeIndex,
          pointIndex: pointIndex,
          pointsPerStroke: pointsPerStroke
        )
        let timestamp = currentUptimeTimestampMillis()
        if pointIndex == 0 {
          touchBegan(
            engine,
            Float(point.x * scaleX),
            Float(point.y * scaleY),
            1.0,
            0.0,
            PencilStrokeInputNormalizer.perpendicularAltitude,
            timestamp,
            true
          )
        } else {
          touchMoved(
            engine,
            Float(point.x * scaleX),
            Float(point.y * scaleY),
            1.0,
            0.0,
            PencilStrokeInputNormalizer.perpendicularAltitude,
            timestamp,
            true
          )
        }
      }

      clearPredictedPoints(engine)
      touchEnded(engine, currentUptimeTimestampMillis())
    }
  }

  private func benchmarkDefaultToolColor(_ toolType: String) -> UIColor {
    switch toolType {
    case "highlighter":
      return UIColor(red: 1.0, green: 0.88, blue: 0.32, alpha: 1.0)
    case "crayon":
      return UIColor(red: 0.12, green: 0.42, blue: 1.0, alpha: 1.0)
    case "eraser":
      return .white
    default:
      return .black
    }
  }

  private func benchmarkColorOption(
    _ options: NSDictionary,
    defaultValue: UIColor
  ) -> UIColor {
    guard let color = options["color"] as? String else {
      return defaultValue
    }
    return UIColor(hex: color) ?? defaultValue
  }

  private func benchmarkStringOption(
    _ options: NSDictionary,
    key: String,
    defaultValue: String
  ) -> String {
    options[key] as? String ?? defaultValue
  }

  private func benchmarkIntOption(
    _ options: NSDictionary,
    key: String,
    defaultValue: Int,
    minimum: Int,
    maximum: Int
  ) -> Int {
    let value: Int
    if let number = options[key] as? NSNumber {
      value = number.intValue
    } else if let string = options[key] as? String, let parsed = Int(string) {
      value = parsed
    } else {
      value = defaultValue
    }
    return min(max(value, minimum), maximum)
  }

  private func benchmarkDoubleOption(
    _ options: NSDictionary,
    key: String,
    defaultValue: Double,
    minimum: Double,
    maximum: Double
  ) -> Double {
    let value: Double
    if let number = options[key] as? NSNumber {
      value = number.doubleValue
    } else if let string = options[key] as? String, let parsed = Double(string) {
      value = parsed
    } else {
      value = defaultValue
    }
    return min(max(value, minimum), maximum)
  }

  private func benchmarkBoolOption(
    _ options: NSDictionary,
    key: String,
    defaultValue: Bool
  ) -> Bool {
    if let number = options[key] as? NSNumber {
      return number.boolValue
    }
    if let string = options[key] as? String {
      let normalized = string.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
      if ["true", "1", "yes"].contains(normalized) {
        return true
      }
      if ["false", "0", "no"].contains(normalized) {
        return false
      }
    }
    return defaultValue
  }
}
