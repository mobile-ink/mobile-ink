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
#include <include/core/SkBitmap.h>
#include <include/core/SkData.h>
#include <include/encode/SkPngEncoder.h>
#include <include/effects/SkDashPathEffect.h>
#include <cmath>
#include <cstring>
#include <limits>

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

SkColor swapRedBlueChannels(SkColor color) {
    return SkColorSetARGB(
        SkColorGetA(color),
        SkColorGetB(color),
        SkColorGetG(color),
        SkColorGetR(color)
    );
}

void normalizeStrokeColorsForRasterExport(std::vector<Stroke>& strokes) {
    for (auto& stroke : strokes) {
        if (stroke.isEraser) {
            continue;
        }

        stroke.paint.setColor(swapRedBlueChannels(stroke.paint.getColor()));
    }
}

SkRect boundsForStrokeCopies(const std::unordered_map<size_t, Stroke>& strokes) {
    bool hasPoint = false;
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();

    for (const auto& entry : strokes) {
        for (const auto& point : entry.second.points) {
            hasPoint = true;
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
    }

    if (!hasPoint) {
        return SkRect::MakeEmpty();
    }

    if (std::fabs(maxX - minX) < 1.0f) {
        minX -= 0.5f;
        maxX += 0.5f;
    }
    if (std::fabs(maxY - minY) < 1.0f) {
        minY -= 0.5f;
        maxY += 0.5f;
    }

    return SkRect::MakeLTRB(minX, minY, maxX, maxY);
}

void getSelectionHandlePoints(
    const SkRect& bounds,
    int handleIndex,
    float& anchorX,
    float& anchorY,
    float& handleX,
    float& handleY,
    bool& affectsX,
    bool& affectsY
) {
    const float centerX = (bounds.fLeft + bounds.fRight) * 0.5f;
    const float centerY = (bounds.fTop + bounds.fBottom) * 0.5f;

    switch (handleIndex) {
        case 0:
            anchorX = bounds.fRight; anchorY = bounds.fBottom;
            handleX = bounds.fLeft; handleY = bounds.fTop;
            affectsX = true; affectsY = true;
            break;
        case 1:
            anchorX = centerX; anchorY = bounds.fBottom;
            handleX = centerX; handleY = bounds.fTop;
            affectsX = false; affectsY = true;
            break;
        case 2:
            anchorX = bounds.fLeft; anchorY = bounds.fBottom;
            handleX = bounds.fRight; handleY = bounds.fTop;
            affectsX = true; affectsY = true;
            break;
        case 3:
            anchorX = bounds.fRight; anchorY = centerY;
            handleX = bounds.fLeft; handleY = centerY;
            affectsX = true; affectsY = false;
            break;
        case 4:
            anchorX = bounds.fLeft; anchorY = centerY;
            handleX = bounds.fRight; handleY = centerY;
            affectsX = true; affectsY = false;
            break;
        case 5:
            anchorX = bounds.fRight; anchorY = bounds.fTop;
            handleX = bounds.fLeft; handleY = bounds.fBottom;
            affectsX = true; affectsY = true;
            break;
        case 6:
            anchorX = centerX; anchorY = bounds.fTop;
            handleX = centerX; handleY = bounds.fBottom;
            affectsX = false; affectsY = true;
            break;
        case 7:
        default:
            anchorX = bounds.fLeft; anchorY = bounds.fTop;
            handleX = bounds.fRight; handleY = bounds.fBottom;
            affectsX = true; affectsY = true;
            break;
    }
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

void SkiaDrawingEngine::eraseObjects() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    // Helper lambda to remove strokes and update eraser references
    auto removeStrokes = [this](const std::unordered_set<size_t>& indicesToRemove) {
        // Capture (idx, stroke) pairs for the removed entries BEFORE
        // mutating strokes_. The delta needs them so undo can re-insert
        // at the original indices. Sorted ascending.
        std::vector<size_t> sortedIndices(indicesToRemove.begin(), indicesToRemove.end());
        std::sort(sortedIndices.begin(), sortedIndices.end());
        StrokeDelta delta;
        delta.kind = StrokeDelta::Kind::RemoveStrokes;
        delta.removedStrokes.reserve(sortedIndices.size());
        for (size_t idx : sortedIndices) {
            if (idx < strokes_.size()) {
                delta.removedStrokes.emplace_back(idx, strokes_[idx]);
            }
        }

        std::unordered_map<size_t, size_t> oldToNew;
        size_t newIdx = 0;
        std::vector<Stroke> remaining;
        remaining.reserve(strokes_.size() - indicesToRemove.size());

        for (size_t i = 0; i < strokes_.size(); ++i) {
            if (indicesToRemove.count(i) == 0) {
                oldToNew[i] = newIdx++;
                remaining.push_back(strokes_[i]);
            }
        }
        for (auto& s : remaining) {
            if (s.isEraser) {
                std::unordered_set<size_t> updated;
                for (size_t old : s.affectedStrokeIndices)
                    if (oldToNew.count(old)) updated.insert(oldToNew[old]);
                s.affectedStrokeIndices = updated;
            }
        }
        if (remaining.size() != strokes_.size()) {
            strokes_ = remaining;
            commitDelta(std::move(delta));
            needsStrokeRedraw_ = true;
        }
    };

    if (!pendingDeleteIndices_.empty()) {
        removeStrokes(pendingDeleteIndices_);
        pendingDeleteIndices_.clear();
        currentPoints_.clear();
        currentPath_.reset();
        return;
    }

    if (currentPoints_.empty()) return;

    // Build eraser bounds from current points
    SkRect eraserBounds = SkRect::MakeXYWH(currentPoints_[0].x, currentPoints_[0].y, 0, 0);
    for (const auto& pt : currentPoints_) {
        eraserBounds.join(SkRect::MakeXYWH(pt.x, pt.y, 0, 0));
    }
    eraserBounds.outset(currentPaint_.getStrokeWidth() / 2.0f, currentPaint_.getStrokeWidth() / 2.0f);

    std::unordered_set<size_t> toRemove;
    for (size_t i = 0; i < strokes_.size(); ++i) {
        if (strokes_[i].path.getBounds().intersects(eraserBounds)) toRemove.insert(i);
    }
    removeStrokes(toRemove);
    currentPoints_.clear();
    currentPath_.reset();
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

void SkiaDrawingEngine::setBackgroundType(const char* backgroundType) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    backgroundType_ = backgroundType ? backgroundType : "plain";
}

std::string SkiaDrawingEngine::getBackgroundType() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);
    return backgroundType_;
}

