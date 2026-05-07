import UIKit

/// A view that renders various background types for the native drawing canvas
/// Supports plain, lined, grid, dotted, graph, and PDF backgrounds with caching
@available(iOS 14.0, *)
class MobileInkBackgroundView: UIView {
  private static let notebookAspectRatio: CGFloat = 11.0 / 8.5
  private static let targetPatternSpacing: CGFloat = 60
  private static let graphMajorLineMultiple = 5

  var backgroundType: String = "plain" {
    didSet {
      setNeedsDisplay()
    }
  }
  var isDarkMode: Bool = false {
    didSet {
      setNeedsDisplay()
    }
  }

  // PDF state
  private var pdfDocument: CGPDFDocument?
  private var pdfPage: CGPDFPage? {
    didSet {
      setNeedsDisplay()
    }
  }
  private var windowPageNumbers: [Int] = [] {
    didSet {
      setNeedsDisplay()
    }
  }

  // Page caching for instant switching. Each cached CGPDFPage retains a reference
  // into the document's content stream; CoreGraphics also keeps decompressed page
  // data resident for fast re-render. On heavy notebooks under memory pressure,
  // 5 cached pages can keep ~30+ MB resident across the document. 3 still covers
  // current + prev + next so navigation stays smooth, but caps the cost during
  // long browsing sessions.
  private var pageCache: [Int: CGPDFPage] = [:]
  private let maxCachedPages = 3
  private var pageCacheOrder: [Int] = []  // LRU tracking

  // Track current state to avoid redundant loads
  private var currentPdfUri: String?
  private var currentPageNumber: Int = 1
  private var isLoadingPdf: Bool = false
  private var pdfLoadToken: UInt64 = 0

  // Callback for loading state changes
  var onLoadingStateChanged: ((Bool, Int?) -> Void)?

  override init(frame: CGRect) {
    super.init(frame: frame)
    backgroundColor = .white
    isOpaque = true
  }

  required init?(coder: NSCoder) {
    super.init(coder: coder)
    backgroundColor = .white
    isOpaque = true
  }

  /// Clear all PDF state (call when switching files)
  func unloadPDF() {
    if !Thread.isMainThread {
      DispatchQueue.main.async { [weak self] in
        self?.unloadPDF()
      }
      return
    }

    pdfLoadToken &+= 1
    resetPdfState(clearCurrentUri: true)
  }

  private func resetPdfState(clearCurrentUri: Bool) {
    pdfDocument = nil
    pdfPage = nil
    pageCache.removeAll()
    pageCacheOrder.removeAll()
    isLoadingPdf = false
    if clearCurrentUri {
      currentPdfUri = nil
      currentPageNumber = 1
    }
    windowPageNumbers = []
  }

  private func completePdfLoad(_ isLoading: Bool, pageNumber: Int?) {
    self.isLoadingPdf = isLoading
    onLoadingStateChanged?(isLoading, pageNumber)
  }

  private func failPdfLoad(requestToken: UInt64, uri: String, pageNumber: Int, reason: String) {
    guard requestToken == pdfLoadToken else {
      print("[MobileInkBackgroundView] Ignoring stale PDF failure for token \(requestToken): \(reason)")
      return
    }

    print("[MobileInkBackgroundView] PDF load failed for \(uri) page \(pageNumber): \(reason)")
    completePdfLoad(false, pageNumber: pageNumber)
    resetPdfState(clearCurrentUri: true)
  }

