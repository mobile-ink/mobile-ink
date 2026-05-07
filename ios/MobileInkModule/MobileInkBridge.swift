import Foundation
import React
import PDFKit
import CoreGraphics

// Convert a path string from JS into a Swift URL. JS callers using
// expo-file-system pass paths with a "file://" prefix (e.g.
// "file:///var/mobile/.../foo.body"). Plain `URL(fileURLWithPath:)`
// double-encodes that prefix and produces a broken URL, which manifests
// as "The folder X doesn't exist" from Data.write(:options:.atomic).
// Detect the prefix and route through URL(string:) for those; fall back
// to fileURLWithPath for raw paths.
//
// File-scope (rather than method on the class) so callers inside nested
// async closures don't need to recapture `self` to use these helpers.
private func bridgeFileURL(fromPath path: String) -> URL {
  if path.hasPrefix("file://") {
    if let url = URL(string: path) {
      return url
    }
  }
  return URL(fileURLWithPath: path)
}

// POSIX-style path string for FileManager calls that don't accept URLs
// (e.g. fileExists(atPath:)). Strips the file:// prefix if present.
private func bridgePosixPath(fromPath path: String) -> String {
  return bridgeFileURL(fromPath: path).path
}

@objc(MobileInkBridge)
class MobileInkBridge: RCTEventEmitter {
  private let blankPagePayload = "{\"pages\":{}}"

  override func supportedEvents() -> [String]! {
    return []
  }

  override static func requiresMainQueueSetup() -> Bool {
    return true
  }

  private func getView(forTag viewTag: NSNumber) -> MobileInkCanvasView? {
    // The view at the React tag is a CONTAINER (a wrapper UIView the JS layer
    // mounts), not the MobileInkCanvasView itself. Drill into the container's
    // subviews to find the actual drawing view -- mirrors the pattern in
    // MobileInkCanvasViewManager.findDrawingView. Without this drill-down the
    // direct `as?` cast fails and bridge methods fail with VIEW_NOT_FOUND
    // every save (observed during testing as a hard fallback to the slow path).
    guard let container = bridge.uiManager.view(forReactTag: viewTag) else {
      return nil
    }
    if let drawingView = container as? MobileInkCanvasView {
      return drawingView
    }
    for subview in container.subviews {
      if let drawingView = subview as? MobileInkCanvasView {
        return drawingView
      }
    }
    return nil
  }

  private func extractDrawingBinary(from pagePayload: String) -> Data? {
    guard
      let jsonData = pagePayload.data(using: .utf8),
      let jsonObject = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
      let pages = jsonObject["pages"] as? [String: String],
      let base64Payload = pages["0"],
      let binaryData = Data(base64Encoded: base64Payload),
      !binaryData.isEmpty
    else {
      return nil
    }

    return binaryData
  }

  private func makePagePayload(from drawingBinary: Data?) -> String {
    guard let drawingBinary, !drawingBinary.isEmpty else {
      return blankPagePayload
    }

    let base64Payload = drawingBinary.base64EncodedString()
    let pageDict: [String: Any] = [
      "pages": [
        "0": base64Payload,
      ],
    ]

    guard
      let jsonData = try? JSONSerialization.data(withJSONObject: pageDict),
      let jsonString = String(data: jsonData, encoding: .utf8)
    else {
      return blankPagePayload
    }

    return jsonString
  }

  // MARK: - Data Export/Import

