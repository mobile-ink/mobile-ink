#pragma once

#include <vector>
#include <memory>
#include <string>
#include <cmath>
#include <algorithm>

// Use angle brackets to force system search paths (not CocoaPods symlinks)
#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkPath.h>
#include <include/core/SkPaint.h>
#include <include/core/SkColor.h>
#include <include/core/SkImage.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPathUtils.h>
#include <SkPathOps.h>
#include <unordered_set>
#include <unordered_map>
#include <mutex>

namespace nativedrawing {

// Forward declarations for helper modules
class DrawingSelection;
class BackgroundRenderer;
class DrawingSerialization;
class PathRenderer;
class EraserRenderer;
class StrokeSplitter;
class BatchExporter;
class ActiveStrokeRenderer;

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

class SkiaDrawingEngine {
public:
    SkiaDrawingEngine(int width, int height);
    ~SkiaDrawingEngine();

    // Drawing operations (called from touch events)
    void touchBegan(float x, float y, float pressure, float azimuth = 0.0f, float altitude = 1.57f, long timestamp = 0, bool isPencilInput = false);
    void touchMoved(float x, float y, float pressure, float azimuth = 0.0f, float altitude = 1.57f, long timestamp = 0, bool isPencilInput = false);
    void touchEnded(long timestamp = 0);
    bool updateHoldShapePreview(long timestamp);

    // Predictive touch support (Apple Pencil low-latency rendering)
    void clearPredictedPoints();  // Remove predicted points before adding new actual data
    void addPredictedPoint(float x, float y, float pressure, float azimuth, float altitude, long timestamp, bool isPencilInput = false);

    // Canvas operations
    void clear();
    void undo();
    void redo();
    void eraseObjects();

    // Tool settings
    void setStrokeColor(SkColor color);
    void setStrokeWidth(float width);
    void setTool(const char* toolType);
    void setToolWithParams(const char* toolType, float width, uint32_t color, const char* eraserMode);

    // Background settings
    void setBackgroundType(const char* backgroundType);
    std::string getBackgroundType() const;
    void setPdfBackgroundImage(sk_sp<SkImage> image);

    // Selection operations
    bool selectStrokeAt(float x, float y); // Returns true if stroke was selected
    bool selectShapeStrokeAt(float x, float y); // Finger tap selects only recognized shapes
    void clearSelection();
    void deleteSelection();
    void copySelection();
    void pasteSelection(float offsetX, float offsetY);
    void moveSelection(float dx, float dy);
    void finalizeMove(); // Call when move operation completes to update history
    void beginSelectionTransform(int handleIndex);
    void updateSelectionTransform(float x, float y);
    void finalizeSelectionTransform();
    void cancelSelectionTransform();
    void prepareSelectionDragCache();  // OPTIMIZATION: Pre-cache for fast drag (call when selection changes)
    void beginSelectionDrag();         // Start drag mode (uses pre-cached snapshots)
    void endSelectionDrag();           // Clear drag state
    int getSelectionCount() const;
    std::vector<float> getSelectionBounds(); // Returns [minX, minY, maxX, maxY]

    // Rendering
    void render(SkCanvas* canvas);
    sk_sp<SkImage> makeSnapshot();

    // Batch export: Render multiple pages to PNG without creating new engine each time
    // Returns vector of base64-encoded PNG strings
    std::vector<std::string> batchExportPages(
        const std::vector<std::vector<uint8_t>>& pagesData,
        const std::vector<std::string>& backgroundTypes,
        const std::vector<sk_sp<SkImage>>& pdfBackgrounds,
        const std::vector<int>& pageIndices,
        float scale
    );

    // Eraser cursor (for pixel eraser visualization)
    void setEraserCursor(float x, float y, float radius, bool visible);

    // Serialization
    std::vector<uint8_t> serializeDrawing();
    bool deserializeDrawing(const std::vector<uint8_t>& data);

