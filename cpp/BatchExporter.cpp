#include "BatchExporter.h"
#include <include/core/SkImageInfo.h>
#include <include/encode/SkPngEncoder.h>
#include <cstdio>

namespace nativedrawing {

BatchExporter::BatchExporter(int width, int height)
    : width_(width), height_(height) {}

std::string BatchExporter::encodeBase64(const void* data, size_t length) {
    static const char base64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    std::string base64;
    base64.reserve(((length + 2) / 3) * 4);

    for (size_t j = 0; j < length; j += 3) {
        uint32_t triple = bytes[j] << 16;
        if (j + 1 < length) triple |= bytes[j + 1] << 8;
        if (j + 2 < length) triple |= bytes[j + 2];

        base64.push_back(base64Chars[(triple >> 18) & 0x3F]);
        base64.push_back(base64Chars[(triple >> 12) & 0x3F]);
        base64.push_back((j + 1 < length) ? base64Chars[(triple >> 6) & 0x3F] : '=');
        base64.push_back((j + 2 < length) ? base64Chars[triple & 0x3F] : '=');
    }

    return base64;
}

std::vector<std::string> BatchExporter::exportPages(
    const std::vector<std::vector<uint8_t>>& pagesData,
    const std::vector<std::string>& backgroundTypes,
    const std::vector<sk_sp<SkImage>>& pdfBackgrounds,
    float scale,
    std::function<void(const std::vector<uint8_t>&)> deserializeFunc,
    std::function<void(SkCanvas*, const std::string&, sk_sp<SkImage>)> renderFunc
) {
    std::vector<std::string> results;
    results.reserve(pagesData.size());

    // Calculate scaled dimensions
    int scaledWidth = static_cast<int>(width_ * scale);
    int scaledHeight = static_cast<int>(height_ * scale);

    // Create single reusable surface at scaled dimensions
    SkImageInfo info = SkImageInfo::MakeN32Premul(scaledWidth, scaledHeight);
    sk_sp<SkSurface> exportSurface = SkSurfaces::Raster(info);

    if (!exportSurface) {
        printf("[C++] BatchExporter: Failed to create export surface\n");
        return results;
    }

    for (size_t i = 0; i < pagesData.size(); ++i) {
        SkCanvas* canvas = exportSurface->getCanvas();

        // Clear and set scale transform
        canvas->clear(SK_ColorTRANSPARENT);
        canvas->save();
        canvas->scale(scale, scale);

        // Get background type for this page
        std::string bgType = "plain";
        if (i < backgroundTypes.size() && !backgroundTypes[i].empty()) {
            bgType = backgroundTypes[i];
        }

        // Get PDF background for this page (if provided)
        sk_sp<SkImage> pdfBg = nullptr;
        if (i < pdfBackgrounds.size() && pdfBackgrounds[i]) {
            pdfBg = pdfBackgrounds[i];
        }

        // Deserialize page data (or use empty strokes)
        if (!pagesData[i].empty()) {
            deserializeFunc(pagesData[i]);
        } else {
            deserializeFunc({});  // Clear strokes
        }

        // Render using the provided render function
        renderFunc(canvas, bgType, pdfBg);

        canvas->restore();

        // Encode to PNG
        sk_sp<SkImage> snapshot = exportSurface->makeImageSnapshot();
        if (snapshot) {
            sk_sp<SkData> pngData = SkPngEncoder::Encode(nullptr, snapshot.get(), {});
            if (pngData) {
                std::string base64 = encodeBase64(pngData->data(), pngData->size());
                results.push_back("data:image/png;base64," + base64);
            } else {
                results.push_back("");
            }
        } else {
            results.push_back("");
        }
    }

    printf("[C++] BatchExporter: Exported %zu pages\n", results.size());
    return results;
}

} // namespace nativedrawing