void SkiaDrawingEngine::setPdfBackgroundImage(sk_sp<SkImage> image) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    pdfBackgroundImage_ = image;
}

void SkiaDrawingEngine::renderStrokeGeometry(SkCanvas* canvas, const Stroke& stroke, const SkPaint& paint) {
    if (!canvas) {
        return;
    }

    if (isRecognizedShapeToolType(stroke.toolType)) {
        SkPath shapePath = stroke.path;
        if (shapePath.isEmpty()) {
            buildRecognizedShapePath(stroke.toolType, stroke.points, shapePath);
        }

        if (shapePath.isEmpty()) {
            return;
        }

        SkPaint shapePaint = paint;
        shapePaint.setStyle(SkPaint::kStroke_Style);
        shapePaint.setStrokeWidth(averageCalculatedWidth(stroke.points));
        shapePaint.setStrokeJoin(stroke.toolType == "shape-line"
            ? SkPaint::kRound_Join
            : SkPaint::kMiter_Join);
        shapePaint.setStrokeCap(stroke.toolType == "shape-line"
            ? SkPaint::kRound_Cap
            : SkPaint::kButt_Cap);
        canvas->drawPath(shapePath, shapePaint);
        return;
    }

    if (stroke.toolType == "crayon") {
        pathRenderer_->drawCrayonPath(canvas, stroke.points, paint, false);
    } else if (stroke.toolType == "calligraphy") {
        pathRenderer_->drawCalligraphyPath(canvas, stroke.points, paint, false);
    } else {
        pathRenderer_->drawVariableWidthPath(canvas, stroke.points, paint, false);
    }
}

void SkiaDrawingEngine::redrawStrokes() {
    if (!needsStrokeRedraw_) return;

    SkCanvas* canvas = strokeSurface_->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);

    // Helper to render a single stroke with per-stroke eraser clipping
    auto renderStroke = [&](size_t i) {
        const auto& stroke = strokes_[i];
        SkPaint strokePaint = stroke.paint;

        if (!stroke.isEraser) {
            uint8_t baseAlpha = stroke.paint.getAlpha();
            strokePaint.setAlpha(static_cast<uint8_t>(baseAlpha * stroke.originalAlphaMod));
        }

        if (pendingDeleteIndices_.count(i) > 0) {
            strokePaint.setAlpha(strokePaint.getAlpha() * 0.3);
        }

        // Apply per-stroke eraser clipping if this stroke has been erased
        bool needsClipRestore = false;
        if (!stroke.erasedBy.empty()) {
            // Ensure cache is up-to-date (builds smooth capsule shapes between circles)
            stroke.ensureEraserCacheValid();
            if (!stroke.cachedEraserPath.isEmpty()) {
                // Clip out the erased regions (kDifference = draw everywhere EXCEPT cached path)
                canvas->save();
                canvas->clipPath(stroke.cachedEraserPath, SkClipOp::kDifference);
                needsClipRestore = true;
            }
        }

        // Render stroke (curves unchanged - clipping handles pixel-perfect erasure)
        renderStrokeGeometry(canvas, stroke, strokePaint);

        if (needsClipRestore) {
            canvas->restore();
        }
    };

    // Render all strokes - eraser effect applied via per-stroke clipping
    for (size_t strokeIdx = 0; strokeIdx < strokes_.size(); ++strokeIdx) {
        renderStroke(strokeIdx);
    }

    // All strokes are now in strokeSurface_
    maxAffectedStrokeIndex_ = strokes_.size();

    // Cache snapshot for fast rendering (avoid makeImageSnapshot every frame)
    cachedStrokeSnapshot_ = strokeSurface_->makeImageSnapshot();

    needsStrokeRedraw_ = false;
}