    // State queries
    bool canUndo() const;
    bool canRedo() const;
    bool isEmpty() const;

private:
    // Cap on undo history depth. 20 entries is generous (real users rarely
    // chain past 5 undos). With delta-based history each entry is
    // O(opSize), not O(stateSize), so the total memory cost stays
    // bounded as the stroke count grows -- the memory pathology that
    // forced the previous reduce-the-cap workaround is gone.
    static constexpr size_t MAX_HISTORY_ENTRIES = 20;

    // Delta-history state. Each commit pushes onto undoStack_ and clears
    // redoStack_; undo pops from undoStack_ + reverts + pushes onto
    // redoStack_; redo does the inverse. Both are capped at
    // MAX_HISTORY_ENTRIES (oldest dropped from the bottom of undoStack_).
    std::vector<StrokeDelta> undoStack_;
    std::vector<StrokeDelta> redoStack_;
    void commitDelta(StrokeDelta&& delta);
    void applyDelta(const StrokeDelta& delta);   // forward (used by redo)
    void revertDelta(const StrokeDelta& delta);  // backward (used by undo)

    // Pixel-eraser accumulator. During an eraser drag (touchBegan eraser
    // -> touchMoved... -> touchEnded), each applyPixelEraserAt call
    // appends circles to multiple strokes' erasedBy vectors. We
    // accumulate the (strokeIndex, circles) pairs here so touchEnded can
    // emit one PixelErase delta covering the whole drag. Reset at the
    // start of each eraser touch.
    std::vector<StrokeDelta::PixelEraseEntry> pendingPixelEraseEntries_;
    void recordPixelEraseCircleAdded(size_t strokeIndex, const EraserCircle& circle);

    int width_;
    int height_;

    // Current stroke being drawn
    std::vector<Point> currentPoints_;
    SkPath currentPath_;

    // Input smoothing for reducing jitter
    Point lastSmoothedPoint_;
    bool hasLastSmoothedPoint_;
    bool currentStrokeUsesEnhancedPenProfile_ = false;

    // Predictive touch: tracks how many trailing points are predictions (not actual input)
    // These are cleared at the start of each touchMoved and replaced with new predictions
    size_t predictedPointCount_ = 0;

    // Completed strokes
    std::vector<Stroke> strokes_;

    // Pixel eraser circles - applied during rendering to punch holes
    std::vector<EraserCircle> eraserCircles_;

    // (History is now delta-based: undoStack_/redoStack_ declared above
    //  alongside the apply/revert helpers.)

    // Current tool settings
    SkPaint currentPaint_;
    std::string currentTool_;
    std::string eraserMode_;
    std::unordered_set<size_t> pendingDeleteIndices_;

    // Selection state
    std::unordered_set<size_t> selectedIndices_;
    std::vector<Stroke> copiedStrokes_;

    // Selection drag optimization - transform instead of rebuild
    bool isDraggingSelection_ = false;
    bool hasDragCache_ = false;                 // True if snapshots are pre-cached
    float selectionOffsetX_ = 0.0f;
    float selectionOffsetY_ = 0.0f;
    bool isTransformingSelection_ = false;
    bool selectionTransformHasDelta_ = false;
    int selectionTransformHandleIndex_ = -1;
    std::unordered_map<size_t, Stroke> selectionTransformOriginalStrokes_;
    sk_sp<SkImage> dragBackgroundSnapshot_;     // Cache background for O(1) drag
    sk_sp<SkImage> nonSelectedSnapshot_;
    sk_sp<SkImage> selectedSnapshot_;           // Cache selected strokes for O(1) drag
    sk_sp<SkImage> selectionHighlightSnapshot_; // Cache selection highlight too

    // Offscreen surface for rendering strokes only (transparent background)
    // Background patterns are drawn directly to output canvas, not this surface
    sk_sp<SkSurface> strokeSurface_;
    sk_sp<SkImage> cachedStrokeSnapshot_;  // Cached snapshot for fast rendering
    bool needsStrokeRedraw_;

