#import <Foundation/Foundation.h>

// Don't include Skia headers here - they're included by SkiaDrawingEngine.h
// Just include our engine which has its own Skia includes
#include "cpp/SkiaDrawingEngine.h"
#include "cpp/DrawingSerialization.h"

// Additional Skia includes for batch export PDF handling
// Note: SkImages namespace functions are in SkImage.h
#include <include/core/SkImage.h>
#include <include/core/SkData.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

struct PageStrokeSegment {
    int pageIndex = 0;
    std::vector<nativedrawing::Point> points;
};

int clampPageIndexForY(float y, int pageCount, float pageHeight) {
    if (pageCount <= 0 || pageHeight <= 0.0f) {
        return 0;
    }

    const int unclamped = static_cast<int>(std::floor(y / pageHeight));
    return std::max(0, std::min(pageCount - 1, unclamped));
}

nativedrawing::Point interpolatePointAtY(
    const nativedrawing::Point& start,
    const nativedrawing::Point& end,
    float targetY
) {
    const float denominator = end.y - start.y;
    const float rawT = std::fabs(denominator) < 0.0001f
        ? 0.0f
        : (targetY - start.y) / denominator;
    const float t = std::max(0.0f, std::min(1.0f, rawT));

    nativedrawing::Point interpolated;
    interpolated.x = start.x + (end.x - start.x) * t;
    interpolated.y = targetY;
    interpolated.pressure = start.pressure + (end.pressure - start.pressure) * t;
    interpolated.azimuthAngle = start.azimuthAngle + (end.azimuthAngle - start.azimuthAngle) * t;
    interpolated.altitude = start.altitude + (end.altitude - start.altitude) * t;
    interpolated.calculatedWidth = start.calculatedWidth + (end.calculatedWidth - start.calculatedWidth) * t;
    interpolated.timestamp = start.timestamp + static_cast<long>((end.timestamp - start.timestamp) * t);
    return interpolated;
}

bool deserializeDrawingBytes(
    const uint8_t* data,
    int size,
    nativedrawing::DrawingSerialization& serializer,
    std::vector<nativedrawing::Stroke>& strokes
) {
    if (!data || size <= 0) {
        strokes.clear();
        return true;
    }

    const std::vector<uint8_t> buffer(data, data + size);
    return serializer.deserialize(buffer, strokes);
}

void translateStrokeInPlace(
    nativedrawing::Stroke& stroke,
    float deltaY,
    size_t affectedStrokeOffset
) {
    for (auto& point : stroke.points) {
        point.y += deltaY;
    }

    for (auto& circle : stroke.erasedBy) {
        circle.y += deltaY;
    }

    if (stroke.isEraser && affectedStrokeOffset > 0) {
        std::unordered_set<size_t> translatedIndices;
        for (size_t index : stroke.affectedStrokeIndices) {
            translatedIndices.insert(index + affectedStrokeOffset);
        }
        stroke.affectedStrokeIndices = std::move(translatedIndices);
    }

    stroke.path.reset();
    stroke.pathLength = 0.0f;
    stroke.cachedEraserPath.reset();
    stroke.cachedEraserCount = 0;
    stroke.cachedHasVisiblePoints = true;
}

