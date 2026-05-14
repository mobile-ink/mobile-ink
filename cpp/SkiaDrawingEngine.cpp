#include "SkiaDrawingEngine.h"
#include "ShapeRecognition.h"
#include "DrawingHistory.h"
#include "DrawingSelection.h"
#include "BackgroundRenderer.h"
#include "DrawingSerialization.h"
#include "PathRenderer.h"
#include "EraserRenderer.h"
#include "StrokeSplitter.h"
#include "BatchExporter.h"
#include "ActiveStrokeRenderer.h"
#include <algorithm>
#include <include/core/SkImageInfo.h>
#include <cmath>
#include <cstring>

namespace nativedrawing {

namespace {

constexpr float kDefaultCoordinateSmoothing = 0.35f;
constexpr float kDefaultPressureSmoothing = 0.35f;
constexpr float kDefaultAltitudeSmoothing = 0.35f;
// Lower interpolation factors smooth more because the filtered point remains
// closer to the previous sample instead of chasing raw Pencil input.
constexpr float kPenCoordinateSmoothing = 0.30f;
constexpr float kPenPressureSmoothing = 0.22f;
constexpr float kPenAltitudeSmoothing = 0.24f;
constexpr float kDefaultMinDistanceRealtime = 2.0f;
constexpr float kPenMinDistanceRealtime = 1.0f;
constexpr float kMinimumWidthDelta = 0.65f;

bool usesEnhancedPenProfile(const std::string& toolType, bool isPencilInput) {
    return toolType == "pen" && isPencilInput;
}

float interpolateValue(float previous, float next, float factor) {
    return ((1.0f - factor) * previous) + (factor * next);
}

float pointDecimationDistance(const std::string& toolType, bool isPencilInput) {
    return usesEnhancedPenProfile(toolType, isPencilInput)
        ? kPenMinDistanceRealtime
        : kDefaultMinDistanceRealtime;
}

float limitWidthDelta(float previousWidth, float nextWidth, float baseWidth) {
    const float maxDelta = std::max(kMinimumWidthDelta, baseWidth * 0.24f);
    if (nextWidth > previousWidth + maxDelta) {
        return previousWidth + maxDelta;
    }
    if (nextWidth < previousWidth - maxDelta) {
        return previousWidth - maxDelta;
    }
    return nextWidth;
}

}  // namespace

SkiaDrawingEngine::SkiaDrawingEngine(int width, int height)
    : width_(width)
    , height_(height)
    , currentTool_("pen")
    , eraserMode_("pixel")
    , needsStrokeRedraw_(true)
    , needsEraserMaskRedraw_(true)
    , hasLastSmoothedPoint_(false)
    , eraserCursorX_(0)
    , eraserCursorY_(0)
    , eraserCursorRadius_(0)
    , showEraserCursor_(false)
    , cachedEraserCircleCount_(0)
    , bakedCircleCount_(0)
    , maxAffectedStrokeIndex_(0)
    , backgroundType_("plain")
    , selection_(std::make_unique<DrawingSelection>())
    , backgroundRenderer_(std::make_unique<BackgroundRenderer>())
    , serializer_(std::make_unique<DrawingSerialization>())
    , pathRenderer_(std::make_unique<PathRenderer>())
    , eraserRenderer_(std::make_unique<EraserRenderer>())
    , strokeSplitter_(std::make_unique<StrokeSplitter>(pathRenderer_.get()))
    , batchExporter_(std::make_unique<BatchExporter>(width, height))
    , activeStrokeRenderer_(std::make_unique<ActiveStrokeRenderer>(width, height, pathRenderer_.get())) {

    // Initialize paint with high-quality settings
    currentPaint_.setAntiAlias(true);
    currentPaint_.setStyle(SkPaint::kStroke_Style);
    currentPaint_.setStrokeWidth(3.0f);
    currentPaint_.setColor(SK_ColorBLACK);
    currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
    currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
    currentPaint_.setDither(false);

    // Create offscreen surface for strokes only (transparent background)
    SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    strokeSurface_ = SkSurfaces::Raster(info);

    if (strokeSurface_) {
        strokeSurface_->getCanvas()->clear(SK_ColorTRANSPARENT);
        cachedStrokeSnapshot_ = strokeSurface_->makeImageSnapshot();  // Initial empty snapshot
    }

    // Create eraser mask surface (full RGBA for proper DstIn compositing)
    eraserMaskSurface_ = SkSurfaces::Raster(info);
    if (eraserMaskSurface_) {
        eraserMaskSurface_->getCanvas()->clear(SK_ColorWHITE);
    }

    // History starts empty -- canUndo/canRedo are derived from the
    // stacks' emptiness so no sentinel is needed.
}

SkiaDrawingEngine::~SkiaDrawingEngine() = default;

void SkiaDrawingEngine::commitDelta(StrokeDelta&& delta) {
    commitStrokeDelta(undoStack_, redoStack_, std::move(delta), MAX_HISTORY_ENTRIES);
}

void SkiaDrawingEngine::recordPixelEraseCircleAdded(size_t strokeIndex, const EraserCircle& circle) {
    appendPixelEraseCircleToDelta(pendingPixelEraseEntries_, strokeIndex, circle);
}

void SkiaDrawingEngine::applyDelta(const StrokeDelta& delta) {
    applyStrokeDelta(delta, strokes_, eraserCircles_, *pathRenderer_);
}

void SkiaDrawingEngine::revertDelta(const StrokeDelta& delta) {
    revertStrokeDelta(delta, strokes_, eraserCircles_, *pathRenderer_);
}

void SkiaDrawingEngine::touchBegan(
    float x,
    float y,
    float pressure,
    float azimuth,
    float altitude,
    long timestamp,
    bool isPencilInput
) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    currentPoints_.clear();
    currentPath_.reset();
    predictedPointCount_ = 0;  // Reset prediction tracking for new stroke
    clearActiveShapePreview();