void SkiaDrawingEngine::redrawEraserMask() {
    if (!needsEraserMaskRedraw_) return;

    SkCanvas* canvas = eraserMaskSurface_->getCanvas();
    canvas->clear(SK_ColorWHITE);  // Full alpha (255) = visible

    if (!eraserCircles_.empty()) {
        SkPaint erasePaint;
        erasePaint.setBlendMode(SkBlendMode::kClear);  // Sets pixels to 0 alpha (transparent = erased)
        erasePaint.setAntiAlias(true);
        erasePaint.setStyle(SkPaint::kFill_Style);

        // Build path from all circles (or use cached if available)
        if (eraserCircles_.size() != cachedEraserCircleCount_) {
            cachedEraserPath_.reset();
            for (const auto& circle : eraserCircles_) {
                cachedEraserPath_.addCircle(circle.x, circle.y, circle.radius);
            }
            cachedEraserCircleCount_ = eraserCircles_.size();
        }

        canvas->drawPath(cachedEraserPath_, erasePaint);
    }

    needsEraserMaskRedraw_ = false;
}

void SkiaDrawingEngine::render(SkCanvas* canvas) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    // OPTIMIZATION: When dragging selection, use all cached snapshots - pure O(1) per frame
    if (!selectedIndices_.empty() && isDraggingSelection_) {
        // Draw cached background - O(1)
        if (dragBackgroundSnapshot_) {
            canvas->drawImage(dragBackgroundSnapshot_, 0, 0);
        }

        // Draw cached non-selected strokes - O(1)
        if (nonSelectedSnapshot_) {
            canvas->drawImage(nonSelectedSnapshot_, 0, 0);
        }

        // Draw cached selected strokes with offset - O(1)
        if (selectedSnapshot_) {
            canvas->drawImage(selectedSnapshot_, selectionOffsetX_, selectionOffsetY_);
        }
    } else {
        // Normal path: draw background
        if (backgroundType_ == "pdf") {
            if (pdfBackgroundImage_) {
                canvas->clear(SK_ColorWHITE);
                backgroundRenderer_->drawBackground(
                    canvas,
                    backgroundType_,
                    width_,
                    height_,
                    pdfBackgroundImage_,
                    backgroundOriginY_
                );
            } else {
                canvas->clear(SK_ColorTRANSPARENT);
            }
        } else {
            canvas->clear(SK_ColorWHITE);
            backgroundRenderer_->drawBackground(
                canvas,
                backgroundType_,
                width_,
                height_,
                pdfBackgroundImage_,
                backgroundOriginY_
            );
        }

        // Draw strokes
        if (!selectedIndices_.empty() && !needsStrokeRedraw_) {
            // Selection exists but not actively dragging - render all strokes directly
            for (size_t i = 0; i < strokes_.size(); ++i) {
                const auto& stroke = strokes_[i];
                SkPaint strokePaint = stroke.paint;

                if (!stroke.isEraser) {
                    uint8_t baseAlpha = stroke.paint.getAlpha();
                    strokePaint.setAlpha(static_cast<uint8_t>(baseAlpha * stroke.originalAlphaMod));
                }

                bool needsClipRestore = false;
                if (!stroke.erasedBy.empty()) {
                    stroke.ensureEraserCacheValid();
                    if (!stroke.cachedEraserPath.isEmpty()) {
                        canvas->save();
                        canvas->clipPath(stroke.cachedEraserPath, SkClipOp::kDifference);
                        needsClipRestore = true;
                    }
                }

                renderStrokeGeometry(canvas, stroke, strokePaint);

                if (needsClipRestore) {
                    canvas->restore();
                }
            }
        } else {
            // Normal path: use cached surface
            redrawStrokes();

            // OPTIMIZATION: If object eraser is active, clip out pending-delete strokes
            if (!pendingDeleteIndices_.empty() && cachedStrokeSnapshot_) {
                SkPath excludePath;
                for (size_t idx : pendingDeleteIndices_) {
                    if (idx >= strokes_.size()) continue;
                    SkRect bounds = strokes_[idx].path.getBounds();
                    bounds.outset(strokes_[idx].paint.getStrokeWidth(), strokes_[idx].paint.getStrokeWidth());
                    excludePath.addRect(bounds);
                }

                canvas->save();
                canvas->clipPath(excludePath, SkClipOp::kDifference);
                canvas->drawImage(cachedStrokeSnapshot_, 0, 0);
                canvas->restore();

                for (size_t idx : pendingDeleteIndices_) {
                    if (idx >= strokes_.size()) continue;
                    const auto& stroke = strokes_[idx];

                    SkPaint dimPaint = stroke.paint;
                    uint8_t baseAlpha = stroke.paint.getAlpha();
                    dimPaint.setAlpha(static_cast<uint8_t>(baseAlpha * stroke.originalAlphaMod * 0.3f));

                    bool needsClipRestore = false;
                    if (!stroke.erasedBy.empty()) {
                        stroke.ensureEraserCacheValid();
                        if (!stroke.cachedEraserPath.isEmpty()) {
                            canvas->save();
                            canvas->clipPath(stroke.cachedEraserPath, SkClipOp::kDifference);
                            needsClipRestore = true;
                        }
                    }

                    renderStrokeGeometry(canvas, stroke, dimPaint);

                    if (needsClipRestore) {
                        canvas->restore();
                    }
                }
            } else if (cachedStrokeSnapshot_) {
                canvas->drawImage(cachedStrokeSnapshot_, 0, 0);
            }
        }
    }

    // 4. Draw active stroke incrementally (O(1) per frame instead of O(n))
    if (hasActiveShapePreview_ && !activeShapePreviewPoints_.empty()) {
        Stroke previewStroke;
        previewStroke.points = activeShapePreviewPoints_;
        previewStroke.paint = currentPaint_;
        previewStroke.path = activeShapePreviewPath_;
        previewStroke.toolType = activeShapePreviewToolType_;

        SkPaint previewPaint = currentPaint_;
        if (currentTool_ != "highlighter" && currentTool_ != "marker") {
            const float pressureAlphaMod = 0.85f + (averagePressure(currentPoints_) * 0.15f);
            previewPaint.setAlpha(static_cast<uint8_t>(previewPaint.getAlpha() * pressureAlphaMod));
        }

        renderStrokeGeometry(canvas, previewStroke, previewPaint);
    } else if (currentPoints_.size() >= 2 && currentTool_ != "select" && currentTool_ != "eraser") {
        activeStrokeRenderer_->renderIncremental(canvas, currentPoints_, currentPaint_, currentTool_);
    }

    // Draw eraser cursor for pixel eraser
    if (showEraserCursor_ && eraserCursorRadius_ > 0) {
        SkPaint cursorPaint;
        cursorPaint.setStyle(SkPaint::kStroke_Style);
        cursorPaint.setColor(SkColorSetARGB(180, 128, 128, 128));
        cursorPaint.setStrokeWidth(2.0f);
        cursorPaint.setAntiAlias(true);

        canvas->drawCircle(eraserCursorX_, eraserCursorY_, eraserCursorRadius_, cursorPaint);
    }

    // Draw lasso path if active (during selection drag)
    if (currentTool_ == "select") {
        selection_->renderLasso(canvas);
    }

    // Draw selection highlight if strokes are selected
    if (isDraggingSelection_ && selectionHighlightSnapshot_) {
        // During drag, use cached highlight with offset - O(1)
        canvas->drawImage(selectionHighlightSnapshot_, selectionOffsetX_, selectionOffsetY_);
    } else {
        selection_->renderSelection(canvas, strokes_, selectedIndices_);
    }
}