  override func draw(_ rect: CGRect) {
    guard let context = UIGraphicsGetCurrentContext() else {
      print("MobileInkBackgroundView: No graphics context available")
      return
    }

    // Fill background based on theme
    if isDarkMode {
      context.setFillColor(UIColor(red: 0.15, green: 0.15, blue: 0.15, alpha: 1.0).cgColor)
    } else {
      context.setFillColor(UIColor.white.cgColor)
    }
    context.fill(rect)

    // Draw pattern based on type
    switch backgroundType {
    case "lined":
      drawLinedBackground(in: context, rect: rect)
    case "grid":
      drawGridBackground(in: context, rect: rect)
    case "dotted":
      drawDottedBackground(in: context, rect: rect)
    case "graph":
      drawGraphBackground(in: context, rect: rect)
    case "pdf":
      if windowPageNumbers.count > 1 {
        drawPDFWindowBackground(in: context, rect: rect)
      } else if let pdfPage = pdfPage {
        drawPDFBackground(in: context, rect: rect, pdfPage: pdfPage)
      }
    default:
      break // plain - just background color
    }
  }

  // MARK: - Background Patterns

  private func resolvedSinglePageHeight(for rect: CGRect) -> CGFloat {
    guard rect.width > 0, rect.height > 0 else {
      return max(rect.height, 1)
    }

    let inferredSinglePageHeight = rect.width * Self.notebookAspectRatio
    let stackedPageCount = max(1, Int(round(rect.height / inferredSinglePageHeight)))
    return rect.height / CGFloat(stackedPageCount)
  }

  private func resolvedPatternSpacing(
    for rect: CGRect,
    targetSpacing: CGFloat = MobileInkBackgroundView.targetPatternSpacing,
    requiredLineMultiple: Int = 1
  ) -> CGFloat {
    let singlePageHeight = resolvedSinglePageHeight(for: rect)
    guard singlePageHeight > 0 else {
      return targetSpacing
    }

    let rawLineCount = max(1, Int(round(singlePageHeight / targetSpacing)))
    let adjustedLineCount: Int
    if requiredLineMultiple > 1 {
      let roundedMultiple = Int(
        round(Double(rawLineCount) / Double(requiredLineMultiple))
      ) * requiredLineMultiple
      adjustedLineCount = max(requiredLineMultiple, roundedMultiple)
    } else {
      adjustedLineCount = rawLineCount
    }

    return singlePageHeight / CGFloat(max(1, adjustedLineCount))
  }

  private func drawLinedBackground(in context: CGContext, rect: CGRect) {
    if isDarkMode {
      context.setStrokeColor(UIColor(red: 0.30, green: 0.30, blue: 0.30, alpha: 1.0).cgColor)
    } else {
      context.setStrokeColor(UIColor(red: 0.88, green: 0.88, blue: 0.88, alpha: 1.0).cgColor)
    }
    context.setLineWidth(1.0)

    let lineSpacing = resolvedPatternSpacing(for: rect)
    let numberOfLines = Int(ceil(rect.height / lineSpacing)) + 1

    for i in 0...numberOfLines {
      let y = CGFloat(i) * lineSpacing
      if y <= rect.height {
        context.move(to: CGPoint(x: 0, y: y))
        context.addLine(to: CGPoint(x: rect.width, y: y))
        context.strokePath()
      }
    }
  }

  private func drawGridBackground(in context: CGContext, rect: CGRect) {
    if isDarkMode {
      context.setStrokeColor(UIColor(red: 0.30, green: 0.30, blue: 0.30, alpha: 1.0).cgColor)
    } else {
      context.setStrokeColor(UIColor(red: 0.88, green: 0.88, blue: 0.88, alpha: 1.0).cgColor)
    }
    context.setLineWidth(1.0)

    let spacing = resolvedPatternSpacing(for: rect)

    // Vertical lines
    let vLineCount = Int(rect.width / spacing) + 1
    for i in 0...vLineCount {
      let x = CGFloat(i) * spacing
      context.move(to: CGPoint(x: x, y: 0))
      context.addLine(to: CGPoint(x: x, y: rect.height))
      context.strokePath()
    }

    // Horizontal lines
    let hLineCount = Int(rect.height / spacing) + 1
    for i in 0...hLineCount {
      let y = CGFloat(i) * spacing
      context.move(to: CGPoint(x: 0, y: y))
      context.addLine(to: CGPoint(x: rect.width, y: y))
      context.strokePath()
    }
  }

