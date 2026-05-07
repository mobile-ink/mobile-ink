#include "PathRenderer.h"
#include <cmath>
#include <include/effects/SkPerlinNoiseShader.h>
#include <include/core/SkColorFilter.h>
#include <include/effects/SkColorMatrix.h>

namespace nativedrawing {

namespace {

constexpr float kPerpendicularAltitude = 1.5707963f;

float clampUnit(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float calculateLegacyWidth(float pressure, float altitude, float baseWidth) {
    float pressureFactor = 0.5f + pressure;

    float altitudeNormalized = altitude / kPerpendicularAltitude;
    float tiltFactor = 1.0f + (1.0f - altitudeNormalized) * 0.4f;

    float finalFactor = pressureFactor * tiltFactor;
    finalFactor = std::max(0.3f, std::min(2.0f, finalFactor));

    return baseWidth * finalFactor;
}

float calculateEnhancedPenWidth(float pressure, float altitude, float baseWidth) {
    const float clampedPressure = clampUnit(pressure);
    const float altitudeNormalized = clampUnit(altitude / kPerpendicularAltitude);

    // Bias the response toward a wider dynamic range without making medium pressure
    // feel like a brush. Low pressure stays visibly thinner, heavy pressure opens up.
    const float pressureFactor = 0.18f + (std::pow(clampedPressure, 0.74f) * 1.62f);

    // Keep tilt secondary to pressure so pen writing stays natural rather than
    // turning into a calligraphy tool.
    const float tiltFactor = 1.0f + (std::pow(1.0f - altitudeNormalized, 1.12f) * 0.34f);

    float finalFactor = pressureFactor * tiltFactor;
    finalFactor = std::max(0.18f, std::min(2.35f, finalFactor));

    return baseWidth * finalFactor;
}

}  // namespace

void PathRenderer::drawVariableWidthPath(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    bool applyPressureAlpha
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    // Generate smoothed points using helper
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, false);

    // Build edges using helper
    std::vector<EdgePoint> edges;
    if (!buildEdgePoints(smoothedPoints, edges)) return;

    // Create path from edges
    SkPath strokePath;
    std::vector<SkPoint> leftEdge, rightEdge;
    for (const auto& ep : edges) {
        leftEdge.push_back(ep.left);
        rightEdge.push_back(ep.right);
    }

    // Create paint for filled stroke
    SkPaint fillPaint = basePaint;
    fillPaint.setStyle(SkPaint::kFill_Style);

    // Modulate opacity based on average pressure for pen/pencil expressiveness
    // Skip for: eraser (must be opaque), multiply blend (highlighter), applyPressureAlpha=false
    if (applyPressureAlpha && !points.empty() &&
        fillPaint.asBlendMode() != SkBlendMode::kDstOut &&
        fillPaint.asBlendMode() != SkBlendMode::kMultiply) {
        float avgPressure = 0.0f;
        for (const auto& pt : points) {
            avgPressure += pt.pressure;
        }
        avgPressure /= points.size();

        // Subtle pressure-based alpha (85%-100%)
        uint8_t baseAlpha = fillPaint.getAlpha();
        float pressureAlphaMod = 0.85f + (avgPressure * 0.15f);
        fillPaint.setAlpha(static_cast<uint8_t>(baseAlpha * pressureAlphaMod));
    }

    // For multiply blend (highlighter), draw individual segments so overlaps darken properly
    // For other blend modes, draw as single path for performance
    bool isMultiplyBlend = (fillPaint.asBlendMode() == SkBlendMode::kMultiply);