sk_sp<SkImage> SkiaDrawingEngine::makeSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    SkImageInfo info = SkImageInfo::MakeN32Premul(width_, height_);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    render(surface->getCanvas());
    return surface->makeImageSnapshot();
}

std::vector<std::string> SkiaDrawingEngine::batchExportPages(
    const std::vector<std::vector<uint8_t>>& pagesData,
    const std::vector<std::string>& backgroundTypes,
    const std::vector<sk_sp<SkImage>>& pdfBackgrounds,
    const std::vector<int>& pageIndices,
    float scale
) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    std::vector<std::string> results;
    results.reserve(pagesData.size());

    int scaledWidth = static_cast<int>(width_ * scale);
    int scaledHeight = static_cast<int>(height_ * scale);
    SkImageInfo info = SkImageInfo::MakeN32Premul(scaledWidth, scaledHeight);
    sk_sp<SkSurface> exportSurface = SkSurfaces::Raster(info);

    if (!exportSurface) {
        printf("[C++] batchExportPages: Failed to create export surface\n");
        return results;
    }

    // Save original state
    auto originalStrokes = strokes_;
    auto originalEraserCircles = eraserCircles_;
    auto originalPdfBackground = pdfBackgroundImage_;
    auto originalBackgroundType = backgroundType_;
    float originalBackgroundOriginY = backgroundOriginY_;
    auto originalUndoStack = undoStack_;
    auto originalRedoStack = redoStack_;

    for (size_t i = 0; i < pagesData.size(); ++i) {
        SkCanvas* canvas = exportSurface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        canvas->save();
        canvas->scale(scale, scale);

        backgroundType_ = (i < backgroundTypes.size() && !backgroundTypes[i].empty())
            ? backgroundTypes[i] : "plain";
        pdfBackgroundImage_ = (i < pdfBackgrounds.size()) ? pdfBackgrounds[i] : nullptr;
        int pageIndex = (i < pageIndices.size()) ? pageIndices[i] : static_cast<int>(i);
        backgroundOriginY_ = std::max(0, pageIndex) * static_cast<float>(height_);

        if (!pagesData[i].empty()) {
            if (!deserializeDrawing(pagesData[i])) {
                strokes_.clear();
                eraserCircles_.clear();
            }
        } else {
            strokes_.clear();
            eraserCircles_.clear();
        }

        normalizeStrokeColorsForRasterExport(strokes_);

        needsStrokeRedraw_ = true;
        needsEraserMaskRedraw_ = true;
        render(canvas);
        canvas->restore();

        sk_sp<SkImage> snapshot = exportSurface->makeImageSnapshot();
        if (snapshot) {
            sk_sp<SkData> pngData = SkPngEncoder::Encode(nullptr, snapshot.get(), {});
            if (pngData) {
                results.push_back("data:image/png;base64," +
                    BatchExporter::encodeBase64(pngData->data(), pngData->size()));
            } else {
                results.push_back("");
            }
        } else {
            results.push_back("");
        }
    }

    // Restore original state
    strokes_ = originalStrokes;
    eraserCircles_ = originalEraserCircles;
    cachedEraserCircleCount_ = 0;
    pdfBackgroundImage_ = originalPdfBackground;
    backgroundType_ = originalBackgroundType;
    backgroundOriginY_ = originalBackgroundOriginY;
    undoStack_ = std::move(originalUndoStack);
    redoStack_ = std::move(originalRedoStack);
    needsStrokeRedraw_ = true;
    needsEraserMaskRedraw_ = true;

    return results;
}

