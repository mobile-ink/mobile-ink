#pragma once

#include <string>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkColor.h>

namespace nativedrawing {

/**
 * BackgroundRenderer - Handles canvas background rendering
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Provides background pattern rendering including:
 * - Plain white background
 * - Lined paper (horizontal lines)
 * - Grid paper (horizontal + vertical lines)
 * - Dotted paper (dot pattern)
 * - Graph paper (minor + major grid)
 * - PDF background (pre-rendered image)
 */
class BackgroundRenderer {
public:
    BackgroundRenderer() = default;

    // Draw background pattern based on type
    void drawBackground(
        SkCanvas* canvas,
        const std::string& backgroundType,
        int width,
        int height,
        const sk_sp<SkImage>& pdfBackgroundImage = nullptr,
        float verticalPatternOffset = 0.0f
    );

private:
    // Background pattern constants - large, usable spacing
    static constexpr float LINE_SPACING = 60.0f;
    static constexpr float DOT_SPACING = 60.0f;
    static constexpr float DOT_RADIUS = 4.0f;
    static constexpr float MINOR_GRID_SPACING = 60.0f;
    static constexpr float MAJOR_GRID_SPACING = 300.0f;

    // Colors for light mode
    static const SkColor LIGHT_BG_COLOR;
    static const SkColor LIGHT_LINE_COLOR;
    static const SkColor LIGHT_MINOR_GRID_COLOR;
    static const SkColor LIGHT_MAJOR_GRID_COLOR;

    // Individual pattern renderers
    void drawLinedBackground(SkCanvas* canvas, float w, float h, float verticalPatternOffset);
    void drawGridBackground(SkCanvas* canvas, float w, float h, float verticalPatternOffset);
    void drawDottedBackground(SkCanvas* canvas, float w, float h, float verticalPatternOffset);
    void drawGraphBackground(SkCanvas* canvas, float w, float h, float verticalPatternOffset);
    void drawPdfBackground(SkCanvas* canvas, float w, const sk_sp<SkImage>& image);
};

} // namespace nativedrawing
