import Darwin
import Foundation
import QuartzCore

enum MobileInkRenderBackend: String {
  case cpu
  case ganesh
}

final class MobileInkBenchmarkRecorder {
  private let frameBudgetMultiplier = 1.5
  private let presentationPauseThresholdMs = 250.0
  private(set) var isRunning = false
  private var sessionId = UUID().uuidString
  private var scenario = "custom"
  private var requestedBackend = MobileInkRenderBackend.ganesh.rawValue
  private var startTime = CACurrentMediaTime()
  private var endTime = CACurrentMediaTime()
  private var lastPresentedAt: CFTimeInterval?
  private var pendingInputTimes: [CFTimeInterval] = []
  private var renderDurationsMs: [Double] = []
  private var presentIntervalsMs: [Double] = []
  private var presentationPauseDurationsMs: [Double] = []
  private var inputToPresentLatenciesMs: [Double] = []
  private var renderFrameCount = 0
  private var cpuFrameCount = 0
  private var ganeshFrameCount = 0
  private var ganeshFallbackFrameCount = 0
  private var droppedFrameCount = 0
  private var inputEventCount = 0
  private var syntheticStrokeCount = 0
  private var syntheticPointCount = 0
  private var memoryStartBytes: UInt64 = 0
  private var memoryEndBytes: UInt64 = 0
  private var memoryPeakBytes: UInt64 = 0
  private var memoryLowBytes: UInt64 = 0

  func start(scenario: String, requestedBackend: String) {
    self.sessionId = UUID().uuidString
    self.scenario = scenario
    self.requestedBackend = requestedBackend
    self.startTime = CACurrentMediaTime()
    self.endTime = startTime
    self.lastPresentedAt = nil
    self.pendingInputTimes.removeAll(keepingCapacity: true)
    self.renderDurationsMs.removeAll(keepingCapacity: true)
    self.presentIntervalsMs.removeAll(keepingCapacity: true)
    self.presentationPauseDurationsMs.removeAll(keepingCapacity: true)
    self.inputToPresentLatenciesMs.removeAll(keepingCapacity: true)
    self.renderFrameCount = 0
    self.cpuFrameCount = 0
    self.ganeshFrameCount = 0
    self.ganeshFallbackFrameCount = 0
    self.droppedFrameCount = 0
    self.inputEventCount = 0
    self.syntheticStrokeCount = 0
    self.syntheticPointCount = 0
    self.memoryStartBytes = Self.currentResidentMemoryBytes()
    self.memoryEndBytes = memoryStartBytes
    self.memoryPeakBytes = memoryStartBytes
    self.memoryLowBytes = memoryStartBytes
    self.isRunning = true
  }

  func finish() -> [String: Any] {
    sampleMemory()
    endTime = CACurrentMediaTime()
    isRunning = false
    return summary()
  }

  func recordInputSample(at timestamp: CFTimeInterval = CACurrentMediaTime()) {
    guard isRunning else { return }
    inputEventCount += 1
    pendingInputTimes.append(timestamp)
  }

  func recordSyntheticStroke() {
    guard isRunning else { return }
    syntheticStrokeCount += 1
  }

  func recordSyntheticPoint() {
    guard isRunning else { return }
    syntheticPointCount += 1
  }

  func recordPresentedFrame(
    backend: MobileInkRenderBackend,
    didFallbackFromGanesh: Bool,
    renderDurationMs: Double,
    completedAt: CFTimeInterval,
    maximumFramesPerSecond: Int
  ) {
    guard isRunning else { return }

    renderFrameCount += 1
    renderDurationsMs.append(renderDurationMs)

    switch backend {
    case .cpu:
      cpuFrameCount += 1
    case .ganesh:
      ganeshFrameCount += 1
    }

    if didFallbackFromGanesh {
      ganeshFallbackFrameCount += 1
    }

    if let lastPresentedAt {
      let intervalMs = (completedAt - lastPresentedAt) * 1000.0
      if intervalMs > presentationPauseThresholdMs {
        presentationPauseDurationsMs.append(intervalMs)
      } else {
        presentIntervalsMs.append(intervalMs)
        let frameBudgetMs = 1000.0 / Double(max(1, maximumFramesPerSecond))
        if intervalMs > frameBudgetMs * frameBudgetMultiplier {
          droppedFrameCount += max(1, Int(intervalMs / frameBudgetMs) - 1)
        }
      }
    }
    lastPresentedAt = completedAt

    if !pendingInputTimes.isEmpty {
      for inputTime in pendingInputTimes {
        inputToPresentLatenciesMs.append((completedAt - inputTime) * 1000.0)
      }
      pendingInputTimes.removeAll(keepingCapacity: true)
    }

    sampleMemory()
  }

