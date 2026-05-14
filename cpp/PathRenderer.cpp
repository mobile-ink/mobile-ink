#include "PathRenderer.h"

#include <algorithm>
#include <cmath>

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

} // namespace nativedrawing