  private func drawDottedBackground(in context: CGContext, rect: CGRect) {
    if isDarkMode {
      context.setFillColor(UIColor(red: 0.30, green: 0.30, blue: 0.30, alpha: 1.0).cgColor)
    } else {
      context.setFillColor(UIColor(red: 0.88, green: 0.88, blue: 0.88, alpha: 1.0).cgColor)
    }

    let spacing = resolvedPatternSpacing(for: rect)
    let dotRadius: CGFloat = 4  // Larger, more visible dots

    let hDotCount = Int(rect.width / spacing) + 1
    let vDotCount = Int(ceil(rect.height / spacing)) + 1

    for i in 0...hDotCount {
      for j in 0...vDotCount {
        let x = CGFloat(i) * spacing
        let y = CGFloat(j) * spacing

        if y <= rect.height {
          context.fillEllipse(in: CGRect(x: x - dotRadius, y: y - dotRadius,
                                        width: dotRadius * 2, height: dotRadius * 2))
        }
      }
    }
  }

  private func drawGraphBackground(in context: CGContext, rect: CGRect) {
    // Minor grid
    if isDarkMode {
      context.setStrokeColor(UIColor(red: 0.22, green: 0.22, blue: 0.22, alpha: 1.0).cgColor)
    } else {
      context.setStrokeColor(UIColor(red: 0.94, green: 0.94, blue: 0.94, alpha: 1.0).cgColor)
    }
    context.setLineWidth(0.5)

    let minorSpacing = resolvedPatternSpacing(
      for: rect,
      requiredLineMultiple: Self.graphMajorLineMultiple
    )

    // Minor vertical lines
    let vMinorCount = Int(rect.width / minorSpacing) + 1
    for i in 0...vMinorCount {
      let x = CGFloat(i) * minorSpacing
      context.move(to: CGPoint(x: x, y: 0))
      context.addLine(to: CGPoint(x: x, y: rect.height))
      context.strokePath()
    }

    // Minor horizontal lines
    let hMinorCount = Int(rect.height / minorSpacing) + 1
    for i in 0...hMinorCount {
      let y = CGFloat(i) * minorSpacing
      context.move(to: CGPoint(x: 0, y: y))
      context.addLine(to: CGPoint(x: rect.width, y: y))
      context.strokePath()
    }

    // Major grid
    if isDarkMode {
      context.setStrokeColor(UIColor(red: 0.35, green: 0.35, blue: 0.35, alpha: 1.0).cgColor)
    } else {
      context.setStrokeColor(UIColor(red: 0.80, green: 0.80, blue: 0.80, alpha: 1.0).cgColor)
    }
    context.setLineWidth(1.0)

    let majorSpacing = minorSpacing * CGFloat(Self.graphMajorLineMultiple)

    // Major vertical lines
    let vMajorCount = Int(rect.width / majorSpacing) + 1
    for i in 0...vMajorCount {
      let x = CGFloat(i) * majorSpacing
      context.move(to: CGPoint(x: x, y: 0))
      context.addLine(to: CGPoint(x: x, y: rect.height))
      context.strokePath()
    }

    // Major horizontal lines
    let hMajorCount = Int(rect.height / majorSpacing) + 1
    for i in 0...hMajorCount {
      let y = CGFloat(i) * majorSpacing
      context.move(to: CGPoint(x: 0, y: y))
      context.addLine(to: CGPoint(x: rect.width, y: y))
      context.strokePath()
    }
  }

  // MARK: - PDF Rendering

