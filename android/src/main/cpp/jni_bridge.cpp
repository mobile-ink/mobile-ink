#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <GLES2/gl2.h>
#include "SkiaDrawingEngine.h"
#include "DrawingSerialization.h"
#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkImage.h>
#include <include/core/SkData.h>
#include <include/core/SkColorSpace.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#include <include/gpu/ganesh/gl/GrGLTypes.h>
#include <src/gpu/ganesh/gl/GrGLDefines.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "MobileInk"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace nativedrawing;

namespace {

struct PageStrokeSegment {
    int pageIndex = 0;
    std::vector<Point> points;
};

int clampPageIndexForY(float y, int pageCount, float pageHeight) {
    if (pageCount <= 0 || pageHeight <= 0.0f) {
        return 0;
    }

    const int unclamped = static_cast<int>(std::floor(y / pageHeight));
    return std::max(0, std::min(pageCount - 1, unclamped));
}

Point interpolatePointAtY(const Point& start, const Point& end, float targetY) {
    const float denominator = end.y - start.y;
    const float rawT = std::fabs(denominator) < 0.0001f
        ? 0.0f
        : (targetY - start.y) / denominator;
    const float t = std::max(0.0f, std::min(1.0f, rawT));

    Point interpolated;
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
    DrawingSerialization& serializer,
    std::vector<Stroke>& strokes
) {
    if (!data || size <= 0) {
        strokes.clear();
        return true;
    }

    const std::vector<uint8_t> buffer(data, data + size);
    return serializer.deserialize(buffer, strokes);
}

void translateStrokeInPlace(Stroke& stroke, float deltaY, size_t affectedStrokeOffset) {
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
    const Stroke& stroke,
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
        const Point& previousPoint = stroke.points[index - 1];
        const Point& currentPoint = stroke.points[index];
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

            Point boundaryPoint = interpolatePointAtY(previousPoint, currentPoint, boundaryY);
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

Stroke makePageLocalStroke(const Stroke& originalStroke, const PageStrokeSegment& segment, float pageHeight) {
    Stroke pageStroke = originalStroke;
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
        Point localPoint = point;
        localPoint.y -= pageTop;
        pageStroke.points.push_back(localPoint);
    }

    for (const auto& circle : originalStroke.erasedBy) {
        if ((circle.y + circle.radius) < pageTop || (circle.y - circle.radius) > pageBottom) {
            continue;
        }

        EraserCircle localCircle = circle;
        localCircle.y -= pageTop;
        pageStroke.erasedBy.push_back(localCircle);
    }

    return pageStroke;
}

std::vector<uint8_t> composeContinuousWindowBytes(
    const std::vector<std::vector<uint8_t>>& pageData,
    float pageHeight
) {
    if (pageData.empty() || pageHeight <= 0.0f) {
        return {};
    }

    DrawingSerialization serializer;
    std::vector<Stroke> combinedStrokes;

    for (size_t pageIndex = 0; pageIndex < pageData.size(); ++pageIndex) {
        std::vector<Stroke> pageStrokes;
        const auto& data = pageData[pageIndex];
        if (!deserializeDrawingBytes(data.data(), static_cast<int>(data.size()), serializer, pageStrokes)) {
            continue;
        }

        const size_t affectedStrokeOffset = combinedStrokes.size();
        const float yOffset = static_cast<float>(pageIndex) * pageHeight;
        for (auto& stroke : pageStrokes) {
            translateStrokeInPlace(stroke, yOffset, affectedStrokeOffset);
            combinedStrokes.push_back(std::move(stroke));
        }
    }

    return combinedStrokes.empty() ? std::vector<uint8_t>() : serializer.serialize(combinedStrokes);
}

std::vector<std::vector<uint8_t>> decomposeContinuousWindowBytes(
    const uint8_t* windowData,
    int windowDataSize,
    int pageCount,
    float pageHeight
) {
    std::vector<std::vector<uint8_t>> result(static_cast<size_t>(std::max(0, pageCount)));
    if (pageCount <= 0 || pageHeight <= 0.0f) {
        return result;
    }

    if (!windowData || windowDataSize <= 0) {
        return result;
    }

    DrawingSerialization serializer;
    std::vector<Stroke> combinedStrokes;
    if (!deserializeDrawingBytes(windowData, windowDataSize, serializer, combinedStrokes)) {
        result.clear();
        return result;
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
        if (!pageStrokes[pageIndex].empty()) {
            result[pageIndex] = serializer.serialize(pageStrokes[pageIndex]);
        }
    }

    return result;
}

} // namespace