    // Reset incremental active stroke state
    activeStrokeRenderer_->reset();

    const bool enhancedPenProfile = usesEnhancedPenProfile(currentTool_, isPencilInput);
    currentStrokeUsesEnhancedPenProfile_ = enhancedPenProfile;
    float baseWidth = currentPaint_.getStrokeWidth();
    float calculatedWidth = pathRenderer_->calculateWidth(
        pressure,
        altitude,
        baseWidth,
        currentTool_,
        enhancedPenProfile
    );

    Point p = {x, y, pressure, azimuth, altitude, calculatedWidth, timestamp};
    currentPoints_.push_back(p);

    // Initialize smoothing with first point
    lastSmoothedPoint_ = p;
    hasLastSmoothedPoint_ = true;

    currentPath_.moveTo(x, y);

    // Start lasso selection if tool is select
    if (currentTool_ == "select") {
        selection_->lassoBegin(x, y);
    }
}

void SkiaDrawingEngine::touchMoved(
    float x,
    float y,
    float pressure,
    float azimuth,
    float altitude,
    long timestamp,
    bool isPencilInput
) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (currentPoints_.empty()) return;

    if (hasActiveShapePreview_) {
        const Point& endpoint = currentPoints_.back();
        const float dxFromEndpoint = x - endpoint.x;
        const float dyFromEndpoint = y - endpoint.y;
        const float preserveRadius = std::max(6.0f, currentPaint_.getStrokeWidth() * 1.25f);
        if (dxFromEndpoint * dxFromEndpoint + dyFromEndpoint * dyFromEndpoint <= preserveRadius * preserveRadius) {
            return;
        }

        if (updateActiveShapePreviewForPoint(x, y)) {
            return;
        }

        clearActiveShapePreview();
        activeStrokeRenderer_->reset();
    }

    const bool enhancedPenProfile = usesEnhancedPenProfile(currentTool_, isPencilInput);
    const float coordinateSmoothingFactor = enhancedPenProfile
        ? kPenCoordinateSmoothing
        : kDefaultCoordinateSmoothing;
    const float pressureSmoothingFactor = enhancedPenProfile
        ? kPenPressureSmoothing
        : kDefaultPressureSmoothing;
    const float altitudeSmoothingFactor = enhancedPenProfile
        ? kPenAltitudeSmoothing
        : kDefaultAltitudeSmoothing;

    float smoothedX;
    float smoothedY;
    float smoothedPressure;
    float smoothedAltitude;

    if (hasLastSmoothedPoint_) {
        smoothedX = interpolateValue(lastSmoothedPoint_.x, x, coordinateSmoothingFactor);
        smoothedY = interpolateValue(lastSmoothedPoint_.y, y, coordinateSmoothingFactor);
        smoothedPressure = interpolateValue(lastSmoothedPoint_.pressure, pressure, pressureSmoothingFactor);
        smoothedAltitude = interpolateValue(lastSmoothedPoint_.altitude, altitude, altitudeSmoothingFactor);
    } else {
        smoothedX = x;
        smoothedY = y;
        smoothedPressure = pressure;
        smoothedAltitude = altitude;
    }

    // Point decimation - skip points too close together
    const float minDistanceRealtime = pointDecimationDistance(currentTool_, isPencilInput);
    if (!currentPoints_.empty()) {
        const Point& last = currentPoints_.back();
        float dx = smoothedX - last.x;
        float dy = smoothedY - last.y;
        float distSq = dx * dx + dy * dy;

        if (distSq < minDistanceRealtime * minDistanceRealtime) {
            float baseWidth = currentPaint_.getStrokeWidth();
            float calculatedWidth = pathRenderer_->calculateWidth(
                smoothedPressure,
                smoothedAltitude,
                baseWidth,
                currentTool_,
                enhancedPenProfile
            );

            if (enhancedPenProfile && hasLastSmoothedPoint_) {
                calculatedWidth = limitWidthDelta(lastSmoothedPoint_.calculatedWidth, calculatedWidth, baseWidth);
            }

            Point p = {
                smoothedX,
                smoothedY,
                smoothedPressure,
                azimuth,
                smoothedAltitude,
                calculatedWidth,
                timestamp
            };
            lastSmoothedPoint_ = p;
            hasLastSmoothedPoint_ = true;
            return;
        }
    }

    float baseWidth = currentPaint_.getStrokeWidth();
    float calculatedWidth = pathRenderer_->calculateWidth(
        smoothedPressure,
        smoothedAltitude,
        baseWidth,
        currentTool_,
        enhancedPenProfile
    );

    if (enhancedPenProfile && hasLastSmoothedPoint_) {
        calculatedWidth = limitWidthDelta(lastSmoothedPoint_.calculatedWidth, calculatedWidth, baseWidth);
    }

    Point p = {
        smoothedX,
        smoothedY,
        smoothedPressure,
        azimuth,
        smoothedAltitude,
        calculatedWidth,
        timestamp
    };
    currentPoints_.push_back(p);

    lastSmoothedPoint_ = p;
    hasLastSmoothedPoint_ = true;

    // Object Eraser Logic: Check for intersections during move
    if (currentTool_ == "eraser" && eraserMode_ == "object") {
        SkRect eraserRect = SkRect::MakeXYWH(x - 10, y - 10, 20, 20);

        for (size_t i = 0; i < strokes_.size(); ++i) {
            if (pendingDeleteIndices_.count(i) > 0) continue;
            if (strokes_[i].isEraser) continue;

            if (strokes_[i].path.getBounds().intersects(eraserRect)) {
                pendingDeleteIndices_.insert(i);
            }
        }
        // OPTIMIZATION: Don't set needsStrokeRedraw_ - pending delete preview rendered directly
    }

    // Pixel Eraser Logic: PencilKit-style immediate stroke splitting
    if (currentTool_ == "eraser" && eraserMode_ == "pixel") {
        float eraserRadius = currentPaint_.getStrokeWidth() / 2.0f;

        // Apply eraser and split strokes immediately - no eraserCircles_ needed
        applyPixelEraserAt(p.x, p.y, eraserRadius);
    }

    // Selection Tool Logic: Build lasso path during drag
    if (currentTool_ == "select") {
        selection_->lassoMove(x, y);
        // Actual selection happens on touchEnded, not during drag
    }
}