  private func drawPDFBackground(in context: CGContext, rect: CGRect, pdfPage: CGPDFPage) {
    context.saveGState()

    let pdfRect = pdfPage.getBoxRect(.mediaBox)

    // Check for invalid mediaBox
    guard pdfRect.width > 0 && pdfRect.height > 0 else {
      context.restoreGState()
      return
    }

    // Scale to fit while maintaining aspect ratio
    let scaleX = rect.width / pdfRect.width
    let scaleY = rect.height / pdfRect.height
    let scale = min(scaleX, scaleY)

    // Calculate scaled dimensions
    let scaledWidth = pdfRect.width * scale
    let scaledHeight = pdfRect.height * scale

    // Center the PDF
    let offsetX = (rect.width - scaledWidth) / 2
    let offsetY = (rect.height - scaledHeight) / 2

    // Apply transforms to draw PDF correctly:
    // 1. Move to where bottom-left corner should be (PDF origin is bottom-left)
    // 2. Scale (with Y flip to convert PDF coords to UIKit coords)
    context.translateBy(x: offsetX - pdfRect.origin.x * scale,
                        y: offsetY + scaledHeight + pdfRect.origin.y * scale)
    context.scaleBy(x: scale, y: -scale)

    context.drawPDFPage(pdfPage)

    context.restoreGState()
  }

  private func drawPDFWindowBackground(in context: CGContext, rect: CGRect) {
    let slotHeight = rect.width * (11.0 / 8.5)

    for (index, pageNumber) in windowPageNumbers.enumerated() {
      guard let page = pageCache[pageNumber] ?? pdfDocument?.page(at: pageNumber) else {
        continue
      }

      let slotRect = CGRect(
        x: rect.origin.x,
        y: rect.origin.y + CGFloat(index) * slotHeight,
        width: rect.width,
        height: slotHeight
      )
      drawPDFBackground(in: context, rect: slotRect, pdfPage: page)
    }
  }

  // MARK: - Public API

  /// Draw background into any context (for exports)
  func drawIntoContext(_ context: CGContext, rect: CGRect) {
    context.saveGState()

    // Fill background based on theme
    if isDarkMode {
      context.setFillColor(UIColor(red: 0.15, green: 0.15, blue: 0.15, alpha: 1.0).cgColor)
    } else {
      context.setFillColor(UIColor.white.cgColor)
    }
    context.fill(rect)

    // Draw pattern based on type
    switch backgroundType {
    case "lined":
      drawLinedBackground(in: context, rect: rect)
    case "grid":
      drawGridBackground(in: context, rect: rect)
    case "dotted":
      drawDottedBackground(in: context, rect: rect)
    case "graph":
      drawGraphBackground(in: context, rect: rect)
    case "pdf":
      if windowPageNumbers.count > 1 {
        drawPDFWindowBackground(in: context, rect: rect)
      } else if let pdfPage = pdfPage {
        drawPDFBackground(in: context, rect: rect, pdfPage: pdfPage)
      }
    default:
      break // plain - just background color
    }

    context.restoreGState()
  }