// DrawingContext owns the shared drawing model plus the active Ganesh render surface.
struct DrawingContext {
    std::unique_ptr<SkiaDrawingEngine> engine;
    sk_sp<GrDirectContext> ganeshContext;
    sk_sp<SkSurface> ganeshSurface;
    int ganeshSurfaceWidth = 0;
    int ganeshSurfaceHeight = 0;
    int ganeshSamples = -1;
    int ganeshStencil = -1;

    DrawingContext(int w, int h) {
        engine = std::make_unique<SkiaDrawingEngine>(w, h);
    }

    ~DrawingContext() {
        ganeshSurface = nullptr;
        if (ganeshContext) {
            ganeshContext->abandonContext();
            ganeshContext = nullptr;
        }
    }
};

extern "C" {

// Engine lifecycle
JNIEXPORT jlong JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_createDrawingEngine(
    JNIEnv* env, jobject obj, jint width, jint height) {

    auto* ctx = new DrawingContext(width, height);
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_destroyDrawingEngine(
    JNIEnv* env, jobject obj, jlong contextPtr) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    delete ctx;
}

// Touch handling with stylus support (pressure, azimuth, altitude)
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_touchBegan(
    JNIEnv* env, jobject obj, jlong contextPtr,
    jfloat x, jfloat y, jfloat pressure, jfloat azimuth, jfloat altitude,
    jlong timestamp, jboolean isStylusInput) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->touchBegan(x, y, pressure, azimuth, altitude, static_cast<long>(timestamp), isStylusInput == JNI_TRUE);
    } else {
        LOGE("touchBegan: context or engine is null! ctx=%p", ctx);
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_touchMoved(
    JNIEnv* env, jobject obj, jlong contextPtr,
    jfloat x, jfloat y, jfloat pressure, jfloat azimuth, jfloat altitude,
    jlong timestamp, jboolean isStylusInput) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->touchMoved(x, y, pressure, azimuth, altitude, static_cast<long>(timestamp), isStylusInput == JNI_TRUE);
    } else {
        LOGE("touchMoved: context or engine is null!");
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_touchEnded(JNIEnv* env, jobject obj, jlong contextPtr, jlong timestamp) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->touchEnded(static_cast<long>(timestamp));
    } else {
        LOGE("touchEnded: context or engine is null!");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_updateHoldShapePreview(
    JNIEnv* env, jobject obj, jlong contextPtr, jlong timestamp) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->updateHoldShapePreview(static_cast<long>(timestamp)) ? JNI_TRUE : JNI_FALSE;
    }

    return JNI_FALSE;
}