    // Eraser mask surface for dual-surface architecture
    // White = keep stroke, Black = erase stroke
    // This allows eraser updates without re-rendering all strokes
    sk_sp<SkSurface> eraserMaskSurface_;
    bool needsEraserMaskRedraw_;

    // Eraser cursor state
    float eraserCursorX_;
    float eraserCursorY_;
    float eraserCursorRadius_;
    bool showEraserCursor_;

    // Track last eraser position for smooth stroke drawing (not just circles)
    float lastEraserX_ = 0;
    float lastEraserY_ = 0;
    float lastEraserRadius_ = 0;
    bool hasLastEraserPoint_ = false;

    // OPTIMIZATION: Cached eraser path to avoid rebuilding every frame
    SkPath cachedEraserPath_;
    size_t cachedEraserCircleCount_;

    // Track how many circles are already baked into strokeSurface_
    // During render: only apply kClear for circles beyond this count (new circles)
    // During redraw: apply ALL circles, then update this count
    size_t bakedCircleCount_;

    // Dual-surface: tracks which strokes are affected by eraser
    // Strokes with index < maxAffectedStrokeIndex_ can be erased
    // Strokes with index >= maxAffectedStrokeIndex_ are drawn on top (overlay)
    size_t maxAffectedStrokeIndex_;

    // Background type (plain, lined, grid, dotted, graph, pdf)
    std::string backgroundType_;
    float backgroundOriginY_ = 0.0f;

    // PDF background image (set from platform-specific code)
    sk_sp<SkImage> pdfBackgroundImage_;

    // Helper modules (extracted for maintainability)
    std::unique_ptr<DrawingSelection> selection_;
    std::unique_ptr<BackgroundRenderer> backgroundRenderer_;
    std::unique_ptr<DrawingSerialization> serializer_;
    std::unique_ptr<PathRenderer> pathRenderer_;
    std::unique_ptr<EraserRenderer> eraserRenderer_;
    std::unique_ptr<StrokeSplitter> strokeSplitter_;
    std::unique_ptr<BatchExporter> batchExporter_;
    std::unique_ptr<ActiveStrokeRenderer> activeStrokeRenderer_;

    bool hasActiveShapePreview_ = false;
    std::string activeShapePreviewToolType_;
    std::vector<Point> activeShapePreviewPoints_;
    std::vector<Point> activeShapePreviewBasePoints_;
    SkPath activeShapePreviewPath_;
    Point activeShapePreviewAnchorPoint_{0.0f, 0.0f, 1.0f, 0.0f, 1.57079632679f, 1.0f, 0};
    bool hasActiveShapePreviewAnchor_ = false;
    float activeShapePreviewReferenceAngle_ = 0.0f;
    float activeShapePreviewReferenceDistance_ = 1.0f;

    // Helper methods
    void smoothPath(const std::vector<Point>& points, SkPath& path);
    void finishStroke(long endTimestamp = 0);
    void renderStrokeGeometry(SkCanvas* canvas, const Stroke& stroke, const SkPaint& paint);
    void clearActiveShapePreview();
    bool updateActiveShapePreviewForPoint(float x, float y);
    void redrawStrokes();
    void redrawEraserMask();  // Dual-surface: only redraws eraser circles to mask

    // Pixel eraser stroke splitting - splits a stroke by removing portions that intersect the eraser
    std::vector<Stroke> splitStrokeAtPoint(const Stroke& originalStroke, const Point& eraserPoint, float eraserRadius);

    // Bake eraser effect: apply eraserCircles_ to strokes_ permanently via splitStrokeAtPoint
    // Called at touchEnded for pixel eraser - makes eraserCircles_ empty for O(1) rendering
    void bakeEraserCircles();

    // PencilKit-style pixel eraser: immediately splits strokes at eraser point
    // Returns true if any strokes were modified
    bool applyPixelEraserAt(float x, float y, float radius);

    mutable std::recursive_mutex stateMutex_;
};

} // namespace nativedrawing
