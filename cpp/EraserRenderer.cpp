#include "EraserRenderer.h"
#include <cmath>

namespace nativedrawing {

void EraserRenderer::drawEraserCirclesAsStrokes(
    SkCanvas* canvas,
    const std::vector<EraserCircle>& circles,
    size_t startIdx,
    size_t endIdx
) {
    if (startIdx >= endIdx || startIdx >= circles.size()) return;
    endIdx = std::min(endIdx, circles.size());

    // Group circles into continuous strokes by checking distance
    size_t strokeStart = startIdx;
    float lastRadius = circles[startIdx].radius;

    for (size_t i = startIdx; i <= endIdx; ++i) {
        bool isLast = (i == endIdx);
        bool breakStroke = isLast;

        if (!isLast && i > strokeStart) {
            // Check if this circle is far from the previous (new stroke)
            float dx = circles[i].x - circles[i-1].x;
            float dy = circles[i].y - circles[i-1].y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float avgRadius = (circles[i].radius + circles[i-1].radius) / 2.0f;
            // If distance is more than 2x radius, it's a new stroke
            if (dist > avgRadius * STROKE_BREAK_FACTOR) {
                breakStroke = true;
            }
        }

        if (breakStroke && i > strokeStart) {
            // Draw this eraser stroke segment as a path
            SkPath eraserPath;
            eraserPath.moveTo(circles[strokeStart].x, circles[strokeStart].y);
            for (size_t j = strokeStart + 1; j < i; ++j) {
                eraserPath.lineTo(circles[j].x, circles[j].y);
            }

            SkPaint erasePaint;
            erasePaint.setBlendMode(SkBlendMode::kClear);
            erasePaint.setAntiAlias(true);
            erasePaint.setStyle(SkPaint::kStroke_Style);
            erasePaint.setStrokeWidth(lastRadius * 2.0f);  // Diameter
            erasePaint.setStrokeCap(SkPaint::kRound_Cap);
            erasePaint.setStrokeJoin(SkPaint::kRound_Join);
            canvas->drawPath(eraserPath, erasePaint);

            strokeStart = i;
        }

        if (!isLast) {
            lastRadius = circles[i].radius;
        }
    }
}

size_t EraserRenderer::drawEraserCirclesUpToStroke(
    SkCanvas* canvas,
    const std::vector<EraserCircle>& circles,
    size_t startIdx,
    size_t targetStrokeIndex
) {
    // NOTE: This function is deprecated - eraser circles are now stored per-stroke
    // in stroke.erasedBy and applied via clipPath during rendering.
    // Keeping for backwards compatibility but it just draws all remaining circles.
    if (startIdx >= circles.size()) return startIdx;

    size_t endIdx = circles .size();
    if (endIdx > startIdx) {
        drawEraserCirclesAsStrokes(canvas, circles, startIdx, endIdx);
    }

    return endIdx;
}

} // namespace nativedrawing