// Canvas operations
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_clearCanvas(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->clear();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_undoStroke(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->undo();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_redoStroke(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->redo();
    }
}

// Tool settings
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setStrokeColor(
    JNIEnv* env, jobject obj, jlong contextPtr, jint color) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        // Android color is ARGB, Skia uses ARGB too but we need SkColor format
        ctx->engine->setStrokeColor(static_cast<SkColor>(color));
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setStrokeWidth(
    JNIEnv* env, jobject obj, jlong contextPtr, jfloat width) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->setStrokeWidth(width);
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setTool(
    JNIEnv* env, jobject obj, jlong contextPtr, jstring toolType) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        const char* toolStr = env->GetStringUTFChars(toolType, nullptr);
        ctx->engine->setTool(toolStr);
        env->ReleaseStringUTFChars(toolType, toolStr);
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setToolWithParams(
    JNIEnv* env, jobject obj, jlong contextPtr,
    jstring toolType, jfloat width, jint color, jstring eraserMode) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        const char* toolStr = env->GetStringUTFChars(toolType, nullptr);
        const char* eraserStr = eraserMode ? env->GetStringUTFChars(eraserMode, nullptr) : "";

        ctx->engine->setToolWithParams(toolStr, width, static_cast<uint32_t>(color), eraserStr);

        env->ReleaseStringUTFChars(toolType, toolStr);
        if (eraserMode) {
            env->ReleaseStringUTFChars(eraserMode, eraserStr);
        }
    }
}

// Eraser cursor for pixel eraser visualization
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setEraserCursor(
    JNIEnv* env, jobject obj, jlong contextPtr,
    jfloat x, jfloat y, jfloat radius, jboolean visible) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->setEraserCursor(x, y, radius, visible == JNI_TRUE);
    }
}

// Background type for pattern rendering
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setBackgroundType(
    JNIEnv* env, jobject obj, jlong contextPtr, jstring backgroundType) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        const char* typeStr = env->GetStringUTFChars(backgroundType, nullptr);
        ctx->engine->setBackgroundType(typeStr);
        env->ReleaseStringUTFChars(backgroundType, typeStr);
    }
}

// PDF background bitmap - render PDF in Kotlin, pass to C++ as SkImage
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_setPdfBackgroundBitmap(
    JNIEnv* env, jobject obj, jlong contextPtr, jobject bitmap) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (!ctx || !ctx->engine) {
        LOGE("setPdfBackgroundBitmap: context or engine is null");
        return;
    }

    if (bitmap == nullptr) {
        ctx->engine->setPdfBackgroundImage(nullptr);
        return;
    }

    // Get bitmap info
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("setPdfBackgroundBitmap: failed to get bitmap info");
        return;
    }

    // Lock pixels
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("setPdfBackgroundBitmap: failed to lock pixels");
        return;
    }

    // Create SkImage from bitmap pixels
    // Android ARGB_8888 is compatible with Skia N32 (BGRA on little-endian)
    SkImageInfo skInfo = SkImageInfo::MakeN32Premul(info.width, info.height);

    // Copy pixel data since we need to unlock the bitmap
    size_t dataSize = info.height * info.stride;
    sk_sp<SkData> data = SkData::MakeWithCopy(pixels, dataSize);

    AndroidBitmap_unlockPixels(env, bitmap);

    // Create the image from the copied data
    sk_sp<SkImage> image = SkImages::RasterFromData(skInfo, data, info.stride);

    if (image) {
        ctx->engine->setPdfBackgroundImage(image);
    } else {
        LOGE("setPdfBackgroundBitmap: failed to create SkImage");
    }
}