  @objc func getBase64Data(
    _ viewTag: NSNumber,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let view = self?.getView(forTag: viewTag) else {
        rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
        return
      }

      view.getBase64Data { result in
        guard let array = result as? [Any], array.count >= 2 else {
          resolver("")
          return
        }

        // Format: [error, data] where error is String or NSNull, data is String or NSNull
        let errorValue = array[0]
        let dataValue = array[1]

        if let error = errorValue as? String {
          rejecter("GET_DATA_ERROR", error, nil)
        } else if let data = dataValue as? String {
          resolver(data)
        } else {
          resolver("")
        }
      }
    }
  }

  @objc func loadBase64Data(
    _ viewTag: NSNumber,
    base64String: String,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let view = self?.getView(forTag: viewTag) else {
        rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
        return
      }

      view.loadBase64Data(base64String) { result in
        guard let array = result as? [Any], array.count >= 2 else {
          resolver(false)
          return
        }

        // Format: [error, success] where error is String or NSNull, success is Bool or NSNull
        let errorValue = array[0]
        let successValue = array[1]

        if let error = errorValue as? String {
          rejecter("LOAD_DATA_ERROR", error, nil)
        } else if let success = successValue as? Bool {
          resolver(success)
        } else {
          resolver(false)
        }
      }
    }
  }

  // MARK: - Native-side persistence
  //
  // persistEngineToFile and loadEngineFromFile are the bridge-bypass save/load
  // path for autosave. Conceptually they're identical to getBase64Data/
  // loadBase64Data, but the body bytes never cross the JS<->native bridge --
  // the file write/read happens in Swift, and the bridge transfers only the
  // file path (small) and a success bool back. On heavy notebooks where the
  // body is many MB this is the difference between a 100-200 ms autosave
  // (visible draw chop) and a sub-10 ms autosave.
  //
  // Atomicity: writes use Data.write(to:options:.atomic), which writes to a
  // temp file and renames into place. Partial writes can't poison the final
  // path even under memory pressure.

  @objc func persistEngineToFile(
    _ viewTag: NSNumber,
    filePath: String,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let view = self?.getView(forTag: viewTag) else {
        rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
        return
      }

      // The view's internal getBase64Data callback runs the C++ serialize off
      // the main thread already. We treat its result as a Swift-local string
      // and write it directly to disk -- no bridge crossing.
      view.getBase64Data { result in
        guard let array = result as? [Any], array.count >= 2 else {
          resolver(false)
          return
        }

        let errorValue = array[0]
        let dataValue = array[1]

        if let error = errorValue as? String {
          rejecter("GET_DATA_ERROR", error, nil)
          return
        }

        guard let data = dataValue as? String, !data.isEmpty else {
          // Empty engine state. Persist as an empty file so a subsequent load
          // gets a clean slate; an absent file is treated by the JS layer as
          // "no body."
          DispatchQueue.global(qos: .userInitiated).async {
            do {
              let url = bridgeFileURL(fromPath: filePath)
              try Data().write(to: url, options: .atomic)
              DispatchQueue.main.async { resolver(true) }
            } catch {
              DispatchQueue.main.async {
                rejecter("WRITE_FAILED", error.localizedDescription, error)
              }
            }
          }
          return
        }

        DispatchQueue.global(qos: .userInitiated).async {
          autoreleasepool {
            do {
              let url = bridgeFileURL(fromPath: filePath)
              let parentDir = url.deletingLastPathComponent()
              try? FileManager.default.createDirectory(
                at: parentDir,
                withIntermediateDirectories: true,
                attributes: nil
              )

              guard let bodyData = data.data(using: .utf8) else {
                DispatchQueue.main.async {
                  rejecter("WRITE_FAILED", "Failed to encode body as UTF-8", nil)
                }
                return
              }

              try bodyData.write(to: url, options: .atomic)
              DispatchQueue.main.async { resolver(true) }
            } catch {
              DispatchQueue.main.async {
                rejecter("WRITE_FAILED", error.localizedDescription, error)
              }
            }
          }
        }
      }
    }
  }

  // MARK: - Body file native read+parse
  //
  // readBodyFileParsed reads a notebook body file in native and parses it
  // entirely in C++ (via NSJSONSerialization). The structured result is
  // returned to JS as a Foundation object that React Native passes through
  // the bridge -- under the new architecture this avoids round-tripping a
  // multi-MB string through Hermes JSON.parse, which is the dominant cost
  // of opening a heavy notebook.
  //
  // Resolves with NSNull when the file is missing (let JS treat it as a
  // new file). Rejects on actual read or parse errors so the caller can
  // fall back to the slow path.

  @objc func readBodyFileParsed(
    _ bodyPath: String,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.global(qos: .userInitiated).async {
      autoreleasepool {
        guard FileManager.default.fileExists(atPath: bridgePosixPath(fromPath: bodyPath)) else {
          DispatchQueue.main.async { resolver(NSNull()) }
          return
        }
        do {
          let bodyData = try Data(contentsOf: bridgeFileURL(fromPath: bodyPath))
          if bodyData.isEmpty {
            DispatchQueue.main.async { resolver(NSNull()) }
            return
          }
          let parsed = try JSONSerialization.jsonObject(with: bodyData)
          DispatchQueue.main.async { resolver(parsed) }
        } catch {
          DispatchQueue.main.async {
            rejecter("READ_PARSE_FAILED", error.localizedDescription, error)
          }
        }
      }
    }
  }

  // MARK: - Full-notebook native persistence (read-mutate-write)
  //
  // persistFullNotebookToFile is the autosave fast-path that owns the entire
  // body file in native: reads the existing body, replaces ONLY the visible
  // window's per-page data with the engine's current state, writes back
  // atomically. The body bytes never cross the JS<->native bridge.
  //
  // JS contract:
  //   - visiblePageIds: IDs of pages currently loaded in the engine, in window order
  //   - pagesMetadata: full list of ALL notebook pages with metadata only (id,
  //     title, rotation, pageType, etc.) -- no `data` field. The data fields come
  //     from either the engine (visible pages) or the existing body file
  //     (non-visible pages).
  //   - originalCanvasWidth, pageHeight: needed for the decomposition step
  //   - bodyPath: where to write the resulting notebook JSON
  //
  // The notebook JSON shape this writes matches what the JS layer already
  // produces via serializeNotebookData -- so existing readers work unchanged.

  @objc func persistFullNotebookToFile(
    _ viewTag: NSNumber,
    visiblePageIds: [String],
    pagesMetadata: [[String: Any]],
    originalCanvasWidth: NSNumber?,
    pageHeight: CGFloat,
    bodyPath: String,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      guard let view = self.getView(forTag: viewTag) else {
        rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
        return
      }

      view.getBase64Data { result in
        guard let array = result as? [Any], array.count >= 2 else {
          rejecter("GET_DATA_ERROR", "Invalid result format from engine", nil)
          return
        }

        let errorValue = array[0]
        let dataValue = array[1]

        if let error = errorValue as? String {
          rejecter("GET_DATA_ERROR", error, nil)
          return
        }

        let windowPayload = (dataValue as? String) ?? self.blankPagePayload

        DispatchQueue.global(qos: .userInitiated).async {
          // CRITICAL: wrap in autoreleasepool. Each call allocates a chain of
          // MB-scale Foundation objects (windowPayload Data, NSDictionary
          // metadata, JSONSerialization output, the existing body Data when
          // reading-to-mutate). Without an explicit pool here those autoreleased
          // objects drain only when the work item finishes -- and on a heavy
          // session with autosaves firing every 500 ms the queue can accumulate
          // overlapping work fast enough to OOM. The pool drains within the
          // closure so each save's allocations are freed promptly.
          autoreleasepool {
            do {
              let mergedNotebook = try self.mergeEngineStateIntoNotebookJson(
                windowPayload: windowPayload,
                visiblePageIds: visiblePageIds,
                pagesMetadata: pagesMetadata,
                originalCanvasWidth: originalCanvasWidth?.doubleValue,
                pageHeight: pageHeight,
                existingBodyPath: bodyPath
              )

              let jsonData = try JSONSerialization.data(withJSONObject: mergedNotebook, options: [])

              let url = bridgeFileURL(fromPath: bodyPath)
              let parentDir = url.deletingLastPathComponent()
              try? FileManager.default.createDirectory(
                at: parentDir,
                withIntermediateDirectories: true,
                attributes: nil
              )

              try jsonData.write(to: url, options: .atomic)
              DispatchQueue.main.async { resolver(true) }
            } catch {
              DispatchQueue.main.async {
                rejecter("PERSIST_FAILED", error.localizedDescription, error)
              }
            }
          }
        }
      }
    }
  }

  // Read-mutate-merge helper: takes the engine's combined-window payload,
  // splits it into per-page payloads, then assembles the full notebook JSON
  // by merging engine-fresh visible pages with on-disk non-visible pages.
  // Pure Swift (no React/UI dependencies); safe to call from a background queue.
  private func mergeEngineStateIntoNotebookJson(
    windowPayload: String,
    visiblePageIds: [String],
    pagesMetadata: [[String: Any]],
    originalCanvasWidth: Double?,
    pageHeight: CGFloat,
    existingBodyPath: String
  ) throws -> [String: Any] {
    // Decompose the engine's combined window into per-page payloads in the
    // same single-page wrapper format as legacy storage:
    // {"pages":{"0":"<base64>"}}
    let perPagePayloads = decomposeWindowPayloadSync(
      windowPayload: windowPayload,
      pageCount: visiblePageIds.count,
      pageHeight: pageHeight
    )

    // Map visible page IDs to their fresh-from-engine per-page payloads.
    //
    // SAFETY: When the engine reports a BLANK payload for a visible page, do
    // NOT add it to the map -- we'll fall through to the existing on-disk
    // data below. This protects against data loss when the canvas remounts
    // (the new engine starts empty before strokes load) and an autosave
    // fires in that brief window. The cost is that legitimate "user erased
    // the whole page" cases won't be reflected through the fast path -- they
    // need the slow path's full notebook write to take effect, which is the
    // safer trade than ever overwriting real strokes with blanks.
    var visiblePageDataMap: [String: String] = [:]
    for (i, pageId) in visiblePageIds.enumerated() where i < perPagePayloads.count {
      let payload = perPagePayloads[i]
      if payload.isEmpty || payload == blankPagePayload {
        continue
      }
      visiblePageDataMap[pageId] = payload
    }

    // Load existing body so non-visible pages keep their last-saved data.
    // It's not an error for the file to be missing or malformed -- new
    // notebooks have no body yet, and a corrupt body shouldn't block a save
    // (the save here will overwrite it with a fresh, valid copy).
    var existingPagesById: [String: [String: Any]] = [:]
    if FileManager.default.fileExists(atPath: bridgePosixPath(fromPath: existingBodyPath)) {
      if
        let existingData = try? Data(contentsOf: bridgeFileURL(fromPath: existingBodyPath)),
        let existingJson = try? JSONSerialization.jsonObject(with: existingData) as? [String: Any],
        let existingPages = existingJson["pages"] as? [[String: Any]]
      {
        for page in existingPages {
          if let id = page["id"] as? String {
            existingPagesById[id] = page
          }
        }
      }
    }

    // Compose the new pages array using metadata as the source of truth for
    // ordering and which pages exist. For each page, the data field comes
    // from (in priority order): engine state for visible pages, on-disk data
    // for non-visible pages, or absent (page has no drawing yet).
    let newPages: [[String: Any]] = pagesMetadata.map { metadataPage in
      var page = metadataPage
      let pageId = (metadataPage["id"] as? String) ?? ""

      if let visibleData = visiblePageDataMap[pageId] {
        page["data"] = visibleData
      } else if
        let existing = existingPagesById[pageId],
        let existingData = existing["data"] as? String
      {
        page["data"] = existingData
      }

      return page
    }

    var notebook: [String: Any] = [
      "version": "1.0",
      "pages": newPages,
    ]
    if let canvasWidth = originalCanvasWidth {
      notebook["originalCanvasWidth"] = canvasWidth
    }

    return notebook
  }

  // Synchronous decomposition: same C++ call the existing async
  // decomposeContinuousWindow uses, just without the resolver/rejecter so it
  // can be composed inside larger background-queue work.
  private func decomposeWindowPayloadSync(
    windowPayload: String,
    pageCount: Int,
    pageHeight: CGFloat
  ) -> [String] {
    guard pageCount > 0 else { return [] }

    let drawingBinary = extractDrawingBinary(from: windowPayload)
    if drawingBinary == nil || drawingBinary?.isEmpty == true {
      return Array(repeating: blankPagePayload, count: pageCount)
    }

    var pageDataPtrs = Array<UnsafeMutablePointer<UInt8>?>(repeating: nil, count: pageCount)
    var pageDataSizes = Array<Int32>(repeating: 0, count: pageCount)

    let resultCount = drawingBinary!.withUnsafeBytes { rawBuffer -> Int32 in
      let baseAddress = rawBuffer.bindMemory(to: UInt8.self).baseAddress
      return nativeDecomposeContinuousWindow(
        baseAddress,
        Int32(drawingBinary!.count),
        Int32(pageCount),
        Float(pageHeight),
        &pageDataPtrs,
        &pageDataSizes
      )
    }

    if resultCount == 0 {
      return Array(repeating: blankPagePayload, count: pageCount)
    }

    var payloads: [String] = []
    payloads.reserveCapacity(pageCount)
    for index in 0..<pageCount {
      let pagePtr = pageDataPtrs[index]
      let pageSize = pageDataSizes[index]
      if let pagePtr, pageSize > 0 {
        let pageData = Data(bytes: pagePtr, count: Int(pageSize))
        payloads.append(makePagePayload(from: pageData))
        freeSerializedData(pagePtr)
      } else {
        payloads.append(blankPagePayload)
      }
    }
    return payloads
  }

  @objc func loadEngineFromFile(
    _ viewTag: NSNumber,
    filePath: String,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.global(qos: .userInitiated).async { [weak self] in
      let url = bridgeFileURL(fromPath: filePath)

      // Read the file off the main thread. If it's missing or empty, that's
      // not an error -- it just means the file has no persisted body and the
      // engine should remain empty. Resolve with false so the JS layer can
      // distinguish "loaded successfully" from "nothing to load."
      let body: String
      do {
        let bodyData = try Data(contentsOf: url)
        guard !bodyData.isEmpty, let bodyString = String(data: bodyData, encoding: .utf8) else {
          DispatchQueue.main.async { resolver(false) }
          return
        }
        body = bodyString
      } catch {
        DispatchQueue.main.async { resolver(false) }
        return
      }

      DispatchQueue.main.async {
        guard let view = self?.getView(forTag: viewTag) else {
          rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
          return
        }

        view.loadBase64Data(body) { result in
          guard let array = result as? [Any], array.count >= 2 else {
            resolver(false)
            return
          }

          let errorValue = array[0]
          let successValue = array[1]

          if let error = errorValue as? String {
            rejecter("LOAD_DATA_ERROR", error, nil)
          } else if let success = successValue as? Bool {
            resolver(success)
          } else {
            resolver(false)
          }
        }
      }
    }
  }

  // MARK: - Full-notebook native load
  //
  // loadNotebookForVisibleWindow reads the body file in native, composes the
  // visible window's pages into a single combined payload, loads it into the
  // engine -- all without crossing the bridge with the body bytes. JS gets
  // back just the metadata array (page IDs, titles, rotations) and a
  // success bool, which is small.

  @objc func loadNotebookForVisibleWindow(
    _ viewTag: NSNumber,
    bodyPath: String,
    visiblePageIds: [String],
    pageHeight: CGFloat,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.global(qos: .userInitiated).async { [weak self] in
      guard let self else { return }

      // autoreleasepool keeps the multi-MB body Data + parsed NSDictionary +
      // synthesized metadata from accumulating across rapid load calls
      // (e.g. window shifts during a fast scroll).
      autoreleasepool {
      // Read + parse the body in native. Resolves with { success: false, ... }
      // when the file is absent or malformed so the JS layer can fall back to
      // the existing slow path on first-run files.
      guard FileManager.default.fileExists(atPath: bridgePosixPath(fromPath: bodyPath)) else {
        DispatchQueue.main.async {
          resolver(["success": false, "reason": "missing_body"])
        }
        return
      }

      let notebookJson: [String: Any]
      let pagesArray: [[String: Any]]
      do {
        let bodyData = try Data(contentsOf: bridgeFileURL(fromPath: bodyPath))
        guard let parsed = try JSONSerialization.jsonObject(with: bodyData) as? [String: Any] else {
          DispatchQueue.main.async {
            resolver(["success": false, "reason": "invalid_body_shape"])
          }
          return
        }
        notebookJson = parsed
        pagesArray = (parsed["pages"] as? [[String: Any]]) ?? []
      } catch {
        DispatchQueue.main.async {
          resolver(["success": false, "reason": "read_failed", "error": error.localizedDescription])
        }
        return
      }

      // Strip the heavy `data` field out of the metadata we'll return to JS;
      // JS only needs IDs/titles/etc to render the page list. The data lives
      // in native (loaded into the engine) for visible pages and stays on
      // disk for non-visible pages.
      let pagesMetadata: [[String: Any]] = pagesArray.map { page in
        var slim = page
        slim.removeValue(forKey: "data")
        return slim
      }

      // Build the per-page payloads for the visible window, then compose them
      // into a single combined-window payload for loadBase64Data.
      var pagesById: [String: [String: Any]] = [:]
      for page in pagesArray {
        if let id = page["id"] as? String {
          pagesById[id] = page
        }
      }

      let visiblePerPagePayloads: [String] = visiblePageIds.map { pageId in
        if
          let page = pagesById[pageId],
          let dataString = page["data"] as? String,
          !dataString.isEmpty
        {
          return dataString
        }
        return self.blankPagePayload
      }

      let combinedPayload = self.composeWindowPayloadsSync(
        pagePayloads: visiblePerPagePayloads,
        pageHeight: pageHeight
      )

      DispatchQueue.main.async {
        guard let view = self.getView(forTag: viewTag) else {
          // Body loaded but the view's gone -- still resolve metadata so the
          // JS caller can react. Caller decides whether to retry.
          resolver([
            "success": false,
            "reason": "view_not_found",
            "pagesMetadata": pagesMetadata,
            "originalCanvasWidth": notebookJson["originalCanvasWidth"] ?? NSNull(),
          ])
          return
        }

        view.loadBase64Data(combinedPayload) { result in
          let array = result as? [Any] ?? []
          let loadSucceeded: Bool = {
            guard array.count >= 2 else { return false }
            if let s = array[1] as? Bool { return s }
            return false
          }()

          resolver([
            "success": loadSucceeded,
            "pagesMetadata": pagesMetadata,
            "originalCanvasWidth": notebookJson["originalCanvasWidth"] ?? NSNull(),
          ])
        }
      }
      } // close autoreleasepool
    }
  }

  // Synchronous compose: mirrors composeContinuousWindow but inlined for the
  // background-queue load path. Allocates and frees the same temp buffers
  // the async version does.
  private func composeWindowPayloadsSync(
    pagePayloads: [String],
    pageHeight: CGFloat
  ) -> String {
    let pageCount = pagePayloads.count
    if pageCount == 0 {
      return blankPagePayload
    }

    let pageBuffers: [Data] = pagePayloads.map { extractDrawingBinary(from: $0) ?? Data() }
    var allocatedPagePtrs: [UnsafeMutablePointer<UInt8>?] = []
    allocatedPagePtrs.reserveCapacity(pageCount)
    for pageBuffer in pageBuffers {
      guard !pageBuffer.isEmpty else {
        allocatedPagePtrs.append(nil)
        continue
      }
      let bufferPtr = UnsafeMutablePointer<UInt8>.allocate(capacity: pageBuffer.count)
      pageBuffer.copyBytes(to: bufferPtr, count: pageBuffer.count)
      allocatedPagePtrs.append(bufferPtr)
    }
    defer {
      for pagePtr in allocatedPagePtrs {
        pagePtr?.deallocate()
      }
    }

    var dataPtrs: [UnsafePointer<UInt8>?] = allocatedPagePtrs.map {
      guard let pagePtr = $0 else { return nil }
      return UnsafePointer(pagePtr)
    }
    var dataSizes: [Int32] = pageBuffers.map { Int32($0.count) }
    var outputSize: Int32 = 0

    let resultPtr = nativeComposeContinuousWindow(
      &dataPtrs,
      &dataSizes,
      Int32(pageCount),
      Float(pageHeight),
      &outputSize
    )

    guard let resultPtr, outputSize > 0 else {
      return blankPagePayload
    }
    let resultData = Data(bytes: resultPtr, count: Int(outputSize))
    freeSerializedData(resultPtr)
    return makePagePayload(from: resultData)
  }

  @objc func composeContinuousWindow(
    _ pagePayloads: [String],
    pageHeight: CGFloat,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.global(qos: .userInitiated).async {
      let pageCount = pagePayloads.count
      if pageCount == 0 {
        resolver(self.blankPagePayload)
        return
      }

      let pageBuffers: [Data] = pagePayloads.map { self.extractDrawingBinary(from: $0) ?? Data() }
      var allocatedPagePtrs: [UnsafeMutablePointer<UInt8>?] = []
      allocatedPagePtrs.reserveCapacity(pageCount)

      for pageBuffer in pageBuffers {
        guard !pageBuffer.isEmpty else {
          allocatedPagePtrs.append(nil)
          continue
        }

        let bufferPtr = UnsafeMutablePointer<UInt8>.allocate(capacity: pageBuffer.count)
        pageBuffer.copyBytes(to: bufferPtr, count: pageBuffer.count)
        allocatedPagePtrs.append(bufferPtr)
      }

      defer {
        for pagePtr in allocatedPagePtrs {
          pagePtr?.deallocate()
        }
      }

      var dataPtrs: [UnsafePointer<UInt8>?] = allocatedPagePtrs.map {
        guard let pagePtr = $0 else { return nil }
        return UnsafePointer(pagePtr)
      }
      var dataSizes: [Int32] = pageBuffers.map { Int32($0.count) }
      var outputSize: Int32 = 0

      let resultPtr = nativeComposeContinuousWindow(
        &dataPtrs,
        &dataSizes,
        Int32(pageCount),
        Float(pageHeight),
        &outputSize
      )

      guard let resultPtr, outputSize > 0 else {
        resolver(self.blankPagePayload)
        return
      }

      let resultData = Data(bytes: resultPtr, count: Int(outputSize))
      freeSerializedData(resultPtr)
      resolver(self.makePagePayload(from: resultData))
    }
  }

  @objc func decomposeContinuousWindow(
    _ windowPayload: String,
    pageCount: Int,
    pageHeight: CGFloat,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.global(qos: .userInitiated).async {
      guard pageCount > 0 else {
        resolver([])
        return
      }

      let drawingBinary = self.extractDrawingBinary(from: windowPayload)
      if drawingBinary == nil || drawingBinary?.isEmpty == true {
        resolver(Array(repeating: self.blankPagePayload, count: pageCount))
        return
      }

      var pageDataPtrs = Array<UnsafeMutablePointer<UInt8>?>(repeating: nil, count: pageCount)
      var pageDataSizes = Array<Int32>(repeating: 0, count: pageCount)

      let resultCount = drawingBinary!.withUnsafeBytes { rawBuffer in
        let baseAddress = rawBuffer.bindMemory(to: UInt8.self).baseAddress
        return nativeDecomposeContinuousWindow(
          baseAddress,
          Int32(drawingBinary!.count),
          Int32(pageCount),
          Float(pageHeight),
          &pageDataPtrs,
          &pageDataSizes
        )
      }

      if resultCount == 0 {
        rejecter("DECOMPOSE_FAILED", "Failed to split continuous window payload", nil)
        return
      }

      var payloads: [String] = []
      payloads.reserveCapacity(pageCount)

      for index in 0..<pageCount {
        let pagePtr = pageDataPtrs[index]
        let pageSize = pageDataSizes[index]
        if let pagePtr, pageSize > 0 {
          let pageData = Data(bytes: pagePtr, count: Int(pageSize))
          payloads.append(self.makePagePayload(from: pageData))
          freeSerializedData(pagePtr)
        } else {
          payloads.append(self.blankPagePayload)
        }
      }

      resolver(payloads)
    }
  }

  // MARK: - Image Export

  @objc func getBase64PngData(
    _ viewTag: NSNumber,
    scale: CGFloat,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let view = self?.getView(forTag: viewTag) else {
        rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
        return
      }

      view.getBase64PngData(scale) { result in
        guard let array = result as? [Any], array.count >= 2 else {
          resolver("")
          return
        }

        // Format: [error, data] where error is String or NSNull, data is String or NSNull
        let errorValue = array[0]
        let dataValue = array[1]

        if let error = errorValue as? String {
          rejecter("GET_PNG_ERROR", error, nil)
        } else if let data = dataValue as? String {
          resolver(data)
        } else {
          resolver("")
        }
      }
    }
  }

  @objc func getBase64JpegData(
    _ viewTag: NSNumber,
    scale: CGFloat,
    compression: CGFloat,
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let view = self?.getView(forTag: viewTag) else {
        rejecter("VIEW_NOT_FOUND", "MobileInkCanvasView not found", nil)
        return
      }

      view.getBase64JpegData(scale, compression: compression) { result in
        guard let array = result as? [Any], array.count >= 2 else {
          resolver("")
          return
        }

        // Format: [error, data] where error is String or NSNull, data is String or NSNull
        let errorValue = array[0]
        let dataValue = array[1]

        if let error = errorValue as? String {
          rejecter("GET_JPEG_ERROR", error, nil)
        } else if let data = dataValue as? String {
          resolver(data)
        } else {
          resolver("")
        }
      }
    }
  }

  // MARK: - Batch Export

  /// Batch export multiple pages to PNG images
  /// This is much faster than exporting pages one by one because it:
  /// 1. Creates a single Skia engine and surface (reused for all pages)
  /// 2. Doesn't switch visible pages (no UI updates)
  /// 3. Processes all pages in a single native call
  @objc func batchExportPages(
    _ pagesDataArray: [String],
    backgroundTypes: [String],
    width: Int,
    height: Int,
    scale: CGFloat,
    pdfBackgroundUri: String,
    pageIndices: [NSNumber],
    resolver: @escaping RCTPromiseResolveBlock,
    rejecter: @escaping RCTPromiseRejectBlock
  ) {
    // Process on a background queue for better performance
    DispatchQueue.global(qos: .userInitiated).async {
      // CRITICAL: autoreleasepool. batchExportPages allocates large Foundation
      // objects (PDFDocument, CGContext bitmap buffers up to ~25 MB per page,
      // CGImage results, base64 strings). Without an explicit pool these
      // accumulate across rapid preview-generation calls during drawing and
      // contribute to the OOM crash on heavy notebooks (15 MB CGBitmapContext
      // allocation refused was the proximate cause of the crash report
      // showing "Failed to grow buffer to 7078649").
      autoreleasepool {
      let numPages = pagesDataArray.count

      guard numPages > 0 else {
        resolver([])
        return
      }

      // Load PDF document if URI is provided
      var pdfDocument: PDFDocument? = nil
      var requestedPdfPage: Int? = nil  // For single-page export with #page=N fragment
      if !pdfBackgroundUri.isEmpty {
        // Parse #page=N fragment if present (1-indexed, convert to 0-indexed)
        if let fragmentStart = pdfBackgroundUri.range(of: "#page=") {
          let pageNumStr = pdfBackgroundUri[fragmentStart.upperBound...]
          if let pageNum = Int(pageNumStr), pageNum > 0 {
            requestedPdfPage = pageNum - 1  // Convert to 0-indexed
            print("[BatchExport] Parsed page fragment: #page=\(pageNum) -> index \(pageNum - 1)")
          }
        }

        // Strip fragment from URI for PDF loading
        let cleanUri = pdfBackgroundUri.components(separatedBy: "#").first ?? pdfBackgroundUri

        // Convert URI to URL - handle file paths, file:// URLs, and http(s):// URLs
        var pdfUrl: URL? = nil
        if cleanUri.hasPrefix("http://") || cleanUri.hasPrefix("https://") {
          // Remote URL - PDFDocument can load directly from URL
          pdfUrl = URL(string: cleanUri)
        } else if cleanUri.hasPrefix("file://") {
          pdfUrl = URL(string: cleanUri)
        } else if cleanUri.hasPrefix("/") {
          pdfUrl = URL(fileURLWithPath: cleanUri)
        }
        if let url = pdfUrl {
          pdfDocument = PDFDocument(url: url)
          if pdfDocument == nil {
            print("[BatchExport] Failed to load PDF from: \(cleanUri)")
          } else {
            print("[BatchExport] Loaded PDF with \(pdfDocument!.pageCount) pages from: \(url.lastPathComponent)")
          }
        }
      }

      // Log background types for debugging
      let bgTypesDebug = backgroundTypes.prefix(10).joined(separator: ", ")
      print("[BatchExport] Background types: [\(bgTypesDebug)] (pdfUri empty: \(pdfBackgroundUri.isEmpty))")

      // Decode base64 page data to binary and copy to allocated memory
      var allocatedDataPtrs: [UnsafeMutablePointer<UInt8>?] = []
      var pagesDataSizes: [Int32] = []
      var bgTypesCStrings: [UnsafeMutablePointer<CChar>?] = []

      // PDF pixel data arrays
      var pdfPixelDataPtrs: [UnsafeMutablePointer<UInt8>?] = []
      var pdfPixelWidths: [Int32] = []
      var pdfPixelHeights: [Int32] = []
      var resolvedPageIndices = [Int32]()
      resolvedPageIndices.reserveCapacity(numPages)

      for i in 0..<numPages {
        let pageDataJson = pagesDataArray[i]
        resolvedPageIndices.append(
          i < pageIndices.count ? Int32(truncating: pageIndices[i]) : Int32(i)
        )

        // Parse JSON format: {"pages":{"0":"<base64>"}}
        if let jsonData = pageDataJson.data(using: .utf8),
           let jsonDict = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any],
           let pages = jsonDict["pages"] as? [String: String],
           let page0Base64 = pages["0"],
           let binaryData = Data(base64Encoded: page0Base64),
           !binaryData.isEmpty {
          // Allocate and copy the data
          let ptr = UnsafeMutablePointer<UInt8>.allocate(capacity: binaryData.count)
          binaryData.copyBytes(to: ptr, count: binaryData.count)
          allocatedDataPtrs.append(ptr)
          pagesDataSizes.append(Int32(binaryData.count))
          print("[Swift] Page \(i): \(binaryData.count) bytes, base64 prefix: \(String(page0Base64.prefix(50)))...")
        } else {
          // Empty page
          allocatedDataPtrs.append(nil)
          pagesDataSizes.append(0)
          print("[Swift] Page \(i): empty data")
        }

        // Background type
        let bgType = i < backgroundTypes.count ? backgroundTypes[i] : "plain"
        bgTypesCStrings.append(strdup(bgType))

        // Render PDF page to RGBA if this page has PDF background
        if bgType == "pdf" {
          if let pdf = pdfDocument {
            // For single-page export with #page=N, use the requested page; otherwise use loop index
            let pdfPageIndex = (numPages == 1 && requestedPdfPage != nil) ? requestedPdfPage! : i
            if pdfPageIndex < pdf.pageCount, let pdfPage = pdf.page(at: pdfPageIndex) {
              // Render PDF page to RGBA pixel buffer at canvas dimensions
              if let (pixelPtr, pdfWidth, pdfHeight) = self.renderPdfPageToRgba(pdfPage, width: width, height: height) {
                print("[BatchExport] Rendered PDF page \(i) at \(pdfWidth)x\(pdfHeight)")
                pdfPixelDataPtrs.append(pixelPtr)
                pdfPixelWidths.append(Int32(pdfWidth))
                pdfPixelHeights.append(Int32(pdfHeight))
              } else {
                print("[BatchExport] Failed to render PDF page \(i)")
                pdfPixelDataPtrs.append(nil)
                pdfPixelWidths.append(0)
                pdfPixelHeights.append(0)
              }
            } else {
              print("[BatchExport] PDF page \(i) not available (pdf has \(pdf.pageCount) pages)")
              pdfPixelDataPtrs.append(nil)
              pdfPixelWidths.append(0)
              pdfPixelHeights.append(0)
            }
          } else {
            print("[BatchExport] Page \(i) has pdf bgType but no PDF document loaded")
            pdfPixelDataPtrs.append(nil)
            pdfPixelWidths.append(0)
            pdfPixelHeights.append(0)
          }
        } else {
          pdfPixelDataPtrs.append(nil)
          pdfPixelWidths.append(0)
          pdfPixelHeights.append(0)
        }
      }

      // Convert to const pointers for C
      var pagesDataConstPtrs: [UnsafePointer<UInt8>?] = allocatedDataPtrs.map { ptr in
        ptr.map { UnsafePointer($0) }
      }
      var pdfPixelConstPtrs: [UnsafePointer<UInt8>?] = pdfPixelDataPtrs.map { ptr in
        ptr.map { UnsafePointer($0) }
      }

      // Allocate output arrays
      var outResults = [UnsafeMutablePointer<CChar>?](repeating: nil, count: numPages)
      var outResultLengths = [Int32](repeating: 0, count: numPages)

      // Call C batch export
      pagesDataConstPtrs.withUnsafeBufferPointer { dataPtrsBuffer in
        pagesDataSizes.withUnsafeBufferPointer { sizesBuffer in
          bgTypesCStrings.withUnsafeBufferPointer { bgTypesBuffer in
            pdfPixelConstPtrs.withUnsafeBufferPointer { pdfPtrsBuffer in
              pdfPixelWidths.withUnsafeBufferPointer { pdfWidthsBuffer in
                pdfPixelHeights.withUnsafeBufferPointer { pdfHeightsBuffer in
                  resolvedPageIndices.withUnsafeBufferPointer { pageIndicesBuffer in
                    outResults.withUnsafeMutableBufferPointer { resultsBuffer in
                      outResultLengths.withUnsafeMutableBufferPointer { lengthsBuffer in
                        // Cast bgTypes buffer to const char**
                        let bgTypesPtr = bgTypesBuffer.baseAddress!
                        bgTypesPtr.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: numPages) { reboundPtr in
                          _ = nativeBatchExportPages(
                            dataPtrsBuffer.baseAddress!,
                            sizesBuffer.baseAddress!,
                            reboundPtr,
                            pdfPtrsBuffer.baseAddress!,
                            pdfWidthsBuffer.baseAddress!,
                            pdfHeightsBuffer.baseAddress!,
                            pageIndicesBuffer.baseAddress,
                            Int32(numPages),
                            Int32(width),
                            Int32(height),
                            Float(scale),
                            resultsBuffer.baseAddress!,
                            lengthsBuffer.baseAddress!
                          )
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }

      // Free allocated drawing data
      for ptr in allocatedDataPtrs {
        ptr?.deallocate()
      }

      // Free background type strings
      for cstr in bgTypesCStrings {
        free(cstr)
      }

      // Free PDF pixel data
      for ptr in pdfPixelDataPtrs {
        ptr?.deallocate()
      }

      // Convert results to Swift strings
      var resultStrings: [String] = []
      for i in 0..<numPages {
        if let resultPtr = outResults[i] {
          resultStrings.append(String(cString: resultPtr))
          freeBatchExportResult(resultPtr)
        } else {
          resultStrings.append("")
        }
      }

      print("[BatchExport] Exported \(resultStrings.filter { !$0.isEmpty }.count) of \(numPages) pages")
      resolver(resultStrings)
      } // close autoreleasepool
    }
  }

  /// Render a PDF page to RGBA pixel buffer at canvas dimensions
  /// Returns tuple of (pixel data pointer, width, height) or nil on failure
  /// Rendered at 1x canvas size - Skia's scale transform handles the 2x export scaling
  private func renderPdfPageToRgba(_ pdfPage: PDFPage, width: Int, height: Int) -> (UnsafeMutablePointer<UInt8>, Int, Int)? {
    guard let cgPdfPage = pdfPage.pageRef else {
      print("[BatchExport] Failed to get CGPDFPage from PDFPage")
      return nil
    }

    let pdfRect = cgPdfPage.getBoxRect(.mediaBox)

    guard pdfRect.width > 0 && pdfRect.height > 0 else {
      print("[BatchExport] Invalid PDF mediaBox")
      return nil
    }

    // Render at canvas dimensions (1x) - Skia will scale up during export
    let renderWidth = width
    let renderHeight = height

    // Calculate scale to fit PDF in canvas while maintaining aspect ratio
    let fitScaleX = CGFloat(width) / pdfRect.width
    let fitScaleY = CGFloat(height) / pdfRect.height
    let fitScale = min(fitScaleX, fitScaleY)

    // Create bitmap context with RGBA format
    let bytesPerPixel = 4
    let bytesPerRow = renderWidth * bytesPerPixel
    let totalBytes = bytesPerRow * renderHeight

    let pixelData = UnsafeMutablePointer<UInt8>.allocate(capacity: totalBytes)
    pixelData.initialize(repeating: 255, count: totalBytes) // White background

    guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
          let context = CGContext(
            data: pixelData,
            width: renderWidth,
            height: renderHeight,
            bitsPerComponent: 8,
            bytesPerRow: bytesPerRow,
            space: colorSpace,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
          ) else {
      pixelData.deallocate()
      return nil
    }

    // Flip the context to top-left origin (like UIKit) so Skia can read it correctly
    // CGContext bitmap defaults to bottom-left origin, but Skia expects top-left
    context.translateBy(x: 0, y: CGFloat(renderHeight))
    context.scaleBy(x: 1, y: -1)

    // Fill with white background
    context.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: renderWidth, height: renderHeight))

    // Calculate scaled dimensions
    let scaledPdfWidth = pdfRect.width * fitScale
    let scaledPdfHeight = pdfRect.height * fitScale

    // Center the PDF in the render area
    let offsetX = (CGFloat(renderWidth) - scaledPdfWidth) / 2
    let offsetY = (CGFloat(renderHeight) - scaledPdfHeight) / 2

    // Draw PDF using the same transform as MobileInkBackgroundView (now context is top-left origin)
    context.saveGState()

    // Apply the same transform as MobileInkBackgroundView uses
    context.translateBy(x: offsetX - pdfRect.origin.x * fitScale,
                        y: offsetY + scaledPdfHeight + pdfRect.origin.y * fitScale)
    context.scaleBy(x: fitScale, y: -fitScale)

    context.drawPDFPage(cgPdfPage)

    context.restoreGState()

    return (pixelData, renderWidth, renderHeight)
  }
}
