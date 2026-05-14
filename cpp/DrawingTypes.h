#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathUtils.h>

namespace nativedrawing {

struct Point {
    float x;
    float y;
    float pressure;
    float azimuthAngle;  // Pen tilt angle in radians
    float altitude;      // Pen altitude angle (perpendicular = pi/2)
    float calculatedWidth;  // Width calculated from pressure + altitude
    long timestamp;
};

// Pixel eraser circle - stored per-stroke for rendering-time clipping
struct EraserCircle {
    float x;
    float y;
    float radius;
};

struct Stroke {
    std::vector<Point> points;
    SkPaint paint;
    SkPath path;
    bool isEraser = false; // Mark eraser strokes to prevent selection
    std::unordered_set<size_t> affectedStrokeIndices; // For eraser strokes: which strokes they erase
    float originalAlphaMod = 1.0f; // Preserve original opacity modifier for consistent appearance
    std::string toolType = "pen"; // Tool type for specialized rendering (e.g., crayon needs texture)
    float pathLength = 0.0f;  // Cached total arc-length of the path

    // Per-stroke eraser data: circles that have erased portions of this stroke
    // These move WITH the stroke when selected/moved, ensuring pixel-perfect appearance
    // Empty = no erasure, rendering uses full stroke path
    std::vector<EraserCircle> erasedBy;

    // OPTIMIZATION: Cached eraser path for O(1) rendering instead of O(n) path ops per frame
    mutable SkPath cachedEraserPath;      // Union of all erasedBy circles
    mutable size_t cachedEraserCount = 0; // Track when to rebuild cache

    // OPTIMIZATION: Cached visibility for O(1) selection instead of O(m*k) per frame
    mutable bool cachedHasVisiblePoints = true;  // True if any point is outside all eraser circles

    // Ensure cachedEraserPath is up-to-date with erasedBy circles
    // Builds stroked path matching EraserRenderer::drawEraserCirclesAsStrokes
    void ensureEraserCacheValid() const {
        if (erasedBy.size() != cachedEraserCount) {
            // Always rebuild from scratch to match live eraser rendering exactly
            cachedEraserPath.reset();
            cachedEraserCount = 0;

            if (erasedBy.empty()) return;

            // Match EraserRenderer::drawEraserCirclesAsStrokes approach:
            // Group circles into strokes, create path through centers, stroke with round caps
            constexpr float STROKE_BREAK_FACTOR = 2.0f;
            size_t strokeStart = 0;

            for (size_t i = 0; i <= erasedBy.size(); ++i) {
                bool isLast = (i == erasedBy.size());
                bool breakStroke = isLast;

                if (!isLast && i > strokeStart) {
                    float dx = erasedBy[i].x - erasedBy[i-1].x;
                    float dy = erasedBy[i].y - erasedBy[i-1].y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    float avgRadius = (erasedBy[i].radius + erasedBy[i-1].radius) / 2.0f;
                    if (dist > avgRadius * STROKE_BREAK_FACTOR) {
                        breakStroke = true;
                    }
                }

                if (breakStroke && i > strokeStart) {
                    size_t segmentLen = i - strokeStart;

                    if (segmentLen == 1) {
                        // Single point - just add a circle
                        cachedEraserPath.addCircle(erasedBy[strokeStart].x,
                                                    erasedBy[strokeStart].y,
                                                    erasedBy[strokeStart].radius);
                    } else {
                        // Build path through circle centers
                        SkPath strokePath;
                        strokePath.moveTo(erasedBy[strokeStart].x, erasedBy[strokeStart].y);
                        for (size_t j = strokeStart + 1; j < i; ++j) {
                            strokePath.lineTo(erasedBy[j].x, erasedBy[j].y);
                        }

                        // Convert stroked path to filled path (matches live eraser exactly)
                        SkPaint strokePaint;
                        strokePaint.setStyle(SkPaint::kStroke_Style);
                        strokePaint.setStrokeWidth(erasedBy[strokeStart].radius * 2.0f);
                        strokePaint.setStrokeCap(SkPaint::kRound_Cap);
                        strokePaint.setStrokeJoin(SkPaint::kRound_Join);

                        SkPath filledPath;
                        if (skpathutils::FillPathWithPaint(strokePath, strokePaint, &filledPath)) {
                            cachedEraserPath.addPath(filledPath);
                        } else {
                            // Fallback: add circles for each point
                            for (size_t j = strokeStart; j < i; ++j) {
                                cachedEraserPath.addCircle(erasedBy[j].x,
                                                            erasedBy[j].y,
                                                            erasedBy[j].radius);
                            }
                        }
                    }

                    strokeStart = i;
                }
            }

            cachedEraserCount = erasedBy.size();
        }
    }
};

// Delta-based history. Each entry describes ONE operation that was
// applied to strokes_, sized in proportion to the operation -- a single
// stroke add is ~1-3 KB, a pixel erase pass over K strokes is ~K * 12
// bytes per circle, etc. This replaces the previous full-snapshot
// approach where each history entry contained the entire strokes_
// vector deep-copy: with that model, total history memory grew linearly
// with stroke count even though the entry COUNT was capped, because
// each new snapshot was bigger than the last. On a long drawing
// session this was the dominant cause of the user-reported steady RAM
// climb past 2 GB (47 million live small allocations from duplicated
// stroke point/path vectors across history snapshots).
//
// Undo applies the inverse of the operation in-place; redo applies it
// forward. strokes_ is the canonical state between operations; the
// history is just a sequence of "what happened."
struct StrokeDelta {
    enum class Kind : uint8_t {
        AddStrokes,    // appended at end of strokes_ (touchEnded normal stroke, paste)
        RemoveStrokes, // removed at indices (object eraser, delete selection)
        PixelErase,    // erasedBy circles appended to one or more strokes (pixel eraser)
        MoveStrokes,   // strokes translated by (dx, dy) (selection finalize-move)
        ReplaceStrokes, // selected strokes transformed in place (resize handles)
        Clear,         // all strokes wiped (clear button, full-screen erase)
    };
    Kind kind;