// Selection operations
JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_selectStrokeAt(
    JNIEnv* env, jobject obj, jlong contextPtr, jfloat x, jfloat y) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->selectStrokeAt(x, y) ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_selectShapeStrokeAt(
    JNIEnv* env, jobject obj, jlong contextPtr, jfloat x, jfloat y) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->selectShapeStrokeAt(x, y) ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_clearSelection(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->clearSelection();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_deleteSelection(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->deleteSelection();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_copySelection(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->copySelection();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_pasteSelection(
    JNIEnv* env, jobject obj, jlong contextPtr, jfloat offsetX, jfloat offsetY) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->pasteSelection(offsetX, offsetY);
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_moveSelection(
    JNIEnv* env, jobject obj, jlong contextPtr, jfloat dx, jfloat dy) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->moveSelection(dx, dy);
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_finalizeMove(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->finalizeMove();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_beginSelectionTransform(
    JNIEnv* env, jobject obj, jlong contextPtr, jint handleIndex) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->beginSelectionTransform(static_cast<int>(handleIndex));
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_updateSelectionTransform(
    JNIEnv* env, jobject obj, jlong contextPtr, jfloat x, jfloat y) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->updateSelectionTransform(x, y);
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_finalizeSelectionTransform(
    JNIEnv* env, jobject obj, jlong contextPtr) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->finalizeSelectionTransform();
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_cancelSelectionTransform(
    JNIEnv* env, jobject obj, jlong contextPtr) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        ctx->engine->cancelSelectionTransform();
    }
}

JNIEXPORT jint JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_getSelectionCount(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->getSelectionCount();
    }
    return 0;
}

JNIEXPORT jfloatArray JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_getSelectionBounds(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        std::vector<float> bounds = ctx->engine->getSelectionBounds();
        jfloatArray result = env->NewFloatArray(4);
        env->SetFloatArrayRegion(result, 0, 4, bounds.data());
        return result;
    }
    return nullptr;
}

// State queries
JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_canUndo(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->canUndo() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_canRedo(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->canRedo() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_isEmpty(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        return ctx->engine->isEmpty() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_TRUE;
}

// Serialization
JNIEXPORT jbyteArray JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_serializeDrawing(JNIEnv* env, jobject obj, jlong contextPtr) {
    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine) {
        std::vector<uint8_t> data = ctx->engine->serializeDrawing();
        jbyteArray result = env->NewByteArray(data.size());
        env->SetByteArrayRegion(result, 0, data.size(), reinterpret_cast<const jbyte*>(data.data()));
        return result;
    }
    LOGE("serializeDrawing: ctx or engine is null!");
    return nullptr;
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_deserializeDrawing(
    JNIEnv* env, jobject obj, jlong contextPtr, jbyteArray data) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx && ctx->engine && data) {
        jsize len = env->GetArrayLength(data);
        jbyte* bytes = env->GetByteArrayElements(data, nullptr);

        std::vector<uint8_t> vec(reinterpret_cast<uint8_t*>(bytes),
                                  reinterpret_cast<uint8_t*>(bytes) + len);
        ctx->engine->deserializeDrawing(vec);

        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_renderGaneshToCurrentSurface(
    JNIEnv* env, jobject obj, jlong contextPtr, jint width, jint height) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (!ctx || !ctx->engine || width <= 0 || height <= 0) {
        LOGE("renderGaneshToCurrentSurface: invalid context or size");
        return JNI_FALSE;
    }

    if (!ctx->ganeshContext) {
        auto backendInterface = GrGLMakeNativeInterface();
        if (!backendInterface) {
            LOGE("renderGaneshToCurrentSurface: failed to create GL interface");
            return JNI_FALSE;
        }
        ctx->ganeshContext = GrDirectContexts::MakeGL(backendInterface);
        if (!ctx->ganeshContext) {
            LOGE("renderGaneshToCurrentSurface: failed to create Ganesh context");
            return JNI_FALSE;
        }
    }

    GLint stencil = 0;
    GLint samples = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencil);
    glGetIntegerv(GL_SAMPLES, &samples);

    const auto colorType = kRGBA_8888_SkColorType;
    const auto maxSamples =
        ctx->ganeshContext->maxSurfaceSampleCountForColorType(colorType);
    if (samples > maxSamples) {
        samples = maxSamples;
    }

    if (
        !ctx->ganeshSurface ||
        ctx->ganeshSurfaceWidth != width ||
        ctx->ganeshSurfaceHeight != height ||
        ctx->ganeshSamples != samples ||
        ctx->ganeshStencil != stencil
    ) {
        ctx->ganeshSurface = nullptr;

        GrGLFramebufferInfo fbInfo;
        fbInfo.fFBOID = 0;
        fbInfo.fFormat = GR_GL_RGBA8;

        auto backendRenderTarget =
            GrBackendRenderTargets::MakeGL(width, height, samples, stencil, fbInfo);
        SkSurfaceProps surfaceProps(0, kRGB_H_SkPixelGeometry);
        ctx->ganeshSurface = SkSurfaces::WrapBackendRenderTarget(
            ctx->ganeshContext.get(),
            backendRenderTarget,
            kBottomLeft_GrSurfaceOrigin,
            colorType,
            nullptr,
            &surfaceProps
        );

        if (!ctx->ganeshSurface) {
            LOGE("renderGaneshToCurrentSurface: failed to wrap EGL render target");
            return JNI_FALSE;
        }

        ctx->ganeshSurfaceWidth = width;
        ctx->ganeshSurfaceHeight = height;
        ctx->ganeshSamples = samples;
        ctx->ganeshStencil = stencil;
    }

    SkCanvas* canvas = ctx->ganeshSurface->getCanvas();
    ctx->engine->render(canvas);
    ctx->ganeshContext->flushAndSubmit(ctx->ganeshSurface.get());
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_releaseGaneshContext(
    JNIEnv* env, jobject obj, jlong contextPtr) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (!ctx) {
        return;
    }

    ctx->ganeshSurface = nullptr;
    ctx->ganeshSurfaceWidth = 0;
    ctx->ganeshSurfaceHeight = 0;
    ctx->ganeshSamples = -1;
    ctx->ganeshStencil = -1;

    if (ctx->ganeshContext) {
        ctx->ganeshContext->releaseResourcesAndAbandonContext();
        ctx->ganeshContext = nullptr;
    }
}

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_renderToPixelsScaled(
    JNIEnv* env, jobject obj, jlong contextPtr, jobject bitmap, jfloat scale) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (!ctx || !ctx->engine) {
        LOGE("renderToPixelsScaled: invalid context");
        return;
    }

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("renderToPixelsScaled: failed to get bitmap info");
        return;
    }

    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("renderToPixelsScaled: failed to lock pixels");
        return;
    }

    SkImageInfo skInfo = SkImageInfo::MakeN32Premul(info.width, info.height);
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(skInfo, pixels, info.stride);
    if (!surface) {
        AndroidBitmap_unlockPixels(env, bitmap);
        LOGE("renderToPixelsScaled: failed to wrap bitmap pixels");
        return;
    }

    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    const float resolvedScale = scale > 0.0f ? scale : 1.0f;
    canvas->save();
    canvas->scale(resolvedScale, resolvedScale);
    ctx->engine->render(canvas);
    canvas->restore();

    AndroidBitmap_unlockPixels(env, bitmap);
}