void SkiaDrawingEngine::touchEnded(long timestamp) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (currentPoints_.empty()) return;

    if (currentTool_ == "eraser" && eraserMode_ == "object") {
        eraseObjects();
    } else if (currentTool_ == "eraser" && eraserMode_ == "pixel") {
        // Commit a single PixelErase delta covering every (stroke, circle)
        // pair that was added during this drag. pendingPixelEraseEntries_
        // was populated incrementally by recordPixelEraseCircleAdded
        // during applyPixelEraserAt calls. Reset for the next drag.
        if (!pendingPixelEraseEntries_.empty()) {
            StrokeDelta delta;
            delta.kind = StrokeDelta::Kind::PixelErase;
            delta.pixelEraseEntries = std::move(pendingPixelEraseEntries_);
            commitDelta(std::move(delta));
        }
        pendingPixelEraseEntries_.clear();

        // DON'T set needsStrokeRedraw_ - kClear visual is already correct
        // Full redraw only needed on undo/redo/deserialize

        // Reset eraser position tracking for next stroke
        hasLastEraserPoint_ = false;

        currentPoints_.clear();
        currentPath_.reset();
    } else if (currentTool_ == "select") {
        // Finalize lasso selection - select strokes inside/intersecting the lasso
        selection_->lassoEnd(strokes_, selectedIndices_);
        // Pre-cache for smooth drag start
        if (!selectedIndices_.empty()) {
            prepareSelectionDragCache();
        }
        currentPoints_.clear();
        currentPath_.reset();
    } else {
        finishStroke(timestamp);
    }

    hasLastSmoothedPoint_ = false;
    predictedPointCount_ = 0;  // Reset prediction tracking
    currentStrokeUsesEnhancedPenProfile_ = false;
}