    if (isMultiplyBlend && leftEdge.size() > 1) {
        // Draw as individual quad segments - each segment composites separately
        // This makes self-overlapping strokes darken at intersections
        for (size_t i = 0; i < leftEdge.size() - 1; i++) {
            SkPath segmentPath;
            segmentPath.moveTo(leftEdge[i]);
            segmentPath.lineTo(leftEdge[i + 1]);
            segmentPath.lineTo(rightEdge[i + 1]);
            segmentPath.lineTo(rightEdge[i]);
            segmentPath.close();
            canvas->drawPath(segmentPath, fillPaint);
        }
    } else if (!leftEdge.empty() && !rightEdge.empty()) {
        // Build single filled path with rounded caps for non-multiply blends
        strokePath.moveTo(leftEdge[0]);

        // Draw along left edge
        for (size_t i = 1; i < leftEdge.size(); i++) {
            strokePath.lineTo(leftEdge[i]);
        }

        // End cap: semicircle arc from leftEdge[last] to rightEdge[last]
        if (smoothedPoints.size() >= 2) {
            size_t lastIdx = smoothedPoints.size() - 1;
            const Point& endPt = smoothedPoints[lastIdx];
            float endHalfWidth = endPt.calculatedWidth / 2.0f;

            // Calculate direction at end point (from previous to current)
            float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
            float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
            float endLen = std::sqrt(endDx * endDx + endDy * endDy);
            if (endLen > 0.001f) {
                endDx /= endLen;
                endDy /= endLen;
            }

            // Perpendicular direction (matches how edges were computed)
            float endPerpX = -endDy;
            float endPerpY = endDx;

            // Arc start angle: direction from center to leftEdge (perpendicular direction)
            float endStartAngle = std::atan2(endPerpY, endPerpX) * 180.0f / 3.14159265f;

            SkRect endCapRect = SkRect::MakeXYWH(
                endPt.x - endHalfWidth,
                endPt.y - endHalfWidth,
                endHalfWidth * 2.0f,
                endHalfWidth * 2.0f);
            strokePath.arcTo(endCapRect, endStartAngle, -180.0f, false);
        }

        // Draw back along right edge (reversed, skip last point since arc connected it)
        for (int i = static_cast<int>(rightEdge.size()) - 2; i >= 0; i--) {
            strokePath.lineTo(rightEdge[i]);
        }

        // Start cap: semicircle arc from rightEdge[0] to leftEdge[0]
        if (smoothedPoints.size() >= 2) {
            const Point& startPt = smoothedPoints[0];
            float startHalfWidth = startPt.calculatedWidth / 2.0f;

            // Calculate direction at start point (from current to next)
            float startDx = smoothedPoints[1].x - startPt.x;
            float startDy = smoothedPoints[1].y - startPt.y;
            float startLen = std::sqrt(startDx * startDx + startDy * startDy);
            if (startLen > 0.001f) {
                startDx /= startLen;
                startDy /= startLen;
            }

            // Perpendicular direction
            float startPerpX = -startDy;
            float startPerpY = startDx;

            // Arc start angle: direction from center to rightEdge (opposite of perpendicular)
            float startStartAngle = std::atan2(-startPerpY, -startPerpX) * 180.0f / 3.14159265f;

            SkRect startCapRect = SkRect::MakeXYWH(
                startPt.x - startHalfWidth,
                startPt.y - startHalfWidth,
                startHalfWidth * 2.0f,
                startHalfWidth * 2.0f);
            strokePath.arcTo(startCapRect, startStartAngle, -180.0f, false);
        }

        strokePath.close();
        canvas->drawPath(strokePath, fillPaint);
    }
}

float PathRenderer::calculateWidth(
    float pressure,
    float altitude,
    float baseWidth,
    const std::string& toolType,
    bool useEnhancedPenProfile
) {
    if (toolType == "pen" && useEnhancedPenProfile) {
        return calculateEnhancedPenWidth(pressure, altitude, baseWidth);
    }

    return calculateLegacyWidth(pressure, altitude, baseWidth);
}

void PathRenderer::smoothPath(const std::vector<Point>& points, SkPath& path) {
    if (points.empty()) return;

    // Reset path to clear any old data before building new path
    path.reset();
    path.moveTo(points[0].x, points[0].y);

    if (points.size() == 1) {
        // Single point - draw tiny circle/dot
        path.lineTo(points[0].x + 0.1f, points[0].y + 0.1f);
        return;
    }

    if (points.size() == 2) {
        // Two points - straight line
        path.lineTo(points[1].x, points[1].y);
        return;
    }

    // Use Catmull-Rom splines for ultra-smooth curves
    // This creates curves that pass through all points with smooth tangents
    for (size_t i = 0; i < points.size() - 1; i++) {
        // Get 4 control points for Catmull-Rom (p0, p1, p2, p3)
        // We want to draw curve from p1 to p2
        Point p0, p1, p2, p3;

        p1 = points[i];
        p2 = points[i + 1];

        // For first segment, duplicate first point
        if (i == 0) {
            p0 = p1;
        } else {
            p0 = points[i - 1];
        }

        // For last segment, duplicate last point
        if (i + 2 >= points.size()) {
            p3 = p2;
        } else {
            p3 = points[i + 2];
        }

        // Calculate control points for cubic bezier from Catmull-Rom
        float c1x = p1.x + (p2.x - p0.x) / 6.0f * SPLINE_TENSION;
        float c1y = p1.y + (p2.y - p0.y) / 6.0f * SPLINE_TENSION;

        float c2x = p2.x - (p3.x - p1.x) / 6.0f * SPLINE_TENSION;
        float c2y = p2.y - (p3.y - p1.y) / 6.0f * SPLINE_TENSION;

        // Draw cubic bezier curve from p1 to p2 with control points c1 and c2
        path.cubicTo(c1x, c1y, c2x, c2y, p2.x, p2.y);
    }
}

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

// ===== SPLINE INTERPOLATION HELPERS =====

void PathRenderer::interpolateSplinePoints(
    const std::vector<Point>& points,
    std::vector<Point>& smoothedPoints,
    bool interpolatePressure,
    int segmentsPerSpan,
    float splineTension
) {
    if (points.empty()) return;
    smoothedPoints.reserve((points.size() - 1) * segmentsPerSpan + 1);

    for (size_t i = 0; i < points.size() - 1; i++) {
        Point p0, p1, p2, p3;
        p1 = points[i];
        p2 = points[i + 1];

        if (i == 0) { p0 = p1; }
        else { p0 = points[i - 1]; }

        if (i + 2 >= points.size()) { p3 = p2; }
        else { p3 = points[i + 2]; }

        for (int seg = 0; seg < segmentsPerSpan; seg++) {
            float t = static_cast<float>(seg) / segmentsPerSpan;
            float t2 = t * t;
            float t3 = t2 * t;

            float q0 = -splineTension * t3 + 2.0f * splineTension * t2 - splineTension * t;
            float q1 = (2.0f - splineTension) * t3 + (splineTension - 3.0f) * t2 + 1.0f;
            float q2 = (splineTension - 2.0f) * t3 + (3.0f - 2.0f * splineTension) * t2 + splineTension * t;
            float q3 = splineTension * t3 - splineTension * t2;

            Point interpolated;
            interpolated.x = q0 * p0.x + q1 * p1.x + q2 * p2.x + q3 * p3.x;
            interpolated.y = q0 * p0.y + q1 * p1.y + q2 * p2.y + q3 * p3.y;
            interpolated.calculatedWidth = q0 * p0.calculatedWidth + q1 * p1.calculatedWidth +
                                          q2 * p2.calculatedWidth + q3 * p3.calculatedWidth;

            if (interpolatePressure) {
                interpolated.pressure = q0 * p0.pressure + q1 * p1.pressure +
                                       q2 * p2.pressure + q3 * p3.pressure;
                interpolated.pressure = std::max(0.1f, std::min(1.0f, interpolated.pressure));
            }

            // Interpolate timestamp for velocity calculation (critical for calligraphy)
            interpolated.timestamp = static_cast<long>(
                q0 * p0.timestamp + q1 * p1.timestamp + q2 * p2.timestamp + q3 * p3.timestamp
            );

            smoothedPoints.push_back(interpolated);
        }
    }
    smoothedPoints.push_back(points.back());
}

bool PathRenderer::buildEdgePoints(
    const std::vector<Point>& smoothedPoints,
    std::vector<EdgePoint>& edges
) {
    if (smoothedPoints.size() < 2) return false;
    edges.reserve(smoothedPoints.size());

    for (size_t i = 0; i < smoothedPoints.size(); i++) {
        const Point& p = smoothedPoints[i];
        float halfWidth = p.calculatedWidth / 2.0f;

        float dx = 0.0f, dy = 0.0f;
        if (i < smoothedPoints.size() - 1) {
            dx = smoothedPoints[i + 1].x - p.x;
            dy = smoothedPoints[i + 1].y - p.y;
        } else if (i > 0) {
            dx = p.x - smoothedPoints[i - 1].x;
            dy = p.y - smoothedPoints[i - 1].y;
        }

        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.001f) {
            dx /= len;
            dy /= len;
        }

        float perpX = -dy;
        float perpY = dx;

        EdgePoint ep;
        ep.left = SkPoint::Make(p.x + perpX * halfWidth, p.y + perpY * halfWidth);
        ep.right = SkPoint::Make(p.x - perpX * halfWidth, p.y - perpY * halfWidth);
        ep.pressure = p.pressure;
        ep.angle = std::atan2(dy, dx);
        edges.push_back(ep);
    }
    return true;
}

