#pragma once

#include <vector>
#include <unordered_set>
#include <limits>
#include <functional>
#include "SkiaDrawingEngine.h"

namespace nativedrawing {

/**
 * DrawingSelection - Handles stroke selection operations
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Provides all selection-related functionality including:
 * - Single stroke selection at point
 * - Multi-stroke selection during drag
 * - Selection movement and copying
 * - Selection bounds calculation
 */
class DrawingSelection {
public:
    DrawingSelection() = default;

    // Callback type used by mutating ops to commit history deltas.
    // Engine provides a closure that pushes onto its undoStack_.
    using DeltaCommitter = std::function<void(StrokeDelta&&)>;

    // Selection operations
    bool selectStrokeAt(
        float x, float y,
        std::vector<Stroke>& strokes,
        std::unordered_set<size_t>& selectedIndices
    );

    bool selectShapeStrokeAt(
        float x, float y,
        std::vector<Stroke>& strokes,
        std::unordered_set<size_t>& selectedIndices
    );

    void clearSelection(std::unordered_set<size_t>& selectedIndices);

    void deleteSelection(
        std::vector<Stroke>& strokes,
        std::unordered_set<size_t>& selectedIndices,
        const DeltaCommitter& commit
    );

    void copySelection(
        const std::vector<Stroke>& strokes,
        const std::unordered_set<size_t>& selectedIndices,
        std::vector<Stroke>& copiedStrokes
    );

    void pasteSelection(
        std::vector<Stroke>& strokes,
        const std::vector<Stroke>& copiedStrokes,
        float offsetX, float offsetY,
        const DeltaCommitter& commit
    );

    void moveSelection(
        std::vector<Stroke>& strokes,
        const std::unordered_set<size_t>& selectedIndices,
        float dx, float dy
    );

    void finalizeMove(
        const std::vector<Stroke>& strokes,
        const std::unordered_set<size_t>& selectedIndices,
        float totalDx, float totalDy,
        const DeltaCommitter& commit
    );

    int getSelectionCount(const std::unordered_set<size_t>& selectedIndices) const;

    std::vector<float> getSelectionBounds(
        const std::vector<Stroke>& strokes,
        const std::unordered_set<size_t>& selectedIndices
    );

    // Render selection highlight on canvas
    void renderSelection(
        SkCanvas* canvas,
        const std::vector<Stroke>& strokes,
        const std::unordered_set<size_t>& selectedIndices
    );

    // Utility functions
    bool isPointNearStroke(
        float x, float y,
        const Stroke& stroke,
        float tolerance = 20.0f
    );

    SkRect calculateStrokeBounds(const std::vector<Point>& points);

    // Lasso selection methods
    void lassoBegin(float x, float y);
    void lassoMove(float x, float y);
    void lassoEnd(
        std::vector<Stroke>& strokes,
        std::unordered_set<size_t>& selectedIndices
    );
    void renderLasso(SkCanvas* canvas);
    bool isLassoActive() const { return lassoActive_; }
    void cancelLasso();

private:
    bool selectStrokeAtMatching(
        float x, float y,
        std::vector<Stroke>& strokes,
        std::unordered_set<size_t>& selectedIndices,
        bool shapeStrokesOnly
    );

    // Helper to smooth path (shared with main engine)
    void smoothPath(const std::vector<Point>& points, SkPath& path);

    // Lasso state
    bool lassoActive_ = false;
    SkPath lassoPath_;
    std::vector<Point> lassoPoints_;
    float lassoPhase_ = 0.0f;  // For marching ants animation
};

} // namespace nativedrawing