bool SkiaDrawingEngine::updateHoldShapePreview(long timestamp) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (currentPoints_.empty() || currentTool_ == "select" || currentTool_ == "eraser") {
        clearActiveShapePreview();
        return false;
    }

    ShapeCandidate candidate = recognizeHeldShape(currentPoints_, currentTool_, timestamp);
    if (!candidate.recognized) {
        return false;
    }

    snapRecognizedShapeCandidateToStrokes(candidate, strokes_, averageCalculatedWidth(currentPoints_));

    activeShapePreviewToolType_ = std::move(candidate.toolType);
    activeShapePreviewPoints_ = std::move(candidate.points);
    activeShapePreviewBasePoints_ = activeShapePreviewPoints_;
    buildRecognizedShapePath(activeShapePreviewToolType_, activeShapePreviewPoints_, activeShapePreviewPath_);
    hasActiveShapePreview_ = true;
    activeShapePreviewAnchorPoint_ = activeShapePreviewToolType_ == "shape-line"
        ? activeShapePreviewPoints_.front()
        : centerPointForPoints(activeShapePreviewPoints_);
    hasActiveShapePreviewAnchor_ = !activeShapePreviewPoints_.empty();
    activeShapePreviewReferenceAngle_ = std::atan2(
        currentPoints_.back().y - activeShapePreviewAnchorPoint_.y,
        currentPoints_.back().x - activeShapePreviewAnchorPoint_.x
    );
    activeShapePreviewReferenceDistance_ = std::max(
        1.0f,
        distanceBetween(activeShapePreviewAnchorPoint_, currentPoints_.back())
    );

    // Once the shape is previewing, the cached freehand active surface should
    // stop contributing pixels; the source stroke points remain intact for
    // final recognition or for continuing freehand if the user moves again.
    activeStrokeRenderer_->reset();

    return true;
}