// ===== INCREMENTAL RENDERING METHODS =====

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

IncrementalResult PathRenderer::drawVariableWidthPathIncremental(
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

    // Connect to previous segment
    if (!isFirstSegment) {
        edges[0].left = prevLeftEdge;
        edges[0].right = prevRightEdge;
    }

    // Build unified path (for non-crayon tools)
    SkPath path;
    path.moveTo(edges[0].left);
    for (size_t i = 1; i < edges.size(); i++) {
        path.lineTo(edges[i].left);
    }
    for (int i = static_cast<int>(edges.size()) - 1; i >= 0; i--) {
        path.lineTo(edges[i].right);
    }
    path.close();

    SkPaint fillPaint;
    fillPaint.setColor(baseColor);
    fillPaint.setStyle(SkPaint::kFill_Style);
    fillPaint.setAntiAlias(true);
    canvas->drawPath(path, fillPaint);

    // Return last edge
    result.lastLeftEdge = edges.back().left;
    result.lastRightEdge = edges.back().right;
    result.lastPressure = edges.back().pressure;
    result.lastAngle = 0.0f;
    result.smoothedPointsRendered = smoothedPoints.size();

    return result;
}

void PathRenderer::drawVariableWidthPathTail(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    const SkPoint& prevLeftEdge,
    const SkPoint& prevRightEdge,
    bool hasPreviousEdge
) {
    if (points.size() < 2) return;
    drawVariableWidthPathIncremental(canvas, points, basePaint, prevLeftEdge, prevRightEdge, !hasPreviousEdge);
}

