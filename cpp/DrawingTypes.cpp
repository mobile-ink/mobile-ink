#include "DrawingTypes.h"

#include <cmath>

#include <include/core/SkPathUtils.h>

namespace nativedrawing {

void Stroke::ensureEraserCacheValid() const {
    if (erasedBy.size() == cachedEraserCount) {
        return;
    }

    // Always rebuild from scratch to match live eraser rendering exactly.
    cachedEraserPath.reset();
    cachedEraserCount = 0;

    if (erasedBy.empty()) {
        return;
    }

    // Match EraserRenderer::drawEraserCirclesAsStrokes: group circles into
    // strokes, create a path through centers, then stroke it with round caps.
    constexpr float STROKE_BREAK_FACTOR = 2.0f;
    size_t strokeStart = 0;

    for (size_t i = 0; i <= erasedBy.size(); ++i) {
        bool isLast = (i == erasedBy.size());
        bool breakStroke = isLast;

        if (!isLast && i > strokeStart) {
            float dx = erasedBy[i].x - erasedBy[i - 1].x;
            float dy = erasedBy[i].y - erasedBy[i - 1].y;
            float dist = std::sqrt(dx * dx + dy * dy);
            float avgRadius = (erasedBy[i].radius + erasedBy[i - 1].radius) / 2.0f;
            if (dist > avgRadius * STROKE_BREAK_FACTOR) {
                breakStroke = true;
            }
        }

        if (breakStroke && i > strokeStart) {
            size_t segmentLen = i - strokeStart;

            if (segmentLen == 1) {
                cachedEraserPath.addCircle(
                    erasedBy[strokeStart].x,
                    erasedBy[strokeStart].y,
                    erasedBy[strokeStart].radius
                );
            } else {
                SkPath strokePath;
                strokePath.moveTo(erasedBy[strokeStart].x, erasedBy[strokeStart].y);
                for (size_t j = strokeStart + 1; j < i; ++j) {
                    strokePath.lineTo(erasedBy[j].x, erasedBy[j].y);
                }

                SkPaint strokePaint;
                strokePaint.setStyle(SkPaint::kStroke_Style);
                strokePaint.setStrokeWidth(erasedBy[strokeStart].radius * 2.0f);
                strokePaint.setStrokeCap(SkPaint::kRound_Cap);
                strokePaint.setStrokeJoin(SkPaint::kRound_Join);

                SkPath filledPath;
                if (skpathutils::FillPathWithPaint(strokePath, strokePaint, &filledPath)) {
                    cachedEraserPath.addPath(filledPath);
                } else {
                    for (size_t j = strokeStart; j < i; ++j) {
                        cachedEraserPath.addCircle(
                            erasedBy[j].x,
                            erasedBy[j].y,
                            erasedBy[j].radius
                        );
                    }
                }
            }

            strokeStart = i;
        }
    }

    cachedEraserCount = erasedBy.size();
}

} // namespace nativedrawing