void SkiaDrawingEngine::setEraserCursor(float x, float y, float radius, bool visible) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    eraserCursorX_ = x;
    eraserCursorY_ = y;
    eraserCursorRadius_ = radius;
    showEraserCursor_ = visible;
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

// Selection operations - delegate to selection module
bool SkiaDrawingEngine::selectStrokeAt(float x, float y) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    // Selection highlight is drawn directly to output canvas, no stroke redraw needed
    bool changed = selection_->selectStrokeAt(x, y, strokes_, selectedIndices_);
    if (changed) {
        // Pre-cache for smooth drag start
        prepareSelectionDragCache();
    }
    return changed;
}

bool SkiaDrawingEngine::selectShapeStrokeAt(float x, float y) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    // Finger tap selection is object-like: only recognized, snapped shape
    // strokes should behave as tappable objects. Freehand handwriting remains
    // selectable through the explicit select/lasso tool.
    bool changed = selection_->selectShapeStrokeAt(x, y, strokes_, selectedIndices_);
    if (changed) {
        prepareSelectionDragCache();
    }
    return changed;
}

void SkiaDrawingEngine::clearSelection() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    // Selection highlight is drawn directly to output canvas, no stroke redraw needed
    if (!selectedIndices_.empty()) {
        selection_->clearSelection(selectedIndices_);
        endSelectionDrag();  // Free cached snapshots to prevent memory leak
    }
}

void SkiaDrawingEngine::deleteSelection() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    auto commit = [this](StrokeDelta&& d) { commitDelta(std::move(d)); };
    selection_->deleteSelection(strokes_, selectedIndices_, commit);
    needsStrokeRedraw_ = true;
}

void SkiaDrawingEngine::copySelection() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    selection_->copySelection(strokes_, selectedIndices_, copiedStrokes_);
}

void SkiaDrawingEngine::pasteSelection(float offsetX, float offsetY) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    auto commit = [this](StrokeDelta&& d) { commitDelta(std::move(d)); };
    selection_->pasteSelection(strokes_, copiedStrokes_, offsetX, offsetY, commit);
    needsStrokeRedraw_ = true;
}

void SkiaDrawingEngine::moveSelection(float dx, float dy) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (selectedIndices_.empty()) return;

    if (!isDraggingSelection_) {
        beginSelectionDrag();
    }

    // OPTIMIZATION: Just accumulate offset - O(1), no path/point modification
    // Selected strokes are rendered from cached snapshot with offset
    selectionOffsetX_ += dx;
    selectionOffsetY_ += dy;
}

void SkiaDrawingEngine::finalizeMove() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (selectedIndices_.empty()) return;

    // Capture totals before resetting in moveSelection's path-update step
    // (which relies on the live offset values).
    float totalDx = selectionOffsetX_;
    float totalDy = selectionOffsetY_;

    // Apply accumulated offset to actual point data (ONE time, not per frame)
    if (isDraggingSelection_ && (totalDx != 0.0f || totalDy != 0.0f)) {
        // Use DrawingSelection to update points and rebuild paths
        selection_->moveSelection(strokes_, selectedIndices_, totalDx, totalDy);
    }

    auto commit = [this](StrokeDelta&& d) { commitDelta(std::move(d)); };
    selection_->finalizeMove(strokes_, selectedIndices_, totalDx, totalDy, commit);
    endSelectionDrag();
    // Now rebuild stroke surface with all strokes at final positions
    needsStrokeRedraw_ = true;
}

void SkiaDrawingEngine::beginSelectionTransform(int handleIndex) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (selectedIndices_.empty()) return;

    endSelectionDrag();
    selectionTransformOriginalStrokes_.clear();
    for (size_t idx : selectedIndices_) {
        if (idx < strokes_.size()) {
            selectionTransformOriginalStrokes_[idx] = strokes_[idx];
        }
    }

    isTransformingSelection_ = !selectionTransformOriginalStrokes_.empty();
    selectionTransformHasDelta_ = false;
    selectionTransformHandleIndex_ = handleIndex;
}