std::vector<PageStrokeSegment> splitStrokeAcrossPages(
    const nativedrawing::Stroke& stroke,
    int pageCount,
    float pageHeight
) {
    std::vector<PageStrokeSegment> segments;
    if (stroke.points.empty() || pageCount <= 0 || pageHeight <= 0.0f) {
        return segments;
    }

    const int firstPageIndex = clampPageIndexForY(stroke.points.front().y, pageCount, pageHeight);
    segments.push_back(PageStrokeSegment{ firstPageIndex, { stroke.points.front() } });

    for (size_t index = 1; index < stroke.points.size(); ++index) {
        const nativedrawing::Point& previousPoint = stroke.points[index - 1];
        const nativedrawing::Point& currentPoint = stroke.points[index];
        const int previousPageIndex = clampPageIndexForY(previousPoint.y, pageCount, pageHeight);
        const int currentPageIndex = clampPageIndexForY(currentPoint.y, pageCount, pageHeight);

        if (previousPageIndex == currentPageIndex || std::fabs(currentPoint.y - previousPoint.y) < 0.0001f) {
          segments.back().points.push_back(currentPoint);
          continue;
        }

        int traversedPageIndex = previousPageIndex;
        const int direction = currentPageIndex > previousPageIndex ? 1 : -1;

        while (traversedPageIndex != currentPageIndex) {
            const float boundaryY = direction > 0
                ? static_cast<float>(traversedPageIndex + 1) * pageHeight
                : static_cast<float>(traversedPageIndex) * pageHeight;

            nativedrawing::Point boundaryPoint = interpolatePointAtY(
                previousPoint,
                currentPoint,
                boundaryY
            );
            segments.back().points.push_back(boundaryPoint);

            traversedPageIndex += direction;
            PageStrokeSegment nextSegment;
            nextSegment.pageIndex = traversedPageIndex;
            nextSegment.points.push_back(boundaryPoint);
            segments.push_back(std::move(nextSegment));
        }

        segments.back().points.push_back(currentPoint);
    }

    return segments;
}

nativedrawing::Stroke makePageLocalStroke(
    const nativedrawing::Stroke& originalStroke,
    const PageStrokeSegment& segment,
    float pageHeight
) {
    nativedrawing::Stroke pageStroke = originalStroke;
    pageStroke.points.clear();
    pageStroke.points.reserve(segment.points.size());
    pageStroke.affectedStrokeIndices.clear();
    pageStroke.erasedBy.clear();
    pageStroke.path.reset();
    pageStroke.pathLength = 0.0f;
    pageStroke.cachedEraserPath.reset();
    pageStroke.cachedEraserCount = 0;
    pageStroke.cachedHasVisiblePoints = true;

    const float pageTop = static_cast<float>(segment.pageIndex) * pageHeight;
    const float pageBottom = pageTop + pageHeight;

    for (const auto& point : segment.points) {
        nativedrawing::Point localPoint = point;
        localPoint.y -= pageTop;
        pageStroke.points.push_back(localPoint);
    }

    for (const auto& circle : originalStroke.erasedBy) {
        if ((circle.y + circle.radius) < pageTop || (circle.y - circle.radius) > pageBottom) {
            continue;
        }

        nativedrawing::EraserCircle localCircle = circle;
        localCircle.y -= pageTop;
        pageStroke.erasedBy.push_back(localCircle);
    }

    return pageStroke;
}

} // namespace

using namespace nativedrawing;

// C wrapper functions for Swift
extern "C" {

void* createDrawingEngine(int width, int height) {
    return new SkiaDrawingEngine(width, height);
}

void destroyDrawingEngine(void* engine) {
    if (engine) {
        delete static_cast<SkiaDrawingEngine*>(engine);
    }
}

void touchBegan(void* engine, float x, float y, float pressure, float azimuth, float altitude, long timestamp, bool isPencilInput) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->touchBegan(x, y, pressure, azimuth, altitude, timestamp, isPencilInput);
    }
}

void touchMoved(void* engine, float x, float y, float pressure, float azimuth, float altitude, long timestamp, bool isPencilInput) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->touchMoved(x, y, pressure, azimuth, altitude, timestamp, isPencilInput);
    }
}

void touchEnded(void* engine, long timestamp) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->touchEnded(timestamp);
    }
}

bool updateHoldShapePreview(void* engine, long timestamp) {
    if (!engine) {
        return false;
    }

    return static_cast<SkiaDrawingEngine*>(engine)->updateHoldShapePreview(timestamp);
}

// Predictive touch support for Apple Pencil low-latency rendering
void clearPredictedPoints(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->clearPredictedPoints();
    }
}