bool SkiaDrawingEngine::updateActiveShapePreviewForPoint(float x, float y) {
    if (!hasActiveShapePreview_
        || !hasActiveShapePreviewAnchor_
        || activeShapePreviewBasePoints_.empty()) {
        return false;
    }

    std::vector<Point> transformed =
        (activeShapePreviewToolType_ == "shape-line" || activeShapePreviewToolType_ == "shape-circle")
        ? transformedShapePoints(
            activeShapePreviewBasePoints_,
            activeShapePreviewAnchorPoint_,
            activeShapePreviewReferenceAngle_,
            activeShapePreviewReferenceDistance_,
            x,
            y,
            1.0f,
            activeShapePreviewToolType_ == "shape-line" ? 1.0f : 0.0f
        )
        : transformedShapePointsCenterLockedToTarget(
            activeShapePreviewBasePoints_,
            activeShapePreviewAnchorPoint_,
            activeShapePreviewReferenceAngle_,
            activeShapePreviewReferenceDistance_,
            x,
            y
        );

    if (transformed.size() < 2) {
        return false;
    }

    ShapeCandidate candidate;
    candidate.recognized = true;
    candidate.toolType = activeShapePreviewToolType_;
    candidate.points = std::move(transformed);
    snapRecognizedShapeCandidateToStrokes(candidate, strokes_, averageCalculatedWidth(candidate.points));

    SkPath path;
    if (!buildRecognizedShapePath(candidate.toolType, candidate.points, path)) {
        return false;
    }

    activeShapePreviewPoints_ = std::move(candidate.points);
    activeShapePreviewPath_ = path;
    activeStrokeRenderer_->reset();
    return true;
}

// PREDICTIVE TOUCH: Clear predicted points before adding new actual data
// Called at the start of each touchMoved to discard old predictions
void SkiaDrawingEngine::clearPredictedPoints() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (predictedPointCount_ > 0 && currentPoints_.size() >= predictedPointCount_) {
        currentPoints_.erase(
            currentPoints_.end() - predictedPointCount_,
            currentPoints_.end()
        );
        predictedPointCount_ = 0;
    }
}

// PREDICTIVE TOUCH: Add a predicted point from Apple Pencil
// These points are rendered but will be replaced by actual data on next touch event
void SkiaDrawingEngine::addPredictedPoint(
    float x,
    float y,
    float pressure,
    float azimuth,
    float altitude,
    long timestamp,
    bool isPencilInput
) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (currentPoints_.empty()) return;

    const bool enhancedPenProfile = usesEnhancedPenProfile(currentTool_, isPencilInput);
    const float coordinateSmoothingFactor = enhancedPenProfile
        ? kPenCoordinateSmoothing
        : kDefaultCoordinateSmoothing;
    const float pressureSmoothingFactor = enhancedPenProfile
        ? kPenPressureSmoothing
        : kDefaultPressureSmoothing;
    const float altitudeSmoothingFactor = enhancedPenProfile
        ? kPenAltitudeSmoothing
        : kDefaultAltitudeSmoothing;
    const Point& lastPoint = currentPoints_.back();

    float smoothedX = interpolateValue(lastPoint.x, x, coordinateSmoothingFactor);
    float smoothedY = interpolateValue(lastPoint.y, y, coordinateSmoothingFactor);
    float smoothedPressure = interpolateValue(lastPoint.pressure, pressure, pressureSmoothingFactor);
    float smoothedAltitude = interpolateValue(lastPoint.altitude, altitude, altitudeSmoothingFactor);

    float baseWidth = currentPaint_.getStrokeWidth();
    float calculatedWidth = pathRenderer_->calculateWidth(
        smoothedPressure,
        smoothedAltitude,
        baseWidth,
        currentTool_,
        enhancedPenProfile
    );

    if (enhancedPenProfile) {
        calculatedWidth = limitWidthDelta(lastPoint.calculatedWidth, calculatedWidth, baseWidth);
    }

    Point p = {
        smoothedX,
        smoothedY,
        smoothedPressure,
        azimuth,
        smoothedAltitude,
        calculatedWidth,
        timestamp
    };
    currentPoints_.push_back(p);
    predictedPointCount_++;
}