void PathRenderer::drawVariableWidthEndCaps(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    // Generate smoothed points
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, false);

    if (smoothedPoints.size() < 2) return;

    SkPaint fillPaint = basePaint;
    fillPaint.setStyle(SkPaint::kFill_Style);
    fillPaint.setAntiAlias(true);

    // ===== END CAP - semicircle at the last point =====
    size_t lastIdx = smoothedPoints.size() - 1;
    const Point& endPt = smoothedPoints[lastIdx];
    float endHalfWidth = endPt.calculatedWidth / 2.0f;

    float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
    float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
    float endLen = std::sqrt(endDx * endDx + endDy * endDy);
    if (endLen > 0.001f) { endDx /= endLen; endDy /= endLen; }

    float endPerpX = -endDy;
    float endPerpY = endDx;

    SkPath endCapPath;
    SkPoint endLeft = SkPoint::Make(endPt.x + endPerpX * endHalfWidth, endPt.y + endPerpY * endHalfWidth);
    endCapPath.moveTo(endLeft);

    float endArcAngle = std::atan2(endPerpY, endPerpX) * 180.0f / 3.14159265f;
    SkRect endCapRect = SkRect::MakeXYWH(
        endPt.x - endHalfWidth,
        endPt.y - endHalfWidth,
        endHalfWidth * 2.0f,
        endHalfWidth * 2.0f);
    endCapPath.arcTo(endCapRect, endArcAngle, -180.0f, false);
    endCapPath.close();

    canvas->drawPath(endCapPath, fillPaint);

    // ===== START CAP - semicircle at the first point =====
    const Point& startPt = smoothedPoints[0];
    float startHalfWidth = startPt.calculatedWidth / 2.0f;

    float startDx = smoothedPoints[1].x - startPt.x;
    float startDy = smoothedPoints[1].y - startPt.y;
    float startLen = std::sqrt(startDx * startDx + startDy * startDy);
    if (startLen > 0.001f) { startDx /= startLen; startDy /= startLen; }

    float startPerpX = -startDy;
    float startPerpY = startDx;

    SkPath startCapPath;
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

    canvas->drawPath(startCapPath, fillPaint);
}