void addPredictedPoint(void* engine, float x, float y, float pressure, float azimuth, float altitude, long timestamp, bool isPencilInput) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->addPredictedPoint(x, y, pressure, azimuth, altitude, timestamp, isPencilInput);
    }
}

void clearCanvas(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->clear();
    }
}

void undoStroke(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->undo();
    }
}

void redoStroke(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->redo();
    }
}

void setStrokeColor(void* engine, uint32_t color) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->setStrokeColor(color);
    }
}

void setStrokeWidth(void* engine, float width) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->setStrokeWidth(width);
    }
}

void setTool(void* engine, const char* toolType) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->setTool(toolType);
    }
}

void setToolWithParams(void* engine, const char* toolType, float width, uint32_t color, const char* eraserMode) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->setToolWithParams(toolType, width, color, eraserMode);
    }
}

void setBackgroundType(void* engine, const char* backgroundType) {
    if (engine && backgroundType) {
        static_cast<SkiaDrawingEngine*>(engine)->setBackgroundType(backgroundType);
    }
}

void eraseObjects(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->eraseObjects();
    }
}

bool canUndo(void* engine) {
    if (engine) {
        return static_cast<SkiaDrawingEngine*>(engine)->canUndo();
    }
    return false;
}

bool canRedo(void* engine) {
    if (engine) {
        return static_cast<SkiaDrawingEngine*>(engine)->canRedo();
    }
    return false;
}

bool isEmpty(void* engine) {
    if (engine) {
        return static_cast<SkiaDrawingEngine*>(engine)->isEmpty();
    }
    return true;
}

void renderToCanvas(void* engine, void* canvas) {
    if (engine && canvas) {
        static_cast<SkiaDrawingEngine*>(engine)->render(static_cast<SkCanvas*>(canvas));
    }
}

void* createSkiaCanvas(void* pixels, int width, int height, int rowBytes) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
    if (surface) {
        SkCanvas* canvas = surface->getCanvas();
        surface->ref();
        return canvas;
    }
    return nullptr;
}

void scaleSkiaCanvas(void* canvas, float scaleX, float scaleY) {
    if (canvas) {
        SkCanvas* skCanvas = static_cast<SkCanvas*>(canvas);
        skCanvas->scale(scaleX, scaleY);
    }
}

void destroySkiaCanvas(void* canvas) {
    if (canvas) {
        SkCanvas* skCanvas = static_cast<SkCanvas*>(canvas);
        skCanvas->getSurface()->unref();
    }
}

// Selection operations
bool selectStrokeAt(void* engine, float x, float y) {
    if (engine) {
        return static_cast<SkiaDrawingEngine*>(engine)->selectStrokeAt(x, y);
    }
    return false;
}

bool selectShapeStrokeAt(void* engine, float x, float y) {
    if (engine) {
        return static_cast<SkiaDrawingEngine*>(engine)->selectShapeStrokeAt(x, y);
    }
    return false;
}

void clearSelection(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->clearSelection();
    }
}

void deleteSelection(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->deleteSelection();
    }
}

void copySelection(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->copySelection();
    }
}

void pasteSelection(void* engine, float offsetX, float offsetY) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->pasteSelection(offsetX, offsetY);
    }
}

void moveSelection(void* engine, float dx, float dy) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->moveSelection(dx, dy);
    }
}

void finalizeMove(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->finalizeMove();
    }
}

void beginSelectionTransform(void* engine, int handleIndex) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->beginSelectionTransform(handleIndex);
    }
}

void updateSelectionTransform(void* engine, float x, float y) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->updateSelectionTransform(x, y);
    }
}

void finalizeSelectionTransform(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->finalizeSelectionTransform();
    }
}

void cancelSelectionTransform(void* engine) {
    if (engine) {
        static_cast<SkiaDrawingEngine*>(engine)->cancelSelectionTransform();
    }
}

int getSelectionCount(void* engine) {
    if (engine) {
        return static_cast<SkiaDrawingEngine*>(engine)->getSelectionCount();
    }
    return 0;
}

