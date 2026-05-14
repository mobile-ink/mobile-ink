#pragma once

#include <vector>
#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>
#include <include/core/SkPaint.h>
#include "DrawingTypes.h"

namespace nativedrawing {

/**
 * EraserRenderer - Handles pixel eraser rendering as smooth strokes
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Renders eraser circles as connected stroke paths instead of individual
 * circles, creating smooth eraser strokes that look natural.
 */
class EraserRenderer {
public:
    EraserRenderer() = default;

    /**
     * Draw eraser circles as smooth stroke paths
     * Groups consecutive circles into strokes and draws with round caps
     *
     * @param canvas The canvas to draw on
     * @param circles Vector of eraser circles to render
     * @param startIdx Starting index in circles vector
     * @param endIdx Ending index (exclusive) in circles vector
     */
    void drawEraserCirclesAsStrokes(
        SkCanvas* canvas,
        const std::vector<EraserCircle>& circles,
        size_t startIdx,
        size_t endIdx
    );

    /**
     * Draw eraser circles for a specific stroke index range
     * Only draws circles whose maxAffectedStrokeIndex <= targetStrokeIndex
     * Used for interleaved rendering in redrawStrokes()
     *
     * @param canvas The canvas to draw on
     * @param circles Vector of eraser circles
     * @param startIdx Starting index to check
     * @param targetStrokeIndex Only draw circles affecting strokes up to this index
     * @return Next index to process (first circle not drawn)
     */
    size_t drawEraserCirclesUpToStroke(
        SkCanvas* canvas,
        const std::vector<EraserCircle>& circles,
        size_t startIdx,
        size_t targetStrokeIndex
    );

private:
    // Maximum distance between circles before starting a new stroke segment
    // (2x radius means circles don't overlap = new stroke)
    static constexpr float STROKE_BREAK_FACTOR = 2.0f;
};

} // namespace nativedrawing