// Batch export multiple pages to PNG images
// This is a static method on MobileInkModule (not tied to a view instance)
// pagesDataArray: Array of byte arrays with serialized drawing data for each page
// backgroundTypes: Array of background type strings ("plain", "lined", "grid", "pdf")
// width, height: Canvas dimensions
// scale: Export scale factor
// Returns: Array of base64 PNG data URIs
JNIEXPORT jobjectArray JNICALL
Java_com_mathnotes_mobileink_MobileInkModule_nativeBatchExportPages(
    JNIEnv* env, jclass clazz,
    jobjectArray pagesDataArray,
    jobjectArray backgroundTypesArray,
    jobjectArray pdfBackgroundsArray,
    jintArray pageIndicesArray,
    jint width, jint height, jfloat scale) {

    int numPages = env->GetArrayLength(pagesDataArray);
    if (numPages == 0) {
        // Return empty array
        jclass stringClass = env->FindClass("java/lang/String");
        return env->NewObjectArray(0, stringClass, nullptr);
    }

    // Create temporary engine for batch processing
    auto engine = std::make_unique<SkiaDrawingEngine>(width, height);

    // Prepare vectors for batch export
    std::vector<std::vector<uint8_t>> pagesData;
    std::vector<std::string> bgTypes;
    std::vector<sk_sp<SkImage>> pdfBackgrounds;
    pagesData.reserve(numPages);
    bgTypes.reserve(numPages);
    pdfBackgrounds.reserve(numPages);

    std::vector<int> pageIndices(numPages);
    if (pageIndicesArray != nullptr && env->GetArrayLength(pageIndicesArray) >= numPages) {
        jint* rawPageIndices = env->GetIntArrayElements(pageIndicesArray, nullptr);
        if (rawPageIndices != nullptr) {
            for (int i = 0; i < numPages; i++) {
                pageIndices[i] = rawPageIndices[i];
            }
            env->ReleaseIntArrayElements(pageIndicesArray, rawPageIndices, JNI_ABORT);
        }
    } else {
        for (int i = 0; i < numPages; i++) {
            pageIndices[i] = i;
        }
    }

    for (int i = 0; i < numPages; i++) {
        std::vector<uint8_t> pageData;
        auto pageBytes = (jbyteArray)env->GetObjectArrayElement(pagesDataArray, i);
        if (pageBytes != nullptr) {
            jsize len = env->GetArrayLength(pageBytes);
            jbyte* bytes = env->GetByteArrayElements(pageBytes, nullptr);
            if (bytes != nullptr) {
                if (len > 0) {
                    pageData.assign(
                        reinterpret_cast<uint8_t*>(bytes),
                        reinterpret_cast<uint8_t*>(bytes) + len
                    );
                }
                env->ReleaseByteArrayElements(pageBytes, bytes, JNI_ABORT);
            }
            env->DeleteLocalRef(pageBytes);
        }

        pagesData.push_back(std::move(pageData));

        // Get background type
        jstring bgTypeStr = (jstring)env->GetObjectArrayElement(backgroundTypesArray, i);
        const char* bgType = env->GetStringUTFChars(bgTypeStr, nullptr);
        bgTypes.push_back(bgType ? bgType : "plain");
        env->ReleaseStringUTFChars(bgTypeStr, bgType);
        env->DeleteLocalRef(bgTypeStr);

        sk_sp<SkImage> pdfImage = nullptr;
        if (pdfBackgroundsArray != nullptr) {
            jobject bitmap = env->GetObjectArrayElement(pdfBackgroundsArray, i);
            if (bitmap != nullptr) {
                AndroidBitmapInfo info;
                if (AndroidBitmap_getInfo(env, bitmap, &info) == ANDROID_BITMAP_RESULT_SUCCESS) {
                    void* pixels = nullptr;
                    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) == ANDROID_BITMAP_RESULT_SUCCESS) {
                        SkImageInfo skInfo = SkImageInfo::MakeN32Premul(info.width, info.height);
                        size_t dataSize = info.height * info.stride;
                        sk_sp<SkData> data = SkData::MakeWithCopy(pixels, dataSize);
                        AndroidBitmap_unlockPixels(env, bitmap);
                        pdfImage = SkImages::RasterFromData(skInfo, data, info.stride);
                    }
                }
                env->DeleteLocalRef(bitmap);
            }
        }
        pdfBackgrounds.push_back(pdfImage);
    }


    // Call batch export on the engine
    std::vector<std::string> results = engine->batchExportPages(
        pagesData, bgTypes, pdfBackgrounds, pageIndices, scale);

    // Convert results to Java String array
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray resultArray = env->NewObjectArray(numPages, stringClass, nullptr);

    for (int i = 0; i < numPages && i < (int)results.size(); i++) {
        if (!results[i].empty()) {
            jstring resultStr = env->NewStringUTF(results[i].c_str());
            env->SetObjectArrayElement(resultArray, i, resultStr);
            env->DeleteLocalRef(resultStr);
        } else {
            // Set empty string for failed exports
            jstring emptyStr = env->NewStringUTF("");
            env->SetObjectArrayElement(resultArray, i, emptyStr);
            env->DeleteLocalRef(emptyStr);
        }
    }

    return resultArray;
}