void getSelectionBounds(void* engine, float* outBounds) {
    if (engine && outBounds) {
        std::vector<float> bounds = static_cast<SkiaDrawingEngine*>(engine)->getSelectionBounds();
        if (bounds.size() == 4) {
            outBounds[0] = bounds[0]; // minX
            outBounds[1] = bounds[1]; // minY
            outBounds[2] = bounds[2]; // maxX
            outBounds[3] = bounds[3]; // maxY
        }
    }
}

// Serialization operations
uint8_t* serializeDrawing(void* engine, int* outSize) {
    if (!engine || !outSize) {
        *outSize = 0;
        return nullptr;
    }

    std::vector<uint8_t> data = static_cast<SkiaDrawingEngine*>(engine)->serializeDrawing();
    *outSize = static_cast<int>(data.size());

    if (data.empty()) {
        return nullptr;
    }

    // Allocate buffer that Swift must free using freeSerializedData
    uint8_t* buffer = new uint8_t[data.size()];
    std::memcpy(buffer, data.data(), data.size());
    return buffer;
}

void freeSerializedData(uint8_t* data) {
    if (data) {
        delete[] data;
    }
}

bool deserializeDrawing(void* engine, const uint8_t* data, int size) {
    if (!engine || !data || size <= 0) {
        return false;
    }

    std::vector<uint8_t> buffer(data, data + size);
    return static_cast<SkiaDrawingEngine*>(engine)->deserializeDrawing(buffer);
}

uint8_t* composeContinuousWindow(
    const uint8_t** pageDataPtrs,
    const int* pageDataSizes,
    int pageCount,
    float pageHeight,
    int* outSize
) {
    if (!outSize) {
        return nullptr;
    }

    *outSize = 0;
    if (!pageDataPtrs || !pageDataSizes || pageCount <= 0 || pageHeight <= 0.0f) {
        return nullptr;
    }

    DrawingSerialization serializer;
    std::vector<Stroke> combinedStrokes;

    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        std::vector<Stroke> pageStrokes;
        if (!deserializeDrawingBytes(pageDataPtrs[pageIndex], pageDataSizes[pageIndex], serializer, pageStrokes)) {
            continue;
        }

        const size_t affectedStrokeOffset = combinedStrokes.size();
        const float yOffset = static_cast<float>(pageIndex) * pageHeight;
        for (auto& stroke : pageStrokes) {
            translateStrokeInPlace(stroke, yOffset, affectedStrokeOffset);
            combinedStrokes.push_back(std::move(stroke));
        }
    }

    if (combinedStrokes.empty()) {
        return nullptr;
    }

    std::vector<uint8_t> serialized = serializer.serialize(combinedStrokes);
    if (serialized.empty()) {
        return nullptr;
    }

    *outSize = static_cast<int>(serialized.size());
    uint8_t* buffer = new uint8_t[serialized.size()];
    std::memcpy(buffer, serialized.data(), serialized.size());
    return buffer;
}