void PathRenderer::drawVariableWidthStartCap(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(
        points,
        smoothedPoints,
        false,
        LIVE_SEGMENTS_PER_SPAN,
        LIVE_SPLINE_TENSION
    );

    if (smoothedPoints.size() < 2) return;

    SkPaint fillPaint = basePaint;
    fillPaint.setStyle(SkPaint::kFill_Style);
    fillPaint.setAntiAlias(true);

    const Point& startPt = smoothedPoints[0];
    float startHalfWidth = startPt.calculatedWidth / 2.0f;

    float startDx = smoothedPoints[1].x - startPt.x;
    float startDy = smoothedPoints[1].y - startPt.y;
    float startLen = std::sqrt(startDx * startDx + startDy * startDy);
    if (startLen > 0.001f) { startDx /= startLen; startDy /= startLen; }

    float startPerpX = -startDy;
    float startPerpY = startDx;

    SkPath startCapPath;
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

    canvas->drawPath(startCapPath, fillPaint);
}

void PathRenderer::drawVariableWidthEndCap(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(
        points,
        smoothedPoints,
        false,
        LIVE_SEGMENTS_PER_SPAN,
        LIVE_SPLINE_TENSION
    );

    if (smoothedPoints.size() < 2) return;

    SkPaint fillPaint = basePaint;
    fillPaint.setStyle(SkPaint::kFill_Style);
    fillPaint.setAntiAlias(true);

    size_t lastIdx = smoothedPoints.size() - 1;
    const Point& endPt = smoothedPoints[lastIdx];
    float endHalfWidth = endPt.calculatedWidth / 2.0f;

    float endDx = endPt.x - smoothedPoints[lastIdx - 1].x;
    float endDy = endPt.y - smoothedPoints[lastIdx - 1].y;
    float endLen = std::sqrt(endDx * endDx + endDy * endDy);
    if (endLen > 0.001f) { endDx /= endLen; endDy /= endLen; }

    float endPerpX = -endDy;
    float endPerpY = endDx;

    SkPath endCapPath;
    SkPoint endLeft = SkPoint::Make(endPt.x + endPerpX * endHalfWidth, endPt.y + endPerpY * endHalfWidth);
    endCapPath.moveTo(endLeft);

    float endArcAngle = std::atan2(endPerpY, endPerpX) * 180.0f / 3.14159265f;
    SkRect endCapRect = SkRect::MakeXYWH(
        endPt.x - endHalfWidth,
        endPt.y - endHalfWidth,
        endHalfWidth * 2.0f,
        endHalfWidth * 2.0f);
    endCapPath.arcTo(endCapRect, endArcAngle, -180.0f, false);
    endCapPath.close();

    canvas->drawPath(endCapPath, fillPaint);
}

// ===== CALLIGRAPHY BRUSH IMPLEMENTATION =====
// Pointed/flex nib style: thin upstrokes, thick downstrokes

float PathRenderer::calculateVelocity(const Point& current, const Point& previous) {
    float dx = current.x - previous.x;
    float dy = current.y - previous.y;
    float distance = std::sqrt(dx * dx + dy * dy);

    // Timestamp difference in milliseconds
    long timeDelta = current.timestamp - previous.timestamp;
    if (timeDelta <= 0) {
        // Fallback: estimate based on typical touch sampling rate (120Hz = 8.3ms)
        timeDelta = 8;
    }

    // Velocity in pixels per second
    return (distance / static_cast<float>(timeDelta)) * 1000.0f;
}

float PathRenderer::calculateVerticalDirection(const Point& current, const Point& previous) {
    float dx = current.x - previous.x;
    float dy = current.y - previous.y;
    float distance = std::sqrt(dx * dx + dy * dy);

    if (distance < 0.001f) return 0.0f;  // No movement

    // Normalize dy to get vertical component of direction
    // Positive = downstroke (moving down), negative = upstroke (moving up)
    return dy / distance;
}