void SkiaDrawingEngine::updateSelectionTransform(float x, float y) {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (!isTransformingSelection_ || selectionTransformOriginalStrokes_.empty()) {
        return;
    }

    const SkRect originalBounds = boundsForStrokeCopies(selectionTransformOriginalStrokes_);
    if (originalBounds.isEmpty()) {
        return;
    }

    float anchorX = 0.0f;
    float anchorY = 0.0f;
    float handleX = 0.0f;
    float handleY = 0.0f;
    bool affectsX = false;
    bool affectsY = false;
    getSelectionHandlePoints(
        originalBounds,
        selectionTransformHandleIndex_,
        anchorX,
        anchorY,
        handleX,
        handleY,
        affectsX,
        affectsY
    );

    auto scaleForAxis = [](float anchor, float originalHandle, float current) {
        const float denominator = originalHandle - anchor;
        if (std::fabs(denominator) < 0.001f) {
            return 1.0f;
        }
        return std::max(0.05f, std::min(20.0f, (current - anchor) / denominator));
    };

    const float scaleX = affectsX ? scaleForAxis(anchorX, handleX, x) : 1.0f;
    const float scaleY = affectsY ? scaleForAxis(anchorY, handleY, y) : 1.0f;

    for (const auto& [idx, originalStroke] : selectionTransformOriginalStrokes_) {
        if (idx >= strokes_.size()) continue;

        Stroke transformed = originalStroke;
        for (auto& point : transformed.points) {
            point.x = anchorX + (point.x - anchorX) * scaleX;
            point.y = anchorY + (point.y - anchorY) * scaleY;
        }
        for (auto& circle : transformed.erasedBy) {
            circle.x = anchorX + (circle.x - anchorX) * scaleX;
            circle.y = anchorY + (circle.y - anchorY) * scaleY;
            circle.radius *= std::max(0.05f, (std::fabs(scaleX) + std::fabs(scaleY)) * 0.5f);
        }

        if (!buildRecognizedShapePath(transformed.toolType, transformed.points, transformed.path)) {
            smoothPath(transformed.points, transformed.path);
        }
        SkPathMeasure pathMeasure(transformed.path, false);
        transformed.pathLength = pathMeasure.getLength();
        transformed.cachedEraserCount = 0;
        strokes_[idx] = std::move(transformed);
    }

    selectionTransformHasDelta_ = true;
    needsStrokeRedraw_ = true;
}

void SkiaDrawingEngine::finalizeSelectionTransform() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (!isTransformingSelection_) {
        return;
    }

    if (selectionTransformHasDelta_) {
        StrokeDelta delta;
        delta.kind = StrokeDelta::Kind::ReplaceStrokes;
        for (const auto& [idx, beforeStroke] : selectionTransformOriginalStrokes_) {
            if (idx >= strokes_.size()) continue;
            delta.beforeStrokes.push_back({idx, beforeStroke});
            delta.afterStrokes.push_back({idx, strokes_[idx]});
        }
        if (!delta.beforeStrokes.empty()) {
            commitDelta(std::move(delta));
        }
    }

    selectionTransformOriginalStrokes_.clear();
    selectionTransformHandleIndex_ = -1;
    selectionTransformHasDelta_ = false;
    isTransformingSelection_ = false;
    needsStrokeRedraw_ = true;
}

void SkiaDrawingEngine::cancelSelectionTransform() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (!isTransformingSelection_) {
        return;
    }

    for (const auto& [idx, beforeStroke] : selectionTransformOriginalStrokes_) {
        if (idx < strokes_.size()) {
            strokes_[idx] = beforeStroke;
        }
    }

    selectionTransformOriginalStrokes_.clear();
    selectionTransformHandleIndex_ = -1;
    selectionTransformHasDelta_ = false;
    isTransformingSelection_ = false;
    needsStrokeRedraw_ = true;
}