JNIEXPORT jbyteArray JNICALL
Java_com_mathnotes_mobileink_MobileInkModule_nativeComposeContinuousWindow(
    JNIEnv* env, jclass clazz, jobjectArray pageDataArray, jfloat pageHeight) {

    if (pageDataArray == nullptr || pageHeight <= 0.0f) {
        return nullptr;
    }

    int pageCount = env->GetArrayLength(pageDataArray);
    if (pageCount <= 0) {
        return nullptr;
    }

    std::vector<std::vector<uint8_t>> pageData;
    pageData.reserve(pageCount);

    for (int i = 0; i < pageCount; i++) {
        std::vector<uint8_t> data;
        auto pageBytes = (jbyteArray)env->GetObjectArrayElement(pageDataArray, i);
        if (pageBytes != nullptr) {
            jsize len = env->GetArrayLength(pageBytes);
            jbyte* bytes = env->GetByteArrayElements(pageBytes, nullptr);
            if (bytes != nullptr && len > 0) {
                data.assign(
                    reinterpret_cast<uint8_t*>(bytes),
                    reinterpret_cast<uint8_t*>(bytes) + len
                );
            }
            if (bytes != nullptr) {
                env->ReleaseByteArrayElements(pageBytes, bytes, JNI_ABORT);
            }
            env->DeleteLocalRef(pageBytes);
        }
        pageData.push_back(std::move(data));
    }

    std::vector<uint8_t> output = composeContinuousWindowBytes(pageData, pageHeight);
    if (output.empty()) {
        return nullptr;
    }

    jbyteArray result = env->NewByteArray(output.size());
    env->SetByteArrayRegion(
        result,
        0,
        output.size(),
        reinterpret_cast<const jbyte*>(output.data())
    );
    return result;
}

