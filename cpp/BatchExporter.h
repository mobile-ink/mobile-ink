#pragma once

#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <include/core/SkImage.h>
#include <include/core/SkSurface.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>

namespace nativedrawing {

class BackgroundRenderer;
class PathRenderer;
class EraserRenderer;
struct Stroke;
struct EraserCircle;

/**
 * BatchExporter - Handles batch export of multiple pages to PNG
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Manages the export surface and Base64 encoding for multi-page exports.
 */
class BatchExporter {
public:
    BatchExporter(int width, int height);

    /**
     * Export multiple pages to Base64-encoded PNG strings
     *
     * @param pagesData Vector of serialized page data
     * @param backgroundTypes Background type for each page
     * @param pdfBackgrounds PDF background images (if any)
     * @param scale Export scale factor
     * @param deserializeFunc Function to deserialize page data
     * @param renderStrokesFunc Function to render strokes
     * @param backgroundRenderer Background renderer instance
     * @return Vector of Base64-encoded PNG strings
     */
    std::vector<std::string> exportPages(
        const std::vector<std::vector<uint8_t>>& pagesData,
        const std::vector<std::string>& backgroundTypes,
        const std::vector<sk_sp<SkImage>>& pdfBackgrounds,
        float scale,
        std::function<void(const std::vector<uint8_t>&)> deserializeFunc,
        std::function<void(SkCanvas*, const std::string&, sk_sp<SkImage>)> renderFunc
    );

    /**
     * Encode binary data to Base64 string
     */
    static std::string encodeBase64(const void* data, size_t length);

private:
    int width_;
    int height_;
};

} // namespace nativedrawing
