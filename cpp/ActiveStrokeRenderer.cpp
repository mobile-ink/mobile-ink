#include "ActiveStrokeRenderer.h"
#include "PathRenderer.h"
#include <include/core/SkImageInfo.h>

namespace nativedrawing {

ActiveStrokeRenderer::ActiveStrokeRenderer(int width, int height, PathRenderer* pathRenderer)
    : pathRenderer_(pathRenderer)
    , lastRenderedInputIndex_(0)
    , hasLastEdge_(false)
    , lastHalfWidth_(-1.0f) {

    SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
    activeStrokeSurface_ = SkSurfaces::Raster(info);
    if (activeStrokeSurface_) {
        activeStrokeSurface_->getCanvas()->clear(SK_ColorTRANSPARENT);
    }
}

void ActiveStrokeRenderer::reset() {
    if (activeStrokeSurface_) {
        activeStrokeSurface_->getCanvas()->clear(SK_ColorTRANSPARENT);
    }
    cachedActiveSnapshot_ = nullptr;
    lastRenderedInputIndex_ = 0;
    overlapBuffer_.clear();
    hasLastEdge_ = false;
    lastLeftEdge_ = SkPoint::Make(0, 0);
    lastRightEdge_ = SkPoint::Make(0, 0);
    lastHalfWidth_ = -1.0f;  // Will use default baseWidth/2
}

void ActiveStrokeRenderer::renderIncremental(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& paint,
    const std::string& toolType
) {
    if (!activeStrokeSurface_ || points.size() < 2) return;

    // Calligraphy: full redraw each frame for clean rendering
    // The incremental approach causes overlap artifacts on thin strokes
    if (toolType == "calligraphy") {
        pathRenderer_->drawCalligraphyPath(canvas, points, paint, true);
        return;
    }

    // Spline overlap: Catmull-Rom needs p[i-1], p[i], p[i+1], p[i+2]
    // We can only "finalize" points up to points.size() - 2
    size_t renderableUpTo = (points.size() > OVERLAP) ? points.size() - OVERLAP : 0;

    // Render new points to the cached surface
    if (renderableUpTo > lastRenderedInputIndex_ && renderableUpTo >= 2) {
        std::vector<Point> segment;
        segment.reserve(overlapBuffer_.size() + (renderableUpTo - lastRenderedInputIndex_));

        // Add overlap from previous render (for spline continuity)
        for (const auto& pt : overlapBuffer_) {
            segment.push_back(pt);
        }

        // Add new points
        size_t startIdx = (lastRenderedInputIndex_ > 0) ? lastRenderedInputIndex_ : 0;
        for (size_t i = startIdx; i < renderableUpTo; ++i) {
            segment.push_back(points[i]);
        }

        if (segment.size() >= 2) {
            SkCanvas* surfaceCanvas = activeStrokeSurface_->getCanvas();
            bool isFirstSegment = !hasLastEdge_;

            IncrementalResult result;
            if (toolType == "crayon") {
                result = pathRenderer_->drawCrayonPathIncremental(
                    surfaceCanvas, segment, paint,
                    lastLeftEdge_, lastRightEdge_, isFirstSegment);
                // Draw start cap on first segment
                if (isFirstSegment) {
                    pathRenderer_->drawCrayonStartCap(surfaceCanvas, segment, paint);
                }
            } else if (toolType == "calligraphy") {
                result = pathRenderer_->drawCalligraphyPathIncremental(
                    surfaceCanvas, segment, paint,
                    lastLeftEdge_, lastRightEdge_, isFirstSegment,
                    lastHalfWidth_);
                lastHalfWidth_ = result.lastHalfWidth;
                // Calligraphy has tapered ends, no caps needed
            } else {
                result = pathRenderer_->drawVariableWidthPathIncremental(
                    surfaceCanvas, segment, paint,
                    lastLeftEdge_, lastRightEdge_, isFirstSegment);
                // Draw start cap on first segment
                if (isFirstSegment) {
                    pathRenderer_->drawVariableWidthStartCap(surfaceCanvas, segment, paint);
                }
            }

            lastLeftEdge_ = result.lastLeftEdge;
            lastRightEdge_ = result.lastRightEdge;
            hasLastEdge_ = true;

            // Update overlap buffer
            overlapBuffer_.clear();
            size_t overlapStart = (renderableUpTo >= OVERLAP) ? renderableUpTo - OVERLAP : 0;
            for (size_t i = overlapStart; i < renderableUpTo; ++i) {
                overlapBuffer_.push_back(points[i]);
            }

            lastRenderedInputIndex_ = renderableUpTo;
            cachedActiveSnapshot_ = activeStrokeSurface_->makeImageSnapshot();
        }
    }

    // Draw cached portion to output canvas
    if (cachedActiveSnapshot_) {
        canvas->drawImage(cachedActiveSnapshot_, 0, 0);
    }

    // Draw "tail" - recent points not yet finalized
    if (points.size() > lastRenderedInputIndex_) {
        std::vector<Point> tail;
        tail.reserve(overlapBuffer_.size() + (points.size() - lastRenderedInputIndex_));

        for (const auto& pt : overlapBuffer_) {
            tail.push_back(pt);
        }
        for (size_t i = lastRenderedInputIndex_; i < points.size(); ++i) {
            tail.push_back(points[i]);
        }

        if (tail.size() >= 2) {
            if (toolType == "crayon") {
                // drawCrayonPathTail already draws end cap
                pathRenderer_->drawCrayonPathTail(canvas, tail, paint,
                    lastLeftEdge_, lastRightEdge_, hasLastEdge_);
            } else if (toolType == "calligraphy") {
                // Calligraphy has tapered ends, no caps needed
                pathRenderer_->drawCalligraphyPathTail(canvas, tail, paint,
                    lastLeftEdge_, lastRightEdge_, hasLastEdge_, lastHalfWidth_);
            } else {
                pathRenderer_->drawVariableWidthPathTail(canvas, tail, paint,
                    lastLeftEdge_, lastRightEdge_, hasLastEdge_);
                // Draw end cap at current tip
                pathRenderer_->drawVariableWidthEndCap(canvas, tail, paint);
            }
        }
    }
}

void ActiveStrokeRenderer::renderFinalTail(
    const std::vector<Point>& points,
    const SkPaint& paint,
    const std::string& toolType
) {
    if (!activeStrokeSurface_ || points.size() < 2) return;

    std::vector<Point> finalTail;
    finalTail.reserve(overlapBuffer_.size() + (points.size() - lastRenderedInputIndex_));

    for (const auto& pt : overlapBuffer_) {
        finalTail.push_back(pt);
    }
    for (size_t i = lastRenderedInputIndex_; i < points.size(); ++i) {
        finalTail.push_back(points[i]);
    }

    if (finalTail.size() < 2) return;

    SkCanvas* surfaceCanvas = activeStrokeSurface_->getCanvas();

    if (toolType == "crayon") {
        pathRenderer_->drawCrayonPathIncremental(
            surfaceCanvas, finalTail, paint,
            lastLeftEdge_, lastRightEdge_, !hasLastEdge_);
        // Only draw end cap - start cap was already drawn during incremental rendering
        pathRenderer_->drawCrayonEndCap(surfaceCanvas, points, paint);
    } else if (toolType == "calligraphy") {
        pathRenderer_->drawCalligraphyPathIncremental(
            surfaceCanvas, finalTail, paint,
            lastLeftEdge_, lastRightEdge_, !hasLastEdge_,
            lastHalfWidth_);
        // Calligraphy has tapered ends, no caps needed
    } else {
        pathRenderer_->drawVariableWidthPathIncremental(
            surfaceCanvas, finalTail, paint,
            lastLeftEdge_, lastRightEdge_, !hasLastEdge_);
        // Only draw end cap - start cap was already drawn during incremental rendering
        pathRenderer_->drawVariableWidthEndCap(surfaceCanvas, points, paint);
    }

    cachedActiveSnapshot_ = activeStrokeSurface_->makeImageSnapshot();
}

} // namespace nativedrawing
