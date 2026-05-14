#include "PathRenderer.h"

#include <algorithm>
#include <cmath>

namespace nativedrawing {

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
