#include <jni.h>
#include <android/log.h>
#include <android/bitmap.h>
#include <GLES2/gl2.h>
#include "SkiaDrawingEngine.h"
#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkImage.h>
#include <include/core/SkData.h>
#include <memory>
#include <string>
#include <vector>

#define LOG_TAG "MobileInk"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace nativedrawing;

// DrawingContext holds the engine and a raster surface for CPU rendering
struct DrawingContext {
    std::unique_ptr<SkiaDrawingEngine> engine;
    sk_sp<SkSurface> surface;
    int width;
    int height;

    DrawingContext(int w, int h) : width(w), height(h) {
        engine = std::make_unique<SkiaDrawingEngine>(w, h);

        // Create raster surface for CPU rendering
        SkImageInfo info = SkImageInfo::MakeN32Premul(w, h);
        surface = SkSurfaces::Raster(info);

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

JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_renderToPixels(
    JNIEnv* env, jobject obj, jlong contextPtr, jobject bitmap) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (!ctx || !ctx->engine || !ctx->surface) {
        LOGE("renderToPixels: invalid context");
        return;
    }

    // Get bitmap info
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("renderToPixels: failed to get bitmap info");
        return;
    }

    // Lock pixels
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("renderToPixels: failed to lock pixels");
        return;
    }

    // Render to surface - engine handles background clearing and pattern rendering
    SkCanvas* canvas = ctx->surface->getCanvas();
    ctx->engine->render(canvas);

    // Read pixels from surface into bitmap
    SkImageInfo dstInfo = SkImageInfo::MakeN32Premul(info.width, info.height);
    ctx->surface->readPixels(dstInfo, pixels, info.stride, 0, 0);

    AndroidBitmap_unlockPixels(env, bitmap);
}

// Alternative: render to byte array for cases where bitmap isn't convenient
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_renderToByteArray(
    JNIEnv* env, jobject obj, jlong contextPtr, jbyteArray pixels, jint width, jint height) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (!ctx || !ctx->engine || !ctx->surface) {
        LOGE("renderToByteArray: invalid context");
        return;
    }

    // Render to surface - engine handles background clearing and pattern rendering
    SkCanvas* canvas = ctx->surface->getCanvas();
    ctx->engine->render(canvas);

    // Get pixel data
    jbyte* pixelData = env->GetByteArrayElements(pixels, nullptr);

    SkImageInfo dstInfo = SkImageInfo::MakeN32Premul(width, height);
    ctx->surface->readPixels(dstInfo, pixelData, width * 4, 0, 0);

    env->ReleaseByteArrayElements(pixels, pixelData, 0);
}

// Resize the drawing context when view size changes
JNIEXPORT void JNICALL
Java_com_mathnotes_mobileink_MobileInkCanvasView_resizeEngine(
    JNIEnv* env, jobject obj, jlong contextPtr, jint width, jint height) {

    auto* ctx = reinterpret_cast<DrawingContext*>(contextPtr);
    if (ctx) {
        // Recreate surface with new size
        SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
        ctx->surface = SkSurfaces::Raster(info);
        ctx->width = width;
        ctx->height = height;

        // Note: The engine itself doesn't need resizing as strokes are stored
        // in absolute coordinates. We just need the new render surface.
    }
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

} // extern "C"
