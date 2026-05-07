#include "BackgroundRenderer.h"
#include <include/core/SkPaint.h>
#include <algorithm>
#include <cmath>

namespace nativedrawing {

// Static color definitions
const SkColor BackgroundRenderer::LIGHT_BG_COLOR = SK_ColorWHITE;
const SkColor BackgroundRenderer::LIGHT_LINE_COLOR = SkColorSetRGB(224, 224, 224);      // #E0E0E0
const SkColor BackgroundRenderer::LIGHT_MINOR_GRID_COLOR = SkColorSetRGB(240, 240, 240); // #F0F0F0
const SkColor BackgroundRenderer::LIGHT_MAJOR_GRID_COLOR = SkColorSetRGB(204, 204, 204); // #CCCCCC

namespace {

constexpr float kNotebookAspectRatio = 11.0f / 8.5f;
constexpr int kGraphMajorLineMultiple = 5;

float alignPatternStart(float verticalPatternOffset, float spacing) {
    if (spacing <= 0.0f) {
        return 0.0f;
    }

    float remainder = std::fmod(verticalPatternOffset, spacing);
    if (remainder < 0.0f) {
        remainder += spacing;
    }

    if (std::fabs(remainder) < 0.001f) {
        return 0.0f;
    }

    return spacing - remainder;
}

float resolveSinglePageHeight(float width, float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return std::max(height, 1.0f);
    }

    const float inferredSinglePageHeight = width * kNotebookAspectRatio;
    const float stackedPageCount = std::max(1.0f, std::round(height / inferredSinglePageHeight));
    return height / stackedPageCount;
}

float resolvePatternSpacing(
    float width,
    float height,
    float targetSpacing,
    int requiredLineMultiple = 1
) {
    const float singlePageHeight = resolveSinglePageHeight(width, height);
    if (singlePageHeight <= 0.0f) {
        return targetSpacing;
    }

    const int rawLineCount = std::max(
        1,
        static_cast<int>(std::round(singlePageHeight / targetSpacing))
    );

    int adjustedLineCount = rawLineCount;
    if (requiredLineMultiple > 1) {
        const int roundedMultiple = static_cast<int>(
            std::round(static_cast<float>(rawLineCount) / static_cast<float>(requiredLineMultiple))
        ) * requiredLineMultiple;
        adjustedLineCount = std::max(requiredLineMultiple, roundedMultiple);
    }

    return singlePageHeight / static_cast<float>(std::max(1, adjustedLineCount));
}

} // namespace

void BackgroundRenderer::drawBackground(
    SkCanvas* canvas,
    const std::string& backgroundType,
    int width,
    int height,
    const sk_sp<SkImage>& pdfBackgroundImage,
    float verticalPatternOffset
) {
    float w = static_cast<float>(width);
    float h = static_cast<float>(height);

    // Fill background with white
    canvas->clear(LIGHT_BG_COLOR);

    // PDF background - draw the pre-rendered PDF image
    if (backgroundType == "pdf" && pdfBackgroundImage) {
        drawPdfBackground(canvas, w, pdfBackgroundImage);
        return;
    }

    if (backgroundType == "lined") {
        drawLinedBackground(canvas, w, h, verticalPatternOffset);
    } else if (backgroundType == "grid") {
        drawGridBackground(canvas, w, h, verticalPatternOffset);
    } else if (backgroundType == "dotted") {
        drawDottedBackground(canvas, w, h, verticalPatternOffset);
    } else if (backgroundType == "graph") {
        drawGraphBackground(canvas, w, h, verticalPatternOffset);
    }
    // "plain" or any other type - just white background, already cleared
}

void BackgroundRenderer::drawLinedBackground(
    SkCanvas* canvas,
    float w,
    float h,
    float verticalPatternOffset
) {
    const float lineSpacing = resolvePatternSpacing(w, h, LINE_SPACING);

    SkPaint linePaint;
    linePaint.setColor(LIGHT_LINE_COLOR);
    linePaint.setStyle(SkPaint::kStroke_Style);
    linePaint.setStrokeWidth(1.0f);
    linePaint.setAntiAlias(true);

    const float startY = alignPatternStart(verticalPatternOffset, lineSpacing);
    for (float y = startY; y <= h; y += lineSpacing) {
        canvas->drawLine(0, y, w, y, linePaint);
    }
}

