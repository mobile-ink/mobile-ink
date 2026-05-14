#pragma once

#include <vector>
#include <include/core/SkSurface.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPoint.h>
#include <include/core/SkImage.h>
#include "DrawingTypes.h"
#include <string>

namespace nativedrawing {

class PathRenderer;

/**
 * ActiveStrokeRenderer - Handles incremental O(1) rendering of active strokes
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Implements surface caching and incremental rendering to maintain 60-120fps
 * during stroke input, regardless of stroke complexity.
 */
class ActiveStrokeRenderer {
public:
    explicit ActiveStrokeRenderer(int width, int height, PathRenderer* pathRenderer);

    /**
     * Reset state for a new stroke
     */
    void reset();

    /**
     * Render the active stroke incrementally to the output canvas
     *
     * @param canvas Output canvas to draw to
     * @param points Current stroke points
     * @param paint Paint to use for rendering
     * @param toolType Current tool type (pen, crayon, etc.)
     */
    void renderIncremental(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& paint,
        const std::string& toolType
    );

    /**
     * Render remaining tail points to the active stroke surface
     * Called at stroke completion to finalize the stroke
     */
    void renderFinalTail(
        const std::vector<Point>& points,
        const SkPaint& paint,
        const std::string& toolType
    );

    /**
     * Get the cached active stroke image for compositing
     */
    sk_sp<SkImage> getSnapshot() const { return cachedActiveSnapshot_; }

    /**
     * Get the last rendered input index
     */
    size_t getLastRenderedIndex() const { return lastRenderedInputIndex_; }

private:
    PathRenderer* pathRenderer_;

    // Active stroke surface for incremental rendering
    sk_sp<SkSurface> activeStrokeSurface_;
    sk_sp<SkImage> cachedActiveSnapshot_;

    // Incremental rendering state
    size_t lastRenderedInputIndex_;
    std::vector<Point> overlapBuffer_;
    SkPoint lastLeftEdge_, lastRightEdge_;
    bool hasLastEdge_;
    float lastHalfWidth_;  // For calligraphy width continuity

    static constexpr size_t OVERLAP = 2;  // Spline overlap for Catmull-Rom
};

} // namespace nativedrawing