void SkiaDrawingEngine::finishStroke(long endTimestamp) {
    if (currentPoints_.size() < 2) {
        currentPoints_.clear();
        currentPath_.reset();
        clearActiveShapePreview();
        currentStrokeUsesEnhancedPenProfile_ = false;
        return;
    }

    ShapeCandidate shapeCandidate;
    if (hasActiveShapePreview_) {
        shapeCandidate.recognized = true;
        shapeCandidate.toolType = activeShapePreviewToolType_;
        shapeCandidate.points = activeShapePreviewPoints_;
    } else {
        shapeCandidate = recognizeHeldShape(currentPoints_, currentTool_, endTimestamp);
    }

    snapRecognizedShapeCandidateToStrokes(shapeCandidate, strokes_, averageCalculatedWidth(currentPoints_));

    Stroke stroke;
    stroke.points = shapeCandidate.recognized ? shapeCandidate.points : currentPoints_;
    stroke.paint = currentPaint_;
    stroke.isEraser = (currentTool_ == "eraser" && eraserMode_ == "pixel");
    stroke.toolType = shapeCandidate.recognized
        ? shapeCandidate.toolType
        : currentTool_;  // Store tool type for specialized rendering

    // Keep Apple Pencil pen strokes fully opaque so anti-aliased edges stay
    // crisp after the stroke is finalized. Other tools keep their existing
    // opacity behavior.
    if (!stroke.points.empty()) {
        if (currentStrokeUsesEnhancedPenProfile_
            || currentTool_ == "highlighter"
            || currentTool_ == "marker"
            || currentTool_ == "crayon") {
            stroke.originalAlphaMod = 1.0f;
        } else {
            float avgPressure = 0.0f;
            for (const auto& pt : stroke.points) {
                avgPressure += pt.pressure;
            }
            avgPressure /= stroke.points.size();
            stroke.originalAlphaMod = 0.85f + (avgPressure * 0.15f);  // Subtler range: 85%-100%
        }
    }

    if (!buildRecognizedShapePath(stroke.toolType, stroke.points, stroke.path)) {
        smoothPath(stroke.points, stroke.path);
    }

    // Calculate and cache path length for eraser operations
    SkPathMeasure pathMeasure(stroke.path, false);
    stroke.pathLength = pathMeasure.getLength();

    // If this is an eraser stroke, record which strokes it affects
    if (stroke.isEraser) {
        SkRect eraserBounds = stroke.path.getBounds();
        for (size_t i = 0; i < strokes_.size(); ++i) {
            if (strokes_[i].isEraser) continue;

            SkRect strokeBounds = strokes_[i].path.getBounds();
            if (strokeBounds.intersects(eraserBounds)) {
                stroke.affectedStrokeIndices.insert(i);
            }
        }
    }

    strokes_.push_back(stroke);

    // === FAST PATH: Composite active stroke directly ===
    // This preserves O(1) stroke completion for smooth performance.
    if (strokeSurface_ && currentTool_ != "eraser") {
        SkCanvas* strokeCanvas = strokeSurface_->getCanvas();

        if (isRecognizedShapeToolType(stroke.toolType)) {
            SkPaint strokePaint = stroke.paint;
            if (!stroke.isEraser) {
                uint8_t baseAlpha = stroke.paint.getAlpha();
                strokePaint.setAlpha(static_cast<uint8_t>(baseAlpha * stroke.originalAlphaMod));
            }
            renderStrokeGeometry(strokeCanvas, stroke, strokePaint);
        } else {
            // Render any remaining tail points to complete the stroke
            if (currentPoints_.size() > activeStrokeRenderer_->getLastRenderedIndex()) {
                activeStrokeRenderer_->renderFinalTail(currentPoints_, currentPaint_, currentTool_);
            }

            // Composite active stroke onto persistent stroke surface
            sk_sp<SkImage> activeImage = activeStrokeRenderer_->getSnapshot();
            if (activeImage) {
                strokeCanvas->drawImage(activeImage, 0, 0);
            }
        }

        // Update cached snapshot
        cachedStrokeSnapshot_ = strokeSurface_->makeImageSnapshot();

        // Track that we have composited strokes (for consistency check on tool switch)
        maxAffectedStrokeIndex_ = strokes_.size();
    } else {
        // Fallback for eraser or if surfaces unavailable
        needsStrokeRedraw_ = true;
    }

    // Single-stroke append. The new stroke is the last element in
    // strokes_ (just push_back'd above). Capture it for the delta so
    // undo can pop_back and redo can push_back from delta storage.
    {
        StrokeDelta delta;
        delta.kind = StrokeDelta::Kind::AddStrokes;
        delta.addedStrokes.push_back(strokes_.back());
        commitDelta(std::move(delta));
    }

    // Clean up incremental active stroke rendering state
    activeStrokeRenderer_->reset();

    currentPoints_.clear();
    currentPath_.reset();
    clearActiveShapePreview();
    currentStrokeUsesEnhancedPenProfile_ = false;
}