    // For AddStrokes: the strokes that were appended (1 entry for normal
    // pen stroke, N entries for paste). Undo = pop_back N, redo = push_back from delta.
    std::vector<Stroke> addedStrokes;

    // For RemoveStrokes: pairs of (originalIndex, stroke) in ASCENDING
    // index order. Undo re-inserts in ascending order (later inserts see
    // valid indices because earlier ones already shifted things into
    // place). Redo erases in DESCENDING order so each erase doesn't
    // invalidate higher indices.
    std::vector<std::pair<size_t, Stroke>> removedStrokes;

    // For PixelErase: one entry per stroke that received eraser circles
    // during this op. addedCircles holds the actual circle data so redo
    // can re-append them; undo pops addedCircles.size() entries from the
    // affected stroke's erasedBy.
    struct PixelEraseEntry {
        size_t strokeIndex;
        std::vector<EraserCircle> addedCircles;
    };
    std::vector<PixelEraseEntry> pixelEraseEntries;

    // For MoveStrokes: indices of moved strokes + total translation.
    // Undo = translate by (-dx, -dy); redo = (dx, dy). We re-run path
    // smoothing after the translate either way, so the cached SkPath
    // matches.
    std::vector<size_t> moveIndices;
    float moveDx = 0.0f;
    float moveDy = 0.0f;

    // For ReplaceStrokes: exact before/after snapshots for transformed
    // selected strokes. Used by native selection handles where the edit is
    // a scale/reshape, not a pure translation.
    std::vector<std::pair<size_t, Stroke>> beforeStrokes;
    std::vector<std::pair<size_t, Stroke>> afterStrokes;

    // For Clear: the strokes (and any global eraser circles) that
    // existed before the clear, so undo can restore them. Bounded to
    // exactly one snapshot per clear op, not per stroke.
    std::vector<Stroke> clearedStrokes;
    std::vector<EraserCircle> clearedEraserCircles;
};

} // namespace nativedrawing