void BackgroundRenderer::drawGridBackground(
    SkCanvas* canvas,
    float w,
    float h,
    float verticalPatternOffset
) {
    const float lineSpacing = resolvePatternSpacing(w, h, LINE_SPACING);

    SkPaint linePaint;
    linePaint.setColor(LIGHT_LINE_COLOR);
    linePaint.setStyle(SkPaint::kStroke_Style);
    linePaint.setStrokeWidth(1.0f);
    linePaint.setAntiAlias(true);

    // Vertical lines
    int vLineCount = static_cast<int>(w / lineSpacing) + 1;
    for (int i = 0; i <= vLineCount; i++) {
        float x = i * lineSpacing;
        canvas->drawLine(x, 0, x, h, linePaint);
    }

    // Horizontal lines
    const float startY = alignPatternStart(verticalPatternOffset, lineSpacing);
    for (float y = startY; y <= h; y += lineSpacing) {
        canvas->drawLine(0, y, w, y, linePaint);
    }
}

void BackgroundRenderer::drawDottedBackground(
    SkCanvas* canvas,
    float w,
    float h,
    float verticalPatternOffset
) {
    const float dotSpacing = resolvePatternSpacing(w, h, DOT_SPACING);

    SkPaint dotPaint;
    dotPaint.setColor(LIGHT_LINE_COLOR);
    dotPaint.setStyle(SkPaint::kFill_Style);
    dotPaint.setAntiAlias(true);

    int hDotCount = static_cast<int>(w / dotSpacing) + 1;
    const float startY = alignPatternStart(verticalPatternOffset, dotSpacing);
    int vDotCount = static_cast<int>(std::ceil((h - startY) / dotSpacing)) + 1;

    for (int i = 0; i <= hDotCount; i++) {
        for (int j = 0; j <= vDotCount; j++) {
            float x = i * dotSpacing;
            float y = startY + j * dotSpacing;
            if (y <= h) {
                canvas->drawCircle(x, y, DOT_RADIUS, dotPaint);
            }
        }
    }
}

void BackgroundRenderer::drawGraphBackground(
    SkCanvas* canvas,
    float w,
    float h,
    float verticalPatternOffset
) {
    const float minorSpacing = resolvePatternSpacing(
        w,
        h,
        MINOR_GRID_SPACING,
        kGraphMajorLineMultiple
    );
    const float majorSpacing = minorSpacing * static_cast<float>(kGraphMajorLineMultiple);

    // Minor grid (lighter, thinner)
    SkPaint minorPaint;
    minorPaint.setColor(LIGHT_MINOR_GRID_COLOR);
    minorPaint.setStyle(SkPaint::kStroke_Style);
    minorPaint.setStrokeWidth(0.5f);
    minorPaint.setAntiAlias(true);

    // Minor vertical lines
    int vMinorCount = static_cast<int>(w / minorSpacing) + 1;
    for (int i = 0; i <= vMinorCount; i++) {
        float x = i * minorSpacing;
        canvas->drawLine(x, 0, x, h, minorPaint);
    }

    // Minor horizontal lines
    const float minorStartY = alignPatternStart(verticalPatternOffset, minorSpacing);
    for (float y = minorStartY; y <= h; y += minorSpacing) {
        canvas->drawLine(0, y, w, y, minorPaint);
    }

    // Major grid (darker, thicker)
    SkPaint majorPaint;
    majorPaint.setColor(LIGHT_MAJOR_GRID_COLOR);
    majorPaint.setStyle(SkPaint::kStroke_Style);
    majorPaint.setStrokeWidth(1.0f);
    majorPaint.setAntiAlias(true);

    // Major vertical lines
    int vMajorCount = static_cast<int>(w / majorSpacing) + 1;
    for (int i = 0; i <= vMajorCount; i++) {
        float x = i * majorSpacing;
        canvas->drawLine(x, 0, x, h, majorPaint);
    }

    // Major horizontal lines
    const float majorStartY = alignPatternStart(verticalPatternOffset, majorSpacing);
    for (float y = majorStartY; y <= h; y += majorSpacing) {
        canvas->drawLine(0, y, w, y, majorPaint);
    }
}

void BackgroundRenderer::drawPdfBackground(SkCanvas* canvas, float w, const sk_sp<SkImage>& image) {
    // The PDF image was already pre-rendered by Swift at the correct size to fit the canvas
    // Just draw it at (0,0) without additional scaling
    // The caller (batchExportPages) already applies the export scale to the canvas
    canvas->drawImage(image, 0, 0);
}

} // namespace nativedrawing