void SkiaDrawingEngine::smoothPath(const std::vector<Point>& points, SkPath& path) {
    pathRenderer_->smoothPath(points, path);
}

void SkiaDrawingEngine::clearActiveShapePreview() {
    hasActiveShapePreview_ = false;
    activeShapePreviewToolType_.clear();
    activeShapePreviewPoints_.clear();
    activeShapePreviewBasePoints_.clear();
    activeShapePreviewPath_.reset();
    hasActiveShapePreviewAnchor_ = false;
    activeShapePreviewReferenceAngle_ = 0.0f;
    activeShapePreviewReferenceDistance_ = 1.0f;
}

void SkiaDrawingEngine::clear() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    // Capture the pre-clear state for the delta BEFORE wiping. Clear is
    // the one operation that genuinely needs an O(N) snapshot to support
    // undo, but it happens once per clear (not per stroke), so the cost
    // is bounded.
    StrokeDelta delta;
    delta.kind = StrokeDelta::Kind::Clear;
    delta.clearedStrokes = strokes_;
    delta.clearedEraserCircles = eraserCircles_;

    strokes_.clear();
    eraserCircles_.clear();
    currentPoints_.clear();
    currentPath_.reset();
    clearActiveShapePreview();
    activeStrokeRenderer_->reset();  // Clear any in-progress incremental rendering
    bakedCircleCount_ = 0;  // No circles to bake

    commitDelta(std::move(delta));

    needsStrokeRedraw_ = true;
    needsEraserMaskRedraw_ = true;
}

void SkiaDrawingEngine::undo() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (undoStack_.empty()) return;
    StrokeDelta delta = std::move(undoStack_.back());
    undoStack_.pop_back();
    revertDelta(delta);
    redoStack_.push_back(std::move(delta));

    cachedEraserCircleCount_ = 0;
    bakedCircleCount_ = 0;
    clearActiveShapePreview();
    activeStrokeRenderer_->reset();
    needsStrokeRedraw_ = true;
    needsEraserMaskRedraw_ = true;
}

void SkiaDrawingEngine::redo() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (redoStack_.empty()) return;
    StrokeDelta delta = std::move(redoStack_.back());
    redoStack_.pop_back();
    applyDelta(delta);
    undoStack_.push_back(std::move(delta));

    cachedEraserCircleCount_ = 0;
    bakedCircleCount_ = 0;
    clearActiveShapePreview();
    activeStrokeRenderer_->reset();
    needsStrokeRedraw_ = true;
    needsEraserMaskRedraw_ = true;
}

void SkiaDrawingEngine::setStrokeColor(SkColor color) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (currentTool_ == "eraser") return;
    uint8_t a = SkColorGetA(currentPaint_.getColor());
    currentPaint_.setColor(SkColorSetARGB(a, SkColorGetR(color), SkColorGetG(color), SkColorGetB(color)));
}