  func summary() -> [String: Any] {
    let durationMs = max(0.0, (endTime - startTime) * 1000.0)
    let durationSeconds = max(0.001, durationMs / 1000.0)
    let fpsAverage = Double(renderFrameCount) / durationSeconds
    let presentationPauseTotalMs = presentationPauseDurationsMs.reduce(0, +)
    let memoryDeltaBytes = Int64(memoryEndBytes) - Int64(memoryStartBytes)

    return [
      "sessionId": sessionId,
      "scenario": scenario,
      "requestedBackend": requestedBackend,
      "durationMs": rounded(durationMs),
      "fpsAverage": rounded(fpsAverage),
      "renderFrameCount": renderFrameCount,
      "cpuFrameCount": cpuFrameCount,
      "ganeshFrameCount": ganeshFrameCount,
      "ganeshFallbackFrameCount": ganeshFallbackFrameCount,
      "droppedFrameCount": droppedFrameCount,
      "inputEventCount": inputEventCount,
      "syntheticStrokeCount": syntheticStrokeCount,
      "syntheticPointCount": syntheticPointCount,
      "renderMs": distribution(renderDurationsMs),
      "frameIntervalMs": distribution(presentIntervalsMs),
      "presentationPauseMs": distribution(presentationPauseDurationsMs),
      "presentationPauseCount": presentationPauseDurationsMs.count,
      "presentationPauseTotalMs": rounded(presentationPauseTotalMs),
      "inputToPresentLatencyMs": distribution(inputToPresentLatenciesMs),
      "memory": [
        "startBytes": memoryStartBytes,
        "endBytes": memoryEndBytes,
        "peakBytes": memoryPeakBytes,
        "lowBytes": memoryLowBytes,
        "deltaBytes": memoryDeltaBytes,
        "peakDeltaBytes": Int64(memoryPeakBytes) - Int64(memoryStartBytes),
      ],
    ]
  }

  private func sampleMemory() {
    let current = Self.currentResidentMemoryBytes()
    memoryEndBytes = current
    memoryPeakBytes = max(memoryPeakBytes, current)
    if memoryLowBytes == 0 {
      memoryLowBytes = current
    } else {
      memoryLowBytes = min(memoryLowBytes, current)
    }
  }

  private func distribution(_ values: [Double]) -> [String: Any] {
    guard !values.isEmpty else {
      return [
        "count": 0,
        "average": 0,
        "p50": 0,
        "p95": 0,
        "p99": 0,
        "max": 0,
      ]
    }

    let sorted = values.sorted()
    let average = values.reduce(0, +) / Double(values.count)

    return [
      "count": values.count,
      "average": rounded(average),
      "p50": rounded(percentile(sorted, 0.50)),
      "p95": rounded(percentile(sorted, 0.95)),
      "p99": rounded(percentile(sorted, 0.99)),
      "max": rounded(sorted.last ?? 0),
    ]
  }

  private func percentile(_ sortedValues: [Double], _ percentile: Double) -> Double {
    guard !sortedValues.isEmpty else { return 0 }
    let boundedPercentile = min(1.0, max(0.0, percentile))
    let rawIndex = boundedPercentile * Double(sortedValues.count - 1)
    let lowerIndex = Int(floor(rawIndex))
    let upperIndex = Int(ceil(rawIndex))
    if lowerIndex == upperIndex {
      return sortedValues[lowerIndex]
    }
    let fraction = rawIndex - Double(lowerIndex)
    return sortedValues[lowerIndex] * (1.0 - fraction) + sortedValues[upperIndex] * fraction
  }

  private func rounded(_ value: Double) -> Double {
    (value * 100.0).rounded() / 100.0
  }

  private static func currentResidentMemoryBytes() -> UInt64 {
    var info = mach_task_basic_info()
    var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size) / 4
    let result: kern_return_t = withUnsafeMutablePointer(to: &info) {
      $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
        task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count)
      }
    }

    guard result == KERN_SUCCESS else {
      return 0
    }

    return UInt64(info.resident_size)
  }
}