  func loadPDFBackground(uri: String?) {
    if !Thread.isMainThread {
      DispatchQueue.main.async { [weak self] in
        self?.loadPDFBackground(uri: uri)
      }
      return
    }

    pdfLoadToken &+= 1
    let requestToken = pdfLoadToken

    guard let uri = uri, !uri.isEmpty else {
      resetPdfState(clearCurrentUri: true)
      return
    }

    // Parse page number from URI fragment (#page=N)
    var pageNumber = 1
    var requestedPageNumbers: [Int] = []
    var cleanUri = uri

    if let hashIndex = uri.firstIndex(of: "#") {
      let fragment = String(uri[uri.index(after: hashIndex)...])
      if fragment.hasPrefix("page="), let pageStr = fragment.split(separator: "=").last, let num = Int(pageStr) {
        pageNumber = num
        requestedPageNumbers = [num]
      } else if fragment.hasPrefix("pages=") {
        requestedPageNumbers = fragment
          .dropFirst("pages=".count)
          .split(separator: ",")
          .compactMap { Int($0) }
          .filter { $0 > 0 }
        if let firstPageNumber = requestedPageNumbers.first {
          pageNumber = firstPageNumber
        }
      }
      cleanUri = String(uri[..<hashIndex])
    }

    if requestedPageNumbers.isEmpty {
      requestedPageNumbers = [pageNumber]
    }

    // Check if we already have this page cached
    if cleanUri == currentPdfUri, let cachedPage = pageCache[pageNumber] {
      completePdfLoad(false, pageNumber: pageNumber)
      pdfPage = cachedPage
      currentPageNumber = pageNumber
      windowPageNumbers = requestedPageNumbers
      preloadPages(requestedPageNumbers)
      updateCacheOrder(pageNumber)
      preloadAdjacentPages(around: pageNumber, requestToken: requestToken)
      return
    }

    // Check if same document, just different page
    if cleanUri == currentPdfUri, let document = pdfDocument {
      let validPageNumber = min(max(pageNumber, 1), max(document.numberOfPages, 1))
      if let page = getOrCachePage(validPageNumber, from: document) {
        completePdfLoad(false, pageNumber: validPageNumber)
        pdfPage = page
        currentPageNumber = validPageNumber
        windowPageNumbers = requestedPageNumbers
        preloadPages(requestedPageNumbers)
        preloadAdjacentPages(around: validPageNumber, requestToken: requestToken)
      } else {
        failPdfLoad(
          requestToken: requestToken,
          uri: cleanUri,
          pageNumber: validPageNumber,
          reason: "requested_page_not_found"
        )
      }
      return
    }

    // New document - load async
    loadPDFAsync(
      cleanUri: cleanUri,
      pageNumber: pageNumber,
      requestedPageNumbers: requestedPageNumbers,
      requestToken: requestToken
    )
  }

  // MARK: - PDF Loading

  private func loadPDFAsync(
    cleanUri: String,
    pageNumber: Int,
    requestedPageNumbers: [Int],
    requestToken: UInt64
  ) {
    completePdfLoad(true, pageNumber: pageNumber)

    DispatchQueue.global(qos: .userInitiated).async { [weak self] in
      let data = self?.loadPDFData(from: cleanUri)

      DispatchQueue.main.async {
        guard let self = self else { return }
        guard requestToken == self.pdfLoadToken else {
          print("[MobileInkBackgroundView] Dropping stale PDF load for token \(requestToken)")
          return
        }
        guard let data = data else {
          self.failPdfLoad(
            requestToken: requestToken,
            uri: cleanUri,
            pageNumber: pageNumber,
            reason: "pdf_data_unavailable"
          )
          return
        }
        self.processPDFData(
          data,
          pageNumber: pageNumber,
          requestedPageNumbers: requestedPageNumbers,
          uri: cleanUri,
          requestToken: requestToken
        )
      }
    }
  }

  private func loadPDFData(from cleanUri: String) -> Data? {
    // Handle data URIs (base64)
    if cleanUri.hasPrefix("data:application/pdf;base64,") {
      let base64String = String(cleanUri.dropFirst("data:application/pdf;base64,".count))
      return Data(base64Encoded: base64String)
    }

    // Handle file:// URLs
    if cleanUri.hasPrefix("file://") {
      let filePath = String(cleanUri.dropFirst("file://".count))
      let url = URL(fileURLWithPath: filePath)
      return try? Data(contentsOf: url)
    }

    // Handle raw file paths
    if cleanUri.hasPrefix("/") {
      let url = URL(fileURLWithPath: cleanUri)
      return try? Data(contentsOf: url)
    }

    // Handle remote URLs
    if cleanUri.hasPrefix("http://") || cleanUri.hasPrefix("https://") {
      guard let url = URL(string: cleanUri) else { return nil }
      return try? Data(contentsOf: url)
    }

    return nil
  }

