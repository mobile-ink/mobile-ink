#include "PathRenderer.h"

#include <algorithm>
#include <cmath>

#include <include/core/SkColorFilter.h>
#include <include/effects/SkColorMatrix.h>
#include <include/effects/SkPerlinNoiseShader.h>

namespace nativedrawing {

sk_sp<SkShader> PathRenderer::createCrayonShader(SkColor baseColor, float pressure, float width, float strokeAngle) {
    // ===== CHECK SHADER CACHE FIRST =====
    uint64_t cacheKey = getShaderCacheKey(baseColor, pressure, strokeAngle);
    auto cacheIt = shaderCache_.find(cacheKey);
    if (cacheIt != shaderCache_.end()) {
        return cacheIt->second;  // Return cached shader - HUGE performance win!
    }

    // ===== PENCILKIT-STYLE CRAYON TEXTURE =====
    // Key characteristics:
    // 1. Very visible directional streaks (lined texture)
    // 2. DRAMATIC pressure variance (light = almost nothing, heavy = solid)
    // 3. Sharp on/off threshold (no mushy in-between)

    // ===== LAYER 1: STRONG DIRECTIONAL STREAKS =====
    // Extreme anisotropy for visible "lined" texture like PencilKit
    float freqAlong = 0.008f;   // Very low = long continuous streaks
    float freqAcross = 0.35f;   // Very high = fine perpendicular detail (44:1 ratio)

    sk_sp<SkShader> grainNoise = SkShaders::MakeTurbulence(
        freqAlong, freqAcross,
        4,      // High octaves for detail
        0.0f,
        nullptr
    );

    // Rotate grain to follow stroke direction
    SkMatrix rotationMatrix;
    float angleDegrees = strokeAngle * (180.0f / 3.14159265f);
    rotationMatrix.setRotate(angleDegrees);
    sk_sp<SkShader> rotatedGrain = grainNoise->makeWithLocalMatrix(rotationMatrix);

    // ===== LAYER 2: PAPER GRAIN (finer) =====
    // Simulates paper texture that wax deposits on
    sk_sp<SkShader> paperNoise = SkShaders::MakeTurbulence(
        0.12f, 0.12f,  // Finer grain for paper texture
        3,
        42.0f,
        nullptr
    );

    // ===== COMBINE: Multiply creates realistic wax-on-paper =====
    sk_sp<SkShader> combinedNoise = SkShaders::Blend(
        SkBlendMode::kMultiply,
        rotatedGrain,
        paperNoise
    );

    // ===== DRAMATIC PRESSURE-CONTROLLED THRESHOLD =====
    // PencilKit has HUGE variance: light = barely visible, heavy = nearly solid
    //
    // Scale: Higher = sharper threshold (less gray, more black/white)
    // Offset: Controls coverage (negative = sparse, positive = dense)
    //
    // Key: Light pressure must be REALLY light, solid only at heavy pressure
    // Light pressure (0.1): offset=-1.05 -> extremely sparse
    // Medium pressure (0.5): offset=-0.45 -> moderate coverage
    // Heavy pressure (1.0): offset=0.3 -> solid
    float scale = 1.5f + pressure * 1.0f;     // 1.5 to 2.5 - moderate sharpness
    float offset = -1.2f + pressure * 1.5f;   // -1.2 to 0.3 - very gradual ramp

    float alphaMatrix[20] = {
        0, 0, 0, 0, SkColorGetR(baseColor) / 255.0f,
        0, 0, 0, 0, SkColorGetG(baseColor) / 255.0f,
        0, 0, 0, 0, SkColorGetB(baseColor) / 255.0f,
        scale, scale, scale, 0, offset
    };

    sk_sp<SkColorFilter> thresholdFilter = SkColorFilters::Matrix(alphaMatrix);

    sk_sp<SkShader> resultShader = combinedNoise->makeWithColorFilter(thresholdFilter);

    // Cache the shader for future reuse
    shaderCache_[cacheKey] = resultShader;

    return resultShader;
}

void PathRenderer::drawCrayonPath(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    bool applyPressureAlpha
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    SkColor baseColor = basePaint.getColor();
    float strokeWidth = basePaint.getStrokeWidth();

    // Generate smoothed points with pressure using helper
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    // Build edges using helper
    std::vector<EdgePoint> edges;
    if (!buildEdgePoints(smoothedPoints, edges)) return;

    // ===== BATCH RENDERING WITH PER-POINT PRESSURE =====
    // OPTIMIZED: Increased batch size for 50% fewer shader lookups
    // Texture noise masks batch boundaries, so larger batches are visually acceptable
    constexpr int BATCH_SIZE = 24;

    for (size_t batchStart = 0; batchStart < edges.size() - 1; batchStart += BATCH_SIZE) {
        size_t batchEnd = std::min(batchStart + BATCH_SIZE, edges.size() - 1);

        // Calculate batch pressure and angle (smooth local average)
        float batchPressure = 0.0f;
        float batchDx = 0.0f, batchDy = 0.0f;
        for (size_t i = batchStart; i <= batchEnd; i++) {
            batchPressure += edges[i].pressure;
            batchDx += std::cos(edges[i].angle);
            batchDy += std::sin(edges[i].angle);
        }
        batchPressure /= (batchEnd - batchStart + 1);
        batchPressure = std::max(0.1f, std::min(1.0f, batchPressure));
        float batchAngle = std::atan2(batchDy, batchDx);

        // Create shader with this batch's pressure
        sk_sp<SkShader> batchShader = createCrayonShader(baseColor, batchPressure, strokeWidth, batchAngle);

        SkPaint batchPaint;
        batchPaint.setShader(batchShader);
        batchPaint.setStyle(SkPaint::kFill_Style);
        batchPaint.setAntiAlias(false);  // No AA - let texture create rough edges
        batchPaint.setAlpha(255);

        // Build path for this micro-batch
        SkPath batchPath;
        batchPath.moveTo(edges[batchStart].left);

        for (size_t i = batchStart + 1; i <= batchEnd; i++) {
            batchPath.lineTo(edges[i].left);
        }

        for (int i = static_cast<int>(batchEnd); i >= static_cast<int>(batchStart); i--) {
            batchPath.lineTo(edges[i].right);
        }

        batchPath.close();
        canvas->drawPath(batchPath, batchPaint);
    }

    // ===== TEXTURED END CAPS =====
    // Draw rounded caps at start and end with the crayon texture
    // This makes the tip look curved and textured, not a straight line

    if (smoothedPoints.size() >= 2) {
        // END CAP - semicircle at the last point
        size_t lastIdx = smoothedPoints.size() - 1;
        const Point& endPt = smoothedPoints[lastIdx];
        float endHalfWidth = endPt.calculatedWidth / 2.0f;

        // Direction at end (from previous to current)
        float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
        float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
        float endLen = std::sqrt(endDx * endDx + endDy * endDy);
        if (endLen > 0.001f) { endDx /= endLen; endDy /= endLen; }

        // Create shader for end cap
        float endAngle = std::atan2(endDy, endDx);
        sk_sp<SkShader> endCapShader = createCrayonShader(baseColor, endPt.pressure, strokeWidth, endAngle);

        SkPaint endCapPaint;
        endCapPaint.setShader(endCapShader);
        endCapPaint.setStyle(SkPaint::kFill_Style);
        endCapPaint.setAntiAlias(false);
        endCapPaint.setAlpha(255);

        // Build semicircle path for end cap
        // Arc from left edge to right edge, curving outward in stroke direction
        SkPath endCapPath;
        float perpX = -endDy;
        float perpY = endDx;

        // Start at left edge point
        SkPoint endLeft = SkPoint::Make(endPt.x + perpX * endHalfWidth, endPt.y + perpY * endHalfWidth);
        SkPoint endRight = SkPoint::Make(endPt.x - perpX * endHalfWidth, endPt.y - perpY * endHalfWidth);

        endCapPath.moveTo(endLeft);

        // Arc sweep angle direction (from left to right via the tip)
        float endArcAngle = std::atan2(perpY, perpX) * 180.0f / 3.14159265f;
        SkRect endCapRect = SkRect::MakeXYWH(
            endPt.x - endHalfWidth,
            endPt.y - endHalfWidth,
            endHalfWidth * 2.0f,
            endHalfWidth * 2.0f);
        endCapPath.arcTo(endCapRect, endArcAngle, -180.0f, false);
        endCapPath.close();

        canvas->drawPath(endCapPath, endCapPaint);

        // START CAP - semicircle at the first point
        const Point& startPt = smoothedPoints[0];
        float startHalfWidth = startPt.calculatedWidth / 2.0f;

        // Direction at start (from current to next)
        float startDx = smoothedPoints[1].x - startPt.x;
        float startDy = smoothedPoints[1].y - startPt.y;
        float startLen = std::sqrt(startDx * startDx + startDy * startDy);
        if (startLen > 0.001f) { startDx /= startLen; startDy /= startLen; }

        // Create shader for start cap
        float startAngle = std::atan2(startDy, startDx);
        sk_sp<SkShader> startCapShader = createCrayonShader(baseColor, startPt.pressure, strokeWidth, startAngle);

        SkPaint startCapPaint;
        startCapPaint.setShader(startCapShader);
        startCapPaint.setStyle(SkPaint::kFill_Style);
        startCapPaint.setAntiAlias(false);
        startCapPaint.setAlpha(255);

        // Build semicircle path for start cap (curves backward)
        SkPath startCapPath;
        float startPerpX = -startDy;
        float startPerpY = startDx;

        SkPoint startRight = SkPoint::Make(startPt.x - startPerpX * startHalfWidth, startPt.y - startPerpY * startHalfWidth);

        startCapPath.moveTo(startRight);

        float startArcAngle = std::atan2(-startPerpY, -startPerpX) * 180.0f / 3.14159265f;
        SkRect startCapRect = SkRect::MakeXYWH(
            startPt.x - startHalfWidth,
            startPt.y - startHalfWidth,
            startHalfWidth * 2.0f,
            startHalfWidth * 2.0f);
        startCapPath.arcTo(startCapRect, startArcAngle, -180.0f, false);
        startCapPath.close();

        canvas->drawPath(startCapPath, startCapPaint);
    }
}

void PathRenderer::drawCrayonEndCaps(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    SkColor baseColor = basePaint.getColor();
    float strokeWidth = basePaint.getStrokeWidth();

    // Generate smoothed points
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    if (smoothedPoints.size() < 2) return;

    // ===== END CAP - semicircle at the last point =====
    size_t lastIdx = smoothedPoints.size() - 1;
    const Point& endPt = smoothedPoints[lastIdx];
    float endHalfWidth = endPt.calculatedWidth / 2.0f;

    float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
    float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
    float endLen = std::sqrt(endDx * endDx + endDy * endDy);
    if (endLen > 0.001f) { endDx /= endLen; endDy /= endLen; }

    float endAngle = std::atan2(endDy, endDx);
    sk_sp<SkShader> endCapShader = createCrayonShader(baseColor, endPt.pressure, strokeWidth, endAngle);

    SkPaint endCapPaint;
    endCapPaint.setShader(endCapShader);
    endCapPaint.setStyle(SkPaint::kFill_Style);
    endCapPaint.setAntiAlias(false);
    endCapPaint.setAlpha(255);

    SkPath endCapPath;
    float perpX = -endDy;
    float perpY = endDx;

    SkPoint endLeft = SkPoint::Make(endPt.x + perpX * endHalfWidth, endPt.y + perpY * endHalfWidth);
    endCapPath.moveTo(endLeft);

    float endArcAngle = std::atan2(perpY, perpX) * 180.0f / 3.14159265f;
    SkRect endCapRect = SkRect::MakeXYWH(
        endPt.x - endHalfWidth,
        endPt.y - endHalfWidth,
        endHalfWidth * 2.0f,
        endHalfWidth * 2.0f);
    endCapPath.arcTo(endCapRect, endArcAngle, -180.0f, false);
    endCapPath.close();

    canvas->drawPath(endCapPath, endCapPaint);

    // ===== START CAP - semicircle at the first point =====
    const Point& startPt = smoothedPoints[0];
    float startHalfWidth = startPt.calculatedWidth / 2.0f;

    float startDx = smoothedPoints[1].x - startPt.x;
    float startDy = smoothedPoints[1].y - startPt.y;
    float startLen = std::sqrt(startDx * startDx + startDy * startDy);
    if (startLen > 0.001f) { startDx /= startLen; startDy /= startLen; }

    float startAngle = std::atan2(startDy, startDx);
    sk_sp<SkShader> startCapShader = createCrayonShader(baseColor, startPt.pressure, strokeWidth, startAngle);

    SkPaint startCapPaint;
    startCapPaint.setShader(startCapShader);
    startCapPaint.setStyle(SkPaint::kFill_Style);
    startCapPaint.setAntiAlias(false);
    startCapPaint.setAlpha(255);

    SkPath startCapPath;
    float startPerpX = -startDy;
    float startPerpY = startDx;

    SkPoint startRight = SkPoint::Make(startPt.x - startPerpX * startHalfWidth, startPt.y - startPerpY * startHalfWidth);
    startCapPath.moveTo(startRight);

    float startArcAngle = std::atan2(-startPerpY, -startPerpX) * 180.0f / 3.14159265f;
    SkRect startCapRect = SkRect::MakeXYWH(
        startPt.x - startHalfWidth,
        startPt.y - startHalfWidth,
        startHalfWidth * 2.0f,
        startHalfWidth * 2.0f);
    startCapPath.arcTo(startCapRect, startArcAngle, -180.0f, false);
    startCapPath.close();

    canvas->drawPath(startCapPath, startCapPaint);
}

void PathRenderer::drawCrayonStartCap(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    SkColor baseColor = basePaint.getColor();
    float strokeWidth = basePaint.getStrokeWidth();

    // Generate smoothed points
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    if (smoothedPoints.size() < 2) return;

    // START CAP - semicircle at the first point
    const Point& startPt = smoothedPoints[0];
    float startHalfWidth = startPt.calculatedWidth / 2.0f;

    float startDx = smoothedPoints[1].x - startPt.x;
    float startDy = smoothedPoints[1].y - startPt.y;
    float startLen = std::sqrt(startDx * startDx + startDy * startDy);
    if (startLen > 0.001f) { startDx /= startLen; startDy /= startLen; }

    float startAngle = std::atan2(startDy, startDx);
    sk_sp<SkShader> startCapShader = createCrayonShader(baseColor, startPt.pressure, strokeWidth, startAngle);

    SkPaint startCapPaint;
    startCapPaint.setShader(startCapShader);
    startCapPaint.setStyle(SkPaint::kFill_Style);
    startCapPaint.setAntiAlias(false);
    startCapPaint.setAlpha(255);

    SkPath startCapPath;
    float startPerpX = -startDy;
    float startPerpY = startDx;

    SkPoint startRight = SkPoint::Make(startPt.x - startPerpX * startHalfWidth, startPt.y - startPerpY * startHalfWidth);
    startCapPath.moveTo(startRight);

    float startArcAngle = std::atan2(-startPerpY, -startPerpX) * 180.0f / 3.14159265f;
    SkRect startCapRect = SkRect::MakeXYWH(
        startPt.x - startHalfWidth,
        startPt.y - startHalfWidth,
        startHalfWidth * 2.0f,
        startHalfWidth * 2.0f);
    startCapPath.arcTo(startCapRect, startArcAngle, -180.0f, false);
    startCapPath.close();

    canvas->drawPath(startCapPath, startCapPaint);
}

void PathRenderer::drawCrayonEndCap(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    SkColor baseColor = basePaint.getColor();
    float strokeWidth = basePaint.getStrokeWidth();

    // Generate smoothed points
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    if (smoothedPoints.size() < 2) return;

    // END CAP - semicircle at the last point
    size_t lastIdx = smoothedPoints.size() - 1;
    const Point& endPt = smoothedPoints[lastIdx];
    float endHalfWidth = endPt.calculatedWidth / 2.0f;

    float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
    float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
    float endLen = std::sqrt(endDx * endDx + endDy * endDy);
    if (endLen > 0.001f) { endDx /= endLen; endDy /= endLen; }

    float endAngle = std::atan2(endDy, endDx);
    sk_sp<SkShader> endCapShader = createCrayonShader(baseColor, endPt.pressure, strokeWidth, endAngle);

    SkPaint endCapPaint;
    endCapPaint.setShader(endCapShader);
    endCapPaint.setStyle(SkPaint::kFill_Style);
    endCapPaint.setAntiAlias(false);
    endCapPaint.setAlpha(255);

    SkPath endCapPath;
    float perpX = -endDy;
    float perpY = endDx;

    SkPoint endLeft = SkPoint::Make(endPt.x + perpX * endHalfWidth, endPt.y + perpY * endHalfWidth);
    endCapPath.moveTo(endLeft);

    float endArcAngle = std::atan2(perpY, perpX) * 180.0f / 3.14159265f;
    SkRect endCapRect = SkRect::MakeXYWH(
        endPt.x - endHalfWidth,
        endPt.y - endHalfWidth,
        endHalfWidth * 2.0f,
        endHalfWidth * 2.0f);
    endCapPath.arcTo(endCapRect, endArcAngle, -180.0f, false);
    endCapPath.close();

    canvas->drawPath(endCapPath, endCapPaint);
}

// ===== SHADER CACHING IMPLEMENTATION =====
// OPTIMIZED: Reduced quantization levels for 5x higher cache hit rate
// 8 pressure x 8 angle = 64 slots vs 320 before (perceptually equivalent for noise textures)

uint64_t PathRenderer::getShaderCacheKey(SkColor color, float pressure, float angle) {
    // Quantize pressure to 8 levels (3 bits) - sufficient for noise texture variance
    int pressureLevel = static_cast<int>(std::max(0.0f, std::min(1.0f, pressure)) * 7.99f);

    // Quantize angle to 8 buckets (3 bits) - 45 degrees each
    // Normalize angle to 0-2PI range first
    float normalizedAngle = angle;
    while (normalizedAngle < 0) normalizedAngle += 2.0f * 3.14159265f;
    while (normalizedAngle >= 2.0f * 3.14159265f) normalizedAngle -= 2.0f * 3.14159265f;
    int angleBucket = static_cast<int>(normalizedAngle / (2.0f * 3.14159265f) * 8) % 8;

    // Combine: color (32 bits) | pressure (3 bits) | angle (3 bits)
    return (static_cast<uint64_t>(color) << 6) |
           (static_cast<uint64_t>(pressureLevel) << 3) |
           static_cast<uint64_t>(angleBucket);
}

void PathRenderer::clearShaderCache() {
    shaderCache_.clear();
}

IncrementalResult PathRenderer::drawCrayonPathIncremental(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    const SkPoint& prevLeftEdge,
    const SkPoint& prevRightEdge,
    bool isFirstSegment
) {
    IncrementalResult result = {};
    if (points.empty() || points.size() < 2 || !canvas) return result;

    SkColor baseColor = basePaint.getColor();
    float strokeWidth = basePaint.getStrokeWidth();

    // Generate smoothed points using helper
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(
        points,
        smoothedPoints,
        true,
        LIVE_SEGMENTS_PER_SPAN,
        LIVE_SPLINE_TENSION
    );

    // Build edges using helper
    std::vector<EdgePoint> edges;
    if (!buildEdgePoints(smoothedPoints, edges)) return result;

    // Connect to previous segment (replace first edge with previous edge)
    if (!isFirstSegment) {
        edges[0].left = prevLeftEdge;
        edges[0].right = prevRightEdge;
    }

    // Batch rendering (same as drawCrayonPath)
    constexpr int BATCH_SIZE = 24;
    for (size_t batchStart = 0; batchStart < edges.size() - 1; batchStart += BATCH_SIZE) {
        size_t batchEnd = std::min(batchStart + BATCH_SIZE, edges.size() - 1);

        float batchPressure = 0.0f;
        float batchDx = 0.0f, batchDy = 0.0f;
        for (size_t i = batchStart; i <= batchEnd; i++) {
            batchPressure += edges[i].pressure;
            batchDx += std::cos(edges[i].angle);
            batchDy += std::sin(edges[i].angle);
        }
        batchPressure = std::max(0.1f, std::min(1.0f, batchPressure / (batchEnd - batchStart + 1)));
        float batchAngle = std::atan2(batchDy, batchDx);

        sk_sp<SkShader> batchShader = createCrayonShader(baseColor, batchPressure, strokeWidth, batchAngle);

        SkPaint batchPaint;
        batchPaint.setShader(batchShader);
        batchPaint.setStyle(SkPaint::kFill_Style);
        batchPaint.setAntiAlias(false);
        batchPaint.setAlpha(255);

        SkPath batchPath;
        batchPath.moveTo(edges[batchStart].left);
        for (size_t i = batchStart + 1; i <= batchEnd; i++) {
            batchPath.lineTo(edges[i].left);
        }
        for (int i = static_cast<int>(batchEnd); i >= static_cast<int>(batchStart); i--) {
            batchPath.lineTo(edges[i].right);
        }
        batchPath.close();
        canvas->drawPath(batchPath, batchPaint);
    }

    // Return last edge for next segment connection
    result.lastLeftEdge = edges.back().left;
    result.lastRightEdge = edges.back().right;
    result.lastPressure = edges.back().pressure;
    result.lastAngle = edges.back().angle;
    result.smoothedPointsRendered = smoothedPoints.size();

    return result;
}

void PathRenderer::drawCrayonPathTail(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    const SkPoint& prevLeftEdge,
    const SkPoint& prevRightEdge,
    bool hasPreviousEdge
) {
    // Tail is the recent points showing the current pen position
    // We render it with an end cap so the tip looks rounded while drawing
    if (points.size() < 2) return;

    // First, draw the body using incremental rendering
    IncrementalResult result = drawCrayonPathIncremental(
        canvas, points, basePaint, prevLeftEdge, prevRightEdge, !hasPreviousEdge);

    // Add end cap to the tail tip (so user sees rounded tip while drawing)
    SkColor baseColor = basePaint.getColor();
    float strokeWidth = basePaint.getStrokeWidth();

    // Generate smoothed points to get the last point position
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    if (smoothedPoints.size() >= 2) {
        // Draw textured end cap at the tip
        size_t lastIdx = smoothedPoints.size() - 1;
        const Point& endPt = smoothedPoints[lastIdx];
        float endHalfWidth = endPt.calculatedWidth / 2.0f;

        float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
        float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
        float endLen = std::sqrt(endDx * endDx + endDy * endDy);
        if (endLen > 0.001f) { endDx /= endLen; endDy /= endLen; }

        float endAngle = std::atan2(endDy, endDx);
        sk_sp<SkShader> endCapShader = createCrayonShader(baseColor, endPt.pressure, strokeWidth, endAngle);

        SkPaint endCapPaint;
        endCapPaint.setShader(endCapShader);
        endCapPaint.setStyle(SkPaint::kFill_Style);
        endCapPaint.setAntiAlias(false);
        endCapPaint.setAlpha(255);

        SkPath endCapPath;
        float perpX = -endDy;
        float perpY = endDx;

        SkPoint endLeft = SkPoint::Make(endPt.x + perpX * endHalfWidth, endPt.y + perpY * endHalfWidth);
        endCapPath.moveTo(endLeft);

        float endArcAngle = std::atan2(perpY, perpX) * 180.0f / 3.14159265f;
        SkRect endCapRect = SkRect::MakeXYWH(
            endPt.x - endHalfWidth,
            endPt.y - endHalfWidth,
            endHalfWidth * 2.0f,
            endHalfWidth * 2.0f);
        endCapPath.arcTo(endCapRect, endArcAngle, -180.0f, false);
        endCapPath.close();

        canvas->drawPath(endCapPath, endCapPaint);
    }
}

} // namespace nativedrawing