float PathRenderer::calculateCalligraphyWidth(
    const Point& current,
    const Point& previous,
    float baseWidth,
    float velocity,
    float verticalDirection
) {
    // === DIRECTION FACTOR ===
    // Downstrokes (positive Y direction) are thick, upstrokes are thin
    // verticalDirection: -1.0 (pure upstroke) to +1.0 (pure downstroke)
    // Map to width multiplier: 0.3x (thin upstroke) to 1.5x (thick downstroke)
    float directionFactor = 0.9f + (verticalDirection * 0.6f);  // 0.3 to 1.5

    // === VELOCITY FACTOR ===
    // Fast strokes = thinner, slow strokes = thicker
    // Velocity typically ranges from 0 (stationary) to 3000+ px/sec
    // Normalize to 0-1 range where 0 = fast (>2000px/s), 1 = slow (<200px/s)
    float velocityNormalized = 1.0f - std::min(1.0f, velocity / 2000.0f);
    float velocityFactor = 0.6f + (velocityNormalized * 0.4f);  // 0.6x to 1.0x

    // === PRESSURE FACTOR ===
    // Pressure amplifies the direction effect
    // Light pressure = less width variation, heavy pressure = more dramatic
    float pressure = current.pressure;
    float pressureFactor = 0.5f + (pressure * 0.5f);  // 0.5x to 1.0x base

    // === COMBINE FACTORS ===
    // Direction is primary, velocity and pressure modulate
    float combinedFactor = directionFactor * velocityFactor * pressureFactor;

    // Clamp to reasonable range (0.15x to 2.0x of base width)
    combinedFactor = std::max(0.15f, std::min(2.0f, combinedFactor));

    return baseWidth * combinedFactor;
}

bool PathRenderer::buildCalligraphyEdgePoints(
    const std::vector<Point>& smoothedPoints,
    std::vector<EdgePoint>& edges,
    float baseWidth,
    float initialHalfWidth,
    float* outFinalHalfWidth
) {
    if (smoothedPoints.size() < 2) return false;
    edges.reserve(smoothedPoints.size());

    float prevVelocity = 0.0f;
    float prevDirection = 0.0f;
    float prevHalfWidth = (initialHalfWidth > 0) ? initialHalfWidth : (baseWidth / 2.0f);

    for (size_t i = 0; i < smoothedPoints.size(); i++) {
        const Point& p = smoothedPoints[i];

        // Calculate velocity and direction from previous point
        float velocity = 0.0f;
        float verticalDirection = 0.0f;

        if (i > 0) {
            velocity = calculateVelocity(p, smoothedPoints[i - 1]);
            verticalDirection = calculateVerticalDirection(p, smoothedPoints[i - 1]);

            // Smooth velocity and direction to avoid jitter
            velocity = 0.7f * prevVelocity + 0.3f * velocity;
            verticalDirection = 0.8f * prevDirection + 0.2f * verticalDirection;
        }

        prevVelocity = velocity;
        prevDirection = verticalDirection;

        // Calculate calligraphy width
        Point prevPoint = (i > 0) ? smoothedPoints[i - 1] : p;
        float targetHalfWidth = calculateCalligraphyWidth(p, prevPoint, baseWidth, velocity, verticalDirection) / 2.0f;

        // CRITICAL: Limit rate of width change to prevent edge crossing
        // Max change is 20% of previous width per point
        float maxChange = prevHalfWidth * 0.2f;
        float halfWidth;
        if (targetHalfWidth > prevHalfWidth + maxChange) {
            halfWidth = prevHalfWidth + maxChange;
        } else if (targetHalfWidth < prevHalfWidth - maxChange) {
            halfWidth = prevHalfWidth - maxChange;
        } else {
            halfWidth = targetHalfWidth;
        }
        prevHalfWidth = halfWidth;

        // Calculate perpendicular direction
        float dx = 0.0f, dy = 0.0f;
        if (i < smoothedPoints.size() - 1) {
            dx = smoothedPoints[i + 1].x - p.x;
            dy = smoothedPoints[i + 1].y - p.y;
        } else if (i > 0) {
            dx = p.x - smoothedPoints[i - 1].x;
            dy = p.y - smoothedPoints[i - 1].y;
        }

        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.001f) {
            dx /= len;
            dy /= len;
        }

        float perpX = -dy;
        float perpY = dx;

        EdgePoint ep;
        ep.left = SkPoint::Make(p.x + perpX * halfWidth, p.y + perpY * halfWidth);
        ep.right = SkPoint::Make(p.x - perpX * halfWidth, p.y - perpY * halfWidth);
        ep.pressure = p.pressure;
        ep.angle = std::atan2(dy, dx);
        edges.push_back(ep);
    }

    // Output final half width for incremental continuity
    if (outFinalHalfWidth) {
        *outFinalHalfWidth = prevHalfWidth;
    }

    return true;
}