int decomposeContinuousWindow(
    const uint8_t* windowData,
    int windowDataSize,
    int pageCount,
    float pageHeight,
    uint8_t** outPageDataPtrs,
    int* outPageDataSizes
) {
    if (!outPageDataPtrs || !outPageDataSizes || pageCount <= 0 || pageHeight <= 0.0f) {
        return 0;
    }

    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        outPageDataPtrs[pageIndex] = nullptr;
        outPageDataSizes[pageIndex] = 0;
    }

    if (!windowData || windowDataSize <= 0) {
        return pageCount;
    }

    DrawingSerialization serializer;
    std::vector<Stroke> combinedStrokes;
    if (!deserializeDrawingBytes(windowData, windowDataSize, serializer, combinedStrokes)) {
        return 0;
    }

    std::vector<std::vector<Stroke>> pageStrokes(static_cast<size_t>(pageCount));
    std::vector<std::unordered_map<int, std::vector<size_t>>> outputIndexMap(combinedStrokes.size());

    for (size_t originalStrokeIndex = 0; originalStrokeIndex < combinedStrokes.size(); ++originalStrokeIndex) {
        const Stroke& originalStroke = combinedStrokes[originalStrokeIndex];
        std::vector<PageStrokeSegment> segments = splitStrokeAcrossPages(originalStroke, pageCount, pageHeight);
        for (const auto& segment : segments) {
            Stroke pageLocalStroke = makePageLocalStroke(originalStroke, segment, pageHeight);
            pageStrokes[segment.pageIndex].push_back(std::move(pageLocalStroke));
            outputIndexMap[originalStrokeIndex][segment.pageIndex].push_back(
                pageStrokes[segment.pageIndex].size() - 1
            );
        }
    }

    for (size_t originalStrokeIndex = 0; originalStrokeIndex < combinedStrokes.size(); ++originalStrokeIndex) {
        const Stroke& originalStroke = combinedStrokes[originalStrokeIndex];
        if (!originalStroke.isEraser) {
            continue;
        }

        for (const auto& pageEntry : outputIndexMap[originalStrokeIndex]) {
            const int pageIndex = pageEntry.first;
            const std::vector<size_t>& eraserSegmentIndices = pageEntry.second;
            std::unordered_set<size_t> remappedAffectedIndices;
            for (size_t affectedOriginalIndex : originalStroke.affectedStrokeIndices) {
                if (affectedOriginalIndex >= outputIndexMap.size()) {
                    continue;
                }

                const auto affectedSegmentsIt = outputIndexMap[affectedOriginalIndex].find(pageIndex);
                if (affectedSegmentsIt == outputIndexMap[affectedOriginalIndex].end()) {
                    continue;
                }

                remappedAffectedIndices.insert(
                    affectedSegmentsIt->second.begin(),
                    affectedSegmentsIt->second.end()
                );
            }

            for (size_t eraserSegmentIndex : eraserSegmentIndices) {
                if (eraserSegmentIndex < pageStrokes[pageIndex].size()) {
                    pageStrokes[pageIndex][eraserSegmentIndex].affectedStrokeIndices = remappedAffectedIndices;
                }
            }
        }
    }

    for (int pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        if (pageStrokes[pageIndex].empty()) {
            continue;
        }

        std::vector<uint8_t> serialized = serializer.serialize(pageStrokes[pageIndex]);
        if (serialized.empty()) {
            continue;
        }

        outPageDataSizes[pageIndex] = static_cast<int>(serialized.size());
        uint8_t* buffer = new uint8_t[serialized.size()];
        std::memcpy(buffer, serialized.data(), serialized.size());
        outPageDataPtrs[pageIndex] = buffer;
    }

    return pageCount;
}

void* makeSnapshot(void* engine) {
    if (!engine) {
        return nullptr;
    }

    sk_sp<SkImage> image = static_cast<SkiaDrawingEngine*>(engine)->makeSnapshot();
    if (image) {
        image->ref(); // Increment ref count for Swift ownership
        return image.get();
    }
    return nullptr;
}

void releaseSkImage(void* image) {
    if (image) {
        static_cast<SkImage*>(image)->unref();
    }
}