JNIEXPORT jobjectArray JNICALL
Java_com_mathnotes_mobileink_MobileInkModule_nativeDecomposeContinuousWindow(
    JNIEnv* env, jclass clazz, jbyteArray windowDataArray, jint pageCount, jfloat pageHeight) {

    jclass byteArrayClass = env->FindClass("[B");
    jobjectArray resultArray = env->NewObjectArray(std::max(0, static_cast<int>(pageCount)), byteArrayClass, nullptr);
    if (pageCount <= 0 || pageHeight <= 0.0f) {
        return resultArray;
    }

    std::vector<uint8_t> windowData;
    if (windowDataArray != nullptr) {
        jsize len = env->GetArrayLength(windowDataArray);
        jbyte* bytes = env->GetByteArrayElements(windowDataArray, nullptr);
        if (bytes != nullptr && len > 0) {
            windowData.assign(
                reinterpret_cast<uint8_t*>(bytes),
                reinterpret_cast<uint8_t*>(bytes) + len
            );
        }
        if (bytes != nullptr) {
            env->ReleaseByteArrayElements(windowDataArray, bytes, JNI_ABORT);
        }
    }

    std::vector<std::vector<uint8_t>> pages = decomposeContinuousWindowBytes(
        windowData.empty() ? nullptr : windowData.data(),
        static_cast<int>(windowData.size()),
        pageCount,
        pageHeight
    );

    if (pages.empty() && pageCount > 0 && !windowData.empty()) {
        return resultArray;
    }

    for (int i = 0; i < pageCount && i < static_cast<int>(pages.size()); i++) {
        const auto& page = pages[i];
        if (page.empty()) {
            continue;
        }
        jbyteArray pageArray = env->NewByteArray(page.size());
        env->SetByteArrayRegion(
            pageArray,
            0,
            page.size(),
            reinterpret_cast<const jbyte*>(page.data())
        );
        env->SetObjectArrayElement(resultArray, i, pageArray);
        env->DeleteLocalRef(pageArray);
    }

    return resultArray;
}

} // extern "C"