void PathRenderer::drawCalligraphyPath(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    bool applyPressureAlpha
) {
    if (points.empty() || points.size() < 2 || !canvas) return;

    float baseWidth = basePaint.getStrokeWidth();

    // Generate smoothed points with timestamp interpolation
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    // Build edges with calligraphy-specific width calculation
    std::vector<EdgePoint> edges;
    if (!buildCalligraphyEdgePoints(smoothedPoints, edges, baseWidth)) return;

    // Create path from edges (same as variable-width path)
    SkPath strokePath;
    strokePath.moveTo(edges[0].left);

    // Left edge forward
    for (size_t i = 1; i < edges.size(); i++) {
        strokePath.lineTo(edges[i].left);
    }

    // End cap - smooth semicircle
    if (edges.size() >= 2) {
        const EdgePoint& lastEdge = edges.back();
        float perpLen = std::sqrt(
            (lastEdge.left.x() - lastEdge.right.x()) * (lastEdge.left.x() - lastEdge.right.x()) +
            (lastEdge.left.y() - lastEdge.right.y()) * (lastEdge.left.y() - lastEdge.right.y())
        ) / 2.0f;

        SkPoint center = SkPoint::Make(
            (lastEdge.left.x() + lastEdge.right.x()) / 2.0f,
            (lastEdge.left.y() + lastEdge.right.y()) / 2.0f
        );

        // Tapered end cap using quadratic bezier
        float dx = smoothedPoints.back().x - smoothedPoints[smoothedPoints.size() - 2].x;
        float dy = smoothedPoints.back().y - smoothedPoints[smoothedPoints.size() - 2].y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.001f) { dx /= len; dy /= len; }

        SkPoint tipPoint = SkPoint::Make(center.x() + dx * perpLen * 0.8f, center.y() + dy * perpLen * 0.8f);
        strokePath.quadTo(tipPoint, lastEdge.right);
    }

    // Right edge backward
    for (int i = static_cast<int>(edges.size()) - 2; i >= 0; i--) {
        strokePath.lineTo(edges[i].right);
    }

    // Start cap - tapered
    if (edges.size() >= 2) {
        const EdgePoint& firstEdge = edges[0];
        SkPoint center = SkPoint::Make(
            (firstEdge.left.x() + firstEdge.right.x()) / 2.0f,
            (firstEdge.left.y() + firstEdge.right.y()) / 2.0f
        );

        float perpLen = std::sqrt(
            (firstEdge.left.x() - firstEdge.right.x()) * (firstEdge.left.x() - firstEdge.right.x()) +
            (firstEdge.left.y() - firstEdge.right.y()) * (firstEdge.left.y() - firstEdge.right.y())
        ) / 2.0f;

        float dx = smoothedPoints[1].x - smoothedPoints[0].x;
        float dy = smoothedPoints[1].y - smoothedPoints[0].y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.001f) { dx /= len; dy /= len; }

        SkPoint tipPoint = SkPoint::Make(center.x() - dx * perpLen * 0.8f, center.y() - dy * perpLen * 0.8f);
        strokePath.quadTo(tipPoint, firstEdge.left);
    }

    strokePath.close();

    // Create paint for filled stroke
    SkPaint fillPaint = basePaint;
    fillPaint.setStyle(SkPaint::kFill_Style);
    fillPaint.setAntiAlias(true);

    canvas->drawPath(strokePath, fillPaint);
}