void SkiaDrawingEngine::prepareSelectionDragCache() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (selectedIndices_.empty()) {
        hasDragCache_ = false;
        return;
    }

    SkImageInfo info = SkImageInfo::MakeN32Premul(width_, height_);

    // Cache the background (grid, lines, etc.)
    sk_sp<SkSurface> bgSurface = SkSurfaces::Raster(info);
    if (bgSurface) {
        SkCanvas* canvas = bgSurface->getCanvas();
        if (backgroundType_ == "pdf" && pdfBackgroundImage_) {
            canvas->clear(SK_ColorWHITE);
            backgroundRenderer_->drawBackground(
                canvas,
                backgroundType_,
                width_,
                height_,
                pdfBackgroundImage_,
                backgroundOriginY_
            );
        } else if (backgroundType_ == "pdf") {
            canvas->clear(SK_ColorTRANSPARENT);
        } else {
            canvas->clear(SK_ColorWHITE);
            backgroundRenderer_->drawBackground(
                canvas,
                backgroundType_,
                width_,
                height_,
                pdfBackgroundImage_,
                backgroundOriginY_
            );
        }
        dragBackgroundSnapshot_ = bgSurface->makeImageSnapshot();
    }

    // Create snapshot of non-selected strokes
    sk_sp<SkSurface> nonSelectedSurface = SkSurfaces::Raster(info);
    if (!nonSelectedSurface) return;

    SkCanvas* canvas = nonSelectedSurface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);

    for (size_t i = 0; i < strokes_.size(); ++i) {
        if (selectedIndices_.count(i) > 0) continue;

        const auto& stroke = strokes_[i];
        SkPaint strokePaint = stroke.paint;
        if (!stroke.isEraser) {
            uint8_t baseAlpha = stroke.paint.getAlpha();
            strokePaint.setAlpha(static_cast<uint8_t>(baseAlpha * stroke.originalAlphaMod));
        }

        bool needsClipRestore = false;
        if (!stroke.erasedBy.empty()) {
            stroke.ensureEraserCacheValid();
            if (!stroke.cachedEraserPath.isEmpty()) {
                canvas->save();
                canvas->clipPath(stroke.cachedEraserPath, SkClipOp::kDifference);
                needsClipRestore = true;
            }
        }

        renderStrokeGeometry(canvas, stroke, strokePaint);

        if (needsClipRestore) canvas->restore();
    }

    nonSelectedSnapshot_ = nonSelectedSurface->makeImageSnapshot();

    // Create snapshot of selected strokes
    sk_sp<SkSurface> selectedSurface = SkSurfaces::Raster(info);
    if (!selectedSurface) return;

    canvas = selectedSurface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);

    for (size_t idx : selectedIndices_) {
        if (idx >= strokes_.size()) continue;

        const auto& stroke = strokes_[idx];
        SkPaint strokePaint = stroke.paint;
        if (!stroke.isEraser) {
            uint8_t baseAlpha = stroke.paint.getAlpha();
            strokePaint.setAlpha(static_cast<uint8_t>(baseAlpha * stroke.originalAlphaMod));
        }

        bool needsClipRestore = false;
        if (!stroke.erasedBy.empty()) {
            stroke.ensureEraserCacheValid();
            if (!stroke.cachedEraserPath.isEmpty()) {
                canvas->save();
                canvas->clipPath(stroke.cachedEraserPath, SkClipOp::kDifference);
                needsClipRestore = true;
            }
        }

        renderStrokeGeometry(canvas, stroke, strokePaint);

        if (needsClipRestore) canvas->restore();
    }

    selectedSnapshot_ = selectedSurface->makeImageSnapshot();

    // Cache the selection highlight (dashed outlines)
    sk_sp<SkSurface> highlightSurface = SkSurfaces::Raster(info);
    if (highlightSurface) {
        canvas = highlightSurface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);
        selection_->renderSelection(canvas, strokes_, selectedIndices_);
        selectionHighlightSnapshot_ = highlightSurface->makeImageSnapshot();
    }

    hasDragCache_ = true;
}

void SkiaDrawingEngine::beginSelectionDrag() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    if (selectedIndices_.empty()) return;

    // Pre-cache if not already done
    if (!hasDragCache_) {
        prepareSelectionDragCache();
    }

    isDraggingSelection_ = true;
    selectionOffsetX_ = 0.0f;
    selectionOffsetY_ = 0.0f;
}

void SkiaDrawingEngine::endSelectionDrag() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    isDraggingSelection_ = false;
    hasDragCache_ = false;
    selectionOffsetX_ = 0.0f;
    selectionOffsetY_ = 0.0f;
    dragBackgroundSnapshot_ = nullptr;
    nonSelectedSnapshot_ = nullptr;
    selectedSnapshot_ = nullptr;
    selectionHighlightSnapshot_ = nullptr;
}

int SkiaDrawingEngine::getSelectionCount() const {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    return selection_->getSelectionCount(selectedIndices_);
}

std::vector<float> SkiaDrawingEngine::getSelectionBounds() {
    std::lock_guard<std::recursive_mutex> lock(stateMutex_);

    std::vector<float> bounds = selection_->getSelectionBounds(strokes_, selectedIndices_);
    if (bounds.size() >= 4 && isDraggingSelection_) {
        bounds[0] += selectionOffsetX_;
        bounds[1] += selectionOffsetY_;
        bounds[2] += selectionOffsetX_;
        bounds[3] += selectionOffsetY_;
    }
    return bounds;
}

std::vector<Stroke> SkiaDrawingEngine::splitStrokeAtPoint(
    const Stroke& originalStroke,
    const Point& eraserPoint,
    float eraserRadius
) {
    return strokeSplitter_->splitStrokeAtPoint(originalStroke, eraserPoint, eraserRadius);
}

void SkiaDrawingEngine::bakeEraserCircles() {
    if (eraserCircles_.size() <= bakedCircleCount_) return;  // Nothing new to bake

    // Apply kClear directly to strokeSurface_ using smooth stroke paths
    SkCanvas* canvas = strokeSurface_->getCanvas();

    eraserRenderer_->drawEraserCirclesAsStrokes(
        canvas, eraserCircles_, bakedCircleCount_, eraserCircles_.size());

    // Update cached snapshot
    cachedStrokeSnapshot_ = strokeSurface_->makeImageSnapshot();

    // Mark all circles as baked (but DON'T clear them - needed for redraw/undo)
    bakedCircleCount_ = eraserCircles_.size();

    printf("[C++] bakeEraserCircles: Baked %zu circles total\n", bakedCircleCount_);
}