void SkiaDrawingEngine::setStrokeWidth(float width) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    currentPaint_.setStrokeWidth(width);
}

void SkiaDrawingEngine::setToolWithParams(const char* toolType, float width, uint32_t color, const char* eraserMode) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    setTool(toolType);
    currentPaint_.setStrokeWidth(width);

    if (toolType && std::string(toolType) != "eraser") {
        // Extract RGB from color, but ALWAYS preserve tool's alpha (set by setTool)
        uint8_t toolAlpha = SkColorGetA(currentPaint_.getColor());
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        currentPaint_.setColor(SkColorSetARGB(toolAlpha, b, g, r));  // Swap R and B for platform
    }
    eraserMode_ = (eraserMode && std::strlen(eraserMode) > 0) ? eraserMode : "pixel";
}

void SkiaDrawingEngine::setTool(const char* toolType) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    currentTool_ = toolType;
    std::string tool(toolType);

    currentPaint_.setAntiAlias(true);
    currentPaint_.setDither(false);

    if (tool == "pen") {
        currentPaint_.setAlpha(255);
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kSrcOver);
    } else if (tool == "pencil") {
        currentPaint_.setAlpha(200);
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kSrcOver);
    } else if (tool == "marker") {
        currentPaint_.setAlpha(115);
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kSrcOver);
    } else if (tool == "highlighter") {
        currentPaint_.setAlpha(140);  // Higher base alpha for visibility
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kMultiply);
    } else if (tool == "eraser") {
        currentPaint_.setAlpha(255);
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kDstOut);
        currentPaint_.setColor(SK_ColorBLACK);
    } else if (tool == "crayon") {
        // Crayon: Semi-transparent (~85%), waxy texture applied during rendering
        currentPaint_.setAlpha(217);  // ~85% opacity (217/255)
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kSrcOver);
    } else if (tool == "calligraphy") {
        // Calligraphy: Fully opaque, smooth ink, flex nib behavior
        // Width varies based on stroke direction (thin upstrokes, thick downstrokes)
        currentPaint_.setAlpha(255);
        currentPaint_.setStrokeCap(SkPaint::kRound_Cap);
        currentPaint_.setStrokeJoin(SkPaint::kRound_Join);
        currentPaint_.setBlendMode(SkBlendMode::kSrcOver);
    }
}

std::vector<uint8_t> SkiaDrawingEngine::serializeDrawing() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    return serializer_->serialize(strokes_);
}

bool SkiaDrawingEngine::deserializeDrawing(const std::vector<uint8_t>& data) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    std::vector<Stroke> loadedStrokes;
    if (!serializer_->deserialize(data, loadedStrokes)) {
        printf("[C++] Failed to deserialize drawing payload safely\n");
        return false;
    }

    strokes_ = std::move(loadedStrokes);
    eraserCircles_.clear();  // Clear eraser circles when loading
    bakedCircleCount_ = 0;  // No circles to bake
    selectedIndices_.clear();
    isDraggingSelection_ = false;
    hasDragCache_ = false;
    selectionOffsetX_ = 0.0f;
    selectionOffsetY_ = 0.0f;
    dragBackgroundSnapshot_ = nullptr;
    nonSelectedSnapshot_ = nullptr;
    selectedSnapshot_ = nullptr;
    selectionHighlightSnapshot_ = nullptr;
    // Reset history. Loading a serialized notebook is treated as a
    // checkpoint -- the user wouldn't expect to undo past the load.
    undoStack_.clear();
    redoStack_.clear();
    needsStrokeRedraw_ = true;
    needsEraserMaskRedraw_ = true;
    return true;
}

bool SkiaDrawingEngine::canUndo() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    return !undoStack_.empty();
}

bool SkiaDrawingEngine::canRedo() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    return !redoStack_.empty();
}

bool SkiaDrawingEngine::isEmpty() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    return strokes_.empty();
}

} // namespace nativedrawing