IncrementalResult PathRenderer::drawCalligraphyPathIncremental(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    const SkPoint& prevLeftEdge,
    const SkPoint& prevRightEdge,
    bool isFirstSegment,
    float initialHalfWidth
) {
    IncrementalResult result = {};
    if (points.empty() || points.size() < 2 || !canvas) return result;

    float baseWidth = basePaint.getStrokeWidth();

    // Generate smoothed points
    std::vector<Point> smoothedPoints;
    interpolateSplinePoints(points, smoothedPoints, true);

    // Build edges with calligraphy width, passing initial width for continuity
    std::vector<EdgePoint> edges;
    float finalHalfWidth = 0.0f;
    if (!buildCalligraphyEdgePoints(smoothedPoints, edges, baseWidth, initialHalfWidth, &finalHalfWidth)) return result;

    // Connect to previous segment
    if (!isFirstSegment) {
        edges[0].left = prevLeftEdge;
        edges[0].right = prevRightEdge;
    }

    // Build path using simple lines
    SkPath path;
    path.moveTo(edges[0].left);

    // Left edge forward
    for (size_t i = 1; i < edges.size(); i++) {
        path.lineTo(edges[i].left);
    }

    // Right edge backward
    for (int i = static_cast<int>(edges.size()) - 1; i >= 0; i--) {
        path.lineTo(edges[i].right);
    }

    path.close();

    SkPaint fillPaint = basePaint;
    fillPaint.setStyle(SkPaint::kFill_Style);
    fillPaint.setAntiAlias(true);

    canvas->drawPath(path, fillPaint);

    // Return edge data for next segment
    result.lastLeftEdge = edges.back().left;
    result.lastRightEdge = edges.back().right;
    result.lastPressure = edges.back().pressure;
    result.lastAngle = edges.back().angle;
    result.lastHalfWidth = finalHalfWidth;
    result.smoothedPointsRendered = smoothedPoints.size();

    return result;
}

void PathRenderer::drawCalligraphyPathTail(
    SkCanvas* canvas,
    const std::vector<Point>& points,
    const SkPaint& basePaint,
    const SkPoint& prevLeftEdge,
    const SkPoint& prevRightEdge,
    bool hasPreviousEdge,
    float initialHalfWidth
) {
    if (points.size() < 2) return;

    // Use incremental rendering for tail, passing width for continuity
    IncrementalResult result = drawCalligraphyPathIncremental(
        canvas, points, basePaint, prevLeftEdge, prevRightEdge, !hasPreviousEdge, initialHalfWidth);

    // Add end cap for visual feedback while drawing
    // Use the rate-limited width from incremental result for consistency
    if (points.size() >= 2 && result.lastHalfWidth > 0) {
        std::vector<Point> smoothedPoints;
        interpolateSplinePoints(points, smoothedPoints, true);

        if (smoothedPoints.size() >= 2) {
            const Point& endPt = smoothedPoints.back();
            const Point& prevPt = smoothedPoints[smoothedPoints.size() - 2];

            // Use rate-limited width from incremental render (not recalculated)
            // This ensures the cap matches the stroke body exactly
            float endHalfWidth = result.lastHalfWidth;

            float dx = endPt.x - prevPt.x;
            float dy = endPt.y - prevPt.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.001f) { dx /= len; dy /= len; }

            float perpX = -dy;
            float perpY = dx;

            SkPoint endLeft = SkPoint::Make(endPt.x + perpX * endHalfWidth, endPt.y + perpY * endHalfWidth);
            SkPoint endRight = SkPoint::Make(endPt.x - perpX * endHalfWidth, endPt.y - perpY * endHalfWidth);
            SkPoint tipPoint = SkPoint::Make(endPt.x + dx * endHalfWidth * 0.8f, endPt.y + dy * endHalfWidth * 0.8f);

            SkPath endCapPath;
            endCapPath.moveTo(endLeft);
            endCapPath.quadTo(tipPoint, endRight);
            endCapPath.close();

            SkPaint fillPaint = basePaint;
            fillPaint.setStyle(SkPaint::kFill_Style);
            fillPaint.setAntiAlias(true);

            canvas->drawPath(endCapPath, fillPaint);
        }
    }
}

} // namespace nativedrawing