bool SkiaDrawingEngine::applyPixelEraserAt(float eraserX, float eraserY, float radius) {
    // Pixel-perfect eraser: store eraser circles WITH strokes for render-time clipping
    // No splitting - curves stay intact, eraser data moves with stroke
    bool anyModified = false;

    // Build list of circles to add (may include interpolated circles for full coverage)
    std::vector<EraserCircle> circlesToAdd;
    circlesToAdd.push_back({eraserX, eraserY, radius});

    // Add intermediate circles along path from last position for full coverage
    if (hasLastEraserPoint_) {
        float dx = eraserX - lastEraserX_;
        float dy = eraserY - lastEraserY_;
        float dist = std::sqrt(dx * dx + dy * dy);
        float avgRadius = (radius + lastEraserRadius_) / 2.0f;

        // Add circles at intervals of radius/2 for overlapping coverage
        if (dist > avgRadius * 0.5f) {
            int steps = static_cast<int>(dist / (avgRadius * 0.5f));
            for (int s = 1; s < steps; s++) {
                float t = static_cast<float>(s) / steps;
                float ix = lastEraserX_ + dx * t;
                float iy = lastEraserY_ + dy * t;
                float ir = lastEraserRadius_ + (radius - lastEraserRadius_) * t;
                circlesToAdd.push_back({ix, iy, ir});
            }
        }
    }

    for (size_t strokeIndex = 0; strokeIndex < strokes_.size(); ++strokeIndex) {
        auto& stroke = strokes_[strokeIndex];
        if (stroke.isEraser) continue;
        if (stroke.points.size() < 2) continue;

        // Check if eraser path intersects stroke bounds (expand for full path)
        SkRect bounds = stroke.path.getBounds();
        bounds.outset(radius, radius);

        bool affectsStroke = false;
        for (const auto& circle : circlesToAdd) {
            if (bounds.contains(circle.x, circle.y)) {
                affectsStroke = true;
                break;
            }
        }
        if (!affectsStroke) continue;

        // Store all eraser circles with this stroke for full coverage.
        // Each circle is also recorded into pendingPixelEraseEntries_ so
        // touchEnded can emit a single PixelErase delta covering the
        // whole drag. Without this the per-stroke erasedBy mutations
        // would have no record in history -- pixel erase wouldn't undo.
        for (const auto& circle : circlesToAdd) {
            stroke.erasedBy.push_back(circle);
            recordPixelEraseCircleAdded(strokeIndex, circle);
        }

        // OPTIMIZATION: Only update visibility if stroke was previously visible
        // Skip strokes already marked invisible - they stay invisible
        if (stroke.cachedHasVisiblePoints) {
            // Quick check: do new circles potentially cover remaining visible points?
            // Only do full check every 50 circles to avoid O(n^2) during heavy erasing
            size_t circleCount = stroke.erasedBy.size();
            if (circleCount % 50 == 0 || circlesToAdd.size() > 10) {
                bool hasVisible = false;
                for (const auto& pt : stroke.points) {
                    bool pointVisible = true;
                    for (const auto& circle : stroke.erasedBy) {
                        float dx = pt.x - circle.x;
                        float dy = pt.y - circle.y;
                        float totalRadius = circle.radius + pt.calculatedWidth / 2.0f;
                        if (dx * dx + dy * dy <= totalRadius * totalRadius) {
                            pointVisible = false;
                            break;
                        }
                    }
                    if (pointVisible) {
                        hasVisible = true;
                        break;
                    }
                }
                stroke.cachedHasVisiblePoints = hasVisible;
            }
        }

        anyModified = true;
    }

    if (anyModified) {
        // OPTIMIZATION: Apply kClear directly to stroke surface for instant feedback
        // This avoids full redraw during active erasing - much faster
        if (strokeSurface_) {
            SkCanvas* canvas = strokeSurface_->getCanvas();
            SkPaint clearPaint;
            clearPaint.setBlendMode(SkBlendMode::kClear);
            clearPaint.setAntiAlias(true);

            // Draw stroked path from last position to current for full coverage
            // Individual circles leave gaps when moving quickly
            if (hasLastEraserPoint_) {
                // Draw a stroked line from last position to current
                SkPath eraserPath;
                eraserPath.moveTo(lastEraserX_, lastEraserY_);
                eraserPath.lineTo(eraserX, eraserY);

                clearPaint.setStyle(SkPaint::kStroke_Style);
                clearPaint.setStrokeWidth(radius * 2.0f);  // Diameter
                clearPaint.setStrokeCap(SkPaint::kRound_Cap);
                clearPaint.setStrokeJoin(SkPaint::kRound_Join);
                canvas->drawPath(eraserPath, clearPaint);
            } else {
                // First point - just draw circle
                canvas->drawCircle(eraserX, eraserY, radius, clearPaint);
            }

            cachedStrokeSnapshot_ = strokeSurface_->makeImageSnapshot();
        }
        // DON'T set needsStrokeRedraw_ = true - defer full redraw to touchEnded
    }

    // Track position for next call
    lastEraserX_ = eraserX;
    lastEraserY_ = eraserY;
    lastEraserRadius_ = radius;
    hasLastEraserPoint_ = true;

    return anyModified;
}

} // namespace nativedrawing