// Batch export multiple pages to PNG
// pagesDataPtrs: array of pointers to serialized drawing data
// pagesDataSizes: array of sizes for each data buffer
// backgroundTypes: array of background type strings (e.g., "plain", "lined", "pdf")
// pdfPixelDataPtrs: array of pointers to RGBA pixel data for PDF backgrounds (can be null)
// pdfPixelDataWidths: array of PDF image widths (0 if no PDF for that page)
// pdfPixelDataHeights: array of PDF image heights (0 if no PDF for that page)
// numPages: number of pages
// width, height: canvas dimensions
// scale: export scale factor
// outResults: array to receive base64 PNG strings (caller allocates numPages pointers)
// outResultLengths: array to receive lengths of each result string
// Returns number of successfully exported pages
int batchExportPages(
    const uint8_t** pagesDataPtrs,
    const int* pagesDataSizes,
    const char** backgroundTypes,
    const uint8_t** pdfPixelDataPtrs,
    const int* pdfPixelDataWidths,
    const int* pdfPixelDataHeights,
    const int* pageIndices,
    int numPages,
    int width,
    int height,
    float scale,
    char** outResults,
    int* outResultLengths
) {
    if (!pagesDataPtrs || !pagesDataSizes || !backgroundTypes ||
        !outResults || !outResultLengths || numPages <= 0) {
        return 0;
    }

    // Create a temporary engine for batch processing
    SkiaDrawingEngine* engine = new SkiaDrawingEngine(width, height);

    // Prepare page data and background type vectors
    std::vector<std::vector<uint8_t>> pagesData;
    std::vector<std::string> bgTypes;
    std::vector<sk_sp<SkImage>> pdfBackgrounds;
    std::vector<int> resolvedPageIndices;
    pagesData.reserve(numPages);
    bgTypes.reserve(numPages);
    pdfBackgrounds.reserve(numPages);
    resolvedPageIndices.reserve(numPages);

    for (int i = 0; i < numPages; ++i) {
        if (pagesDataPtrs[i] && pagesDataSizes[i] > 0) {
            pagesData.push_back(std::vector<uint8_t>(
                pagesDataPtrs[i],
                pagesDataPtrs[i] + pagesDataSizes[i]
            ));
        } else {
            pagesData.push_back(std::vector<uint8_t>());
        }

        bgTypes.push_back(backgroundTypes[i] ? backgroundTypes[i] : "plain");
        resolvedPageIndices.push_back(pageIndices ? pageIndices[i] : i);

        // Create SkImage from PDF pixel data if provided
        if (pdfPixelDataPtrs && pdfPixelDataPtrs[i] &&
            pdfPixelDataWidths && pdfPixelDataHeights &&
            pdfPixelDataWidths[i] > 0 && pdfPixelDataHeights[i] > 0) {
            int pdfWidth = pdfPixelDataWidths[i];
            int pdfHeight = pdfPixelDataHeights[i];
            size_t rowBytes = pdfWidth * 4; // RGBA

            // Create SkImageInfo for the pixel data
            SkImageInfo info = SkImageInfo::Make(
                pdfWidth, pdfHeight,
                kRGBA_8888_SkColorType,
                kPremul_SkAlphaType
            );

            // Make a copy of the pixel data since SkImage needs to own it
            size_t dataSize = rowBytes * pdfHeight;
            sk_sp<SkData> pixelData = SkData::MakeWithCopy(pdfPixelDataPtrs[i], dataSize);

            // Create SkImage from the pixel data
            sk_sp<SkImage> pdfImage = SkImages::RasterFromData(info, pixelData, rowBytes);
            pdfBackgrounds.push_back(pdfImage);
        } else {
            pdfBackgrounds.push_back(nullptr);
        }
    }

    // Call batch export with PDF backgrounds
    std::vector<std::string> results = engine->batchExportPages(
        pagesData,
        bgTypes,
        pdfBackgrounds,
        resolvedPageIndices,
        scale
    );

    // Copy results to output arrays
    int successCount = 0;
    for (int i = 0; i < numPages && i < (int)results.size(); ++i) {
        if (!results[i].empty()) {
            outResultLengths[i] = static_cast<int>(results[i].size());
            outResults[i] = new char[results[i].size() + 1];
            std::memcpy(outResults[i], results[i].c_str(), results[i].size() + 1);
            successCount++;
        } else {
            outResults[i] = nullptr;
            outResultLengths[i] = 0;
        }
    }

    delete engine;
    return successCount;
}

// Free a batch export result string
void freeBatchExportResult(char* result) {
    if (result) {
        delete[] result;
    }
}

} // extern "C"