  private func processPDFData(
    _ data: Data,
    pageNumber: Int,
    requestedPageNumbers: [Int],
    uri: String,
    requestToken: UInt64
  ) {
    guard requestToken == pdfLoadToken else {
      print("[MobileInkBackgroundView] Ignoring stale PDF process request for token \(requestToken)")
      return
    }

    guard let dataProvider = CGDataProvider(data: data as CFData),
          let document = CGPDFDocument(dataProvider) else {
      failPdfLoad(
        requestToken: requestToken,
        uri: uri,
        pageNumber: pageNumber,
        reason: "pdf_document_parse_failed"
      )
      return
    }

    // Clear old cache if switching documents
    if uri != currentPdfUri {
      pageCache.removeAll()
      pageCacheOrder.removeAll()
    }

    // Store document and update state
    pdfDocument = document
    currentPdfUri = uri
    currentPageNumber = pageNumber
    windowPageNumbers = requestedPageNumbers

    let totalPages = document.numberOfPages
    guard totalPages > 0 else {
      failPdfLoad(
        requestToken: requestToken,
        uri: uri,
        pageNumber: pageNumber,
        reason: "pdf_document_has_no_pages"
      )
      return
    }

    // Get and cache the requested page
    let validPageNumber = min(max(pageNumber, 1), totalPages)
    if let page = getOrCachePage(validPageNumber, from: document) {
      pdfPage = page
      completePdfLoad(false, pageNumber: validPageNumber)
    } else {
      failPdfLoad(
        requestToken: requestToken,
        uri: uri,
        pageNumber: validPageNumber,
        reason: "requested_page_not_found"
      )
      return
    }

    // Preload adjacent pages
    preloadPages(requestedPageNumbers)
    preloadAdjacentPages(around: validPageNumber, requestToken: requestToken)
  }

  // MARK: - Page Caching

  private func getOrCachePage(_ pageNumber: Int, from document: CGPDFDocument) -> CGPDFPage? {
    if let cached = pageCache[pageNumber] {
      updateCacheOrder(pageNumber)
      return cached
    }

    guard let page = document.page(at: pageNumber) else { return nil }

    // Add to cache
    pageCache[pageNumber] = page
    pageCacheOrder.append(pageNumber)

    // Evict oldest if over limit
    while pageCacheOrder.count > maxCachedPages {
      if let oldest = pageCacheOrder.first {
        pageCache.removeValue(forKey: oldest)
        pageCacheOrder.removeFirst()
      }
    }

    return page
  }

  private func updateCacheOrder(_ pageNumber: Int) {
    if let index = pageCacheOrder.firstIndex(of: pageNumber) {
      pageCacheOrder.remove(at: index)
      pageCacheOrder.append(pageNumber)
    }
  }

  private func preloadAdjacentPages(around pageNumber: Int, requestToken: UInt64) {
    guard requestToken == pdfLoadToken else { return }
    guard let document = pdfDocument else { return }

    let totalPages = document.numberOfPages
    let pagesToPreload = [pageNumber - 1, pageNumber + 1]
      .filter { $0 >= 1 && $0 <= totalPages && pageCache[$0] == nil }

    guard !pagesToPreload.isEmpty else { return }

    for num in pagesToPreload {
      _ = getOrCachePage(num, from: document)
    }
  }

  /// Preload specific pages (called from React)
  func preloadPages(_ pageNumbers: [Int]) {
    if !Thread.isMainThread {
      DispatchQueue.main.async { [weak self] in
        self?.preloadPages(pageNumbers)
      }
      return
    }

    guard let document = pdfDocument else { return }

    let totalPages = document.numberOfPages
    let validPages = pageNumbers.filter { $0 >= 1 && $0 <= totalPages && pageCache[$0] == nil }

    guard !validPages.isEmpty else { return }

    for num in validPages {
      _ = getOrCachePage(num, from: document)
    }
  }
}
