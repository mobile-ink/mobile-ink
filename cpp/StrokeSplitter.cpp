#include "StrokeSplitter.h"
#include "PathRenderer.h"
#include <cmath>

namespace nativedrawing {

StrokeSplitter::StrokeSplitter(PathRenderer* pathRenderer)
    : pathRenderer_(pathRenderer) {}

bool StrokeSplitter::isPointInsideEraser(const Point& p, const Point& eraserCenter, float eraserRadius) {
    float strokeRadius = p.calculatedWidth / 2.0f;
    float dx = p.x - eraserCenter.x;
    float dy = p.y - eraserCenter.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    return dist < eraserRadius + strokeRadius;
}

Point StrokeSplitter::interpolatePoint(const Point& p0, const Point& p1, float t) {
    Point result;
    result.x = p0.x + t * (p1.x - p0.x);
    result.y = p0.y + t * (p1.y - p0.y);
    result.pressure = p0.pressure + t * (p1.pressure - p0.pressure);
    result.azimuthAngle = p0.azimuthAngle + t * (p1.azimuthAngle - p0.azimuthAngle);
    result.altitude = p0.altitude + t * (p1.altitude - p0.altitude);
    result.calculatedWidth = p0.calculatedWidth + t * (p1.calculatedWidth - p0.calculatedWidth);
    result.timestamp = 0;
    return result;
}

float StrokeSplitter::closestPointOnSegmentT(const Point& p0, const Point& p1, const Point& center) {
    float dx = p1.x - p0.x;
    float dy = p1.y - p0.y;
    float lenSq = dx * dx + dy * dy;
    if (lenSq < 0.0001f) return 0.0f;

    float t = ((center.x - p0.x) * dx + (center.y - p0.y) * dy) / lenSq;
    return std::max(0.0f, std::min(1.0f, t));
}

bool StrokeSplitter::segmentIntersectsEraser(
    const Point& p0, const Point& p1,
    const Point& eraserCenter, float eraserRadius
) {
    float t = closestPointOnSegmentT(p0, p1, eraserCenter);
    Point closest = interpolatePoint(p0, p1, t);
    return isPointInsideEraser(closest, eraserCenter, eraserRadius);
}

float StrokeSplitter::findIntersectionT(
    const Point& p0, const Point& p1,
    const Point& eraserCenter, float eraserRadius,
    float tStart, float tEnd, bool startInside
) {
    float tLow = tStart, tHigh = tEnd;

    for (int iter = 0; iter < 12; iter++) {
        float tMid = (tLow + tHigh) / 2.0f;
        Point midPoint = interpolatePoint(p0, p1, tMid);
        bool midInside = isPointInsideEraser(midPoint, eraserCenter, eraserRadius);

        if (midInside == startInside) {
            tLow = tMid;
        } else {
            tHigh = tMid;
        }
    }

    return (tLow + tHigh) / 2.0f;
}

std::vector<Point> StrokeSplitter::generateSmoothedPoints(const std::vector<Point>& origPoints) {
    std::vector<Point> smoothedPoints;

    if (origPoints.size() > 100) {
        // Already dense enough - use original points directly
        return origPoints;
    }

    smoothedPoints.reserve((origPoints.size() - 1) * SEGMENTS_PER_SPAN + 1);

    for (size_t i = 0; i < origPoints.size() - 1; i++) {
        Point p0, p1, p2, p3;
        p1 = origPoints[i];
        p2 = origPoints[i + 1];

        if (i == 0) { p0 = p1; }
        else { p0 = origPoints[i - 1]; }

        if (i + 2 >= origPoints.size()) { p3 = p2; }
        else { p3 = origPoints[i + 2]; }

        for (int seg = 0; seg < SEGMENTS_PER_SPAN; seg++) {
            float t = static_cast<float>(seg) / SEGMENTS_PER_SPAN;
            float t2 = t * t;
            float t3 = t2 * t;

            float q0 = -SPLINE_TENSION * t3 + 2.0f * SPLINE_TENSION * t2 - SPLINE_TENSION * t;
            float q1 = (2.0f - SPLINE_TENSION) * t3 + (SPLINE_TENSION - 3.0f) * t2 + 1.0f;
            float q2 = (SPLINE_TENSION - 2.0f) * t3 + (3.0f - 2.0f * SPLINE_TENSION) * t2 + SPLINE_TENSION * t;
            float q3 = SPLINE_TENSION * t3 - SPLINE_TENSION * t2;

            Point interpolated;
            interpolated.x = q0 * p0.x + q1 * p1.x + q2 * p2.x + q3 * p3.x;
            interpolated.y = q0 * p0.y + q1 * p1.y + q2 * p2.y + q3 * p3.y;
            interpolated.pressure = q0 * p0.pressure + q1 * p1.pressure + q2 * p2.pressure + q3 * p3.pressure;
            interpolated.pressure = std::max(0.1f, std::min(1.0f, interpolated.pressure));
            interpolated.azimuthAngle = q0 * p0.azimuthAngle + q1 * p1.azimuthAngle + q2 * p2.azimuthAngle + q3 * p3.azimuthAngle;
            interpolated.altitude = q0 * p0.altitude + q1 * p1.altitude + q2 * p2.altitude + q3 * p3.altitude;
            interpolated.calculatedWidth = q0 * p0.calculatedWidth + q1 * p1.calculatedWidth + q2 * p2.calculatedWidth + q3 * p3.calculatedWidth;
            interpolated.calculatedWidth = std::max(0.5f, interpolated.calculatedWidth);
            interpolated.timestamp = 0;

            smoothedPoints.push_back(interpolated);
        }
    }
    smoothedPoints.push_back(origPoints.back());

    return smoothedPoints;
}

std::vector<Point> StrokeSplitter::downsamplePoints(const std::vector<Point>& points) {
    if (points.size() <= 10) return points;

    std::vector<Point> result;
    result.push_back(points.front());

    float accumulatedDist = 0.0f;

    for (size_t i = 1; i < points.size() - 1; i++) {
        float dx = points[i].x - points[i-1].x;
        float dy = points[i].y - points[i-1].y;
        accumulatedDist += std::sqrt(dx * dx + dy * dy);

        if (accumulatedDist >= MIN_POINT_DIST) {
            result.push_back(points[i]);
            accumulatedDist = 0.0f;
        }
    }

    result.push_back(points.back());
    return result;
}

std::vector<Stroke> StrokeSplitter::splitStrokeAtPoint(
    const Stroke& originalStroke,
    const Point& eraserPoint,
    float eraserRadius
) {
    std::vector<Stroke> result;

    if (originalStroke.points.empty()) {
        return result;
    }

    if (originalStroke.isEraser) {
        result.push_back(originalStroke);
        return result;
    }

    // Quick bounding box check
    float minX = originalStroke.points[0].x, maxX = minX;
    float minY = originalStroke.points[0].y, maxY = minY;
    float maxStrokeWidth = originalStroke.points[0].calculatedWidth;

    for (const auto& p : originalStroke.points) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
        maxStrokeWidth = std::max(maxStrokeWidth, p.calculatedWidth);
    }

    float margin = eraserRadius + maxStrokeWidth / 2.0f + 10.0f;
    if (eraserPoint.x < minX - margin || eraserPoint.x > maxX + margin ||
        eraserPoint.y < minY - margin || eraserPoint.y > maxY + margin) {
        result.push_back(originalStroke);
        return result;
    }

    const auto& origPoints = originalStroke.points;

    if (origPoints.size() < 2) {
        result.push_back(originalStroke);
        return result;
    }

    // Generate smoothed points
    std::vector<Point> smoothedPoints = generateSmoothedPoints(origPoints);

    // Apply eraser algorithm
    std::vector<Point> currentSegment;

    auto saveSegment = [&]() {
        if (currentSegment.size() >= 2) {
            Stroke newStroke;
            newStroke.points = downsamplePoints(currentSegment);
            newStroke.paint = originalStroke.paint;
            newStroke.isEraser = false;
            newStroke.originalAlphaMod = originalStroke.originalAlphaMod;
            newStroke.toolType = originalStroke.toolType;
            if (!buildRecognizedShapePath(newStroke.toolType, newStroke.points, newStroke.path)) {
                pathRenderer_->smoothPath(newStroke.points, newStroke.path);
            }
            result.push_back(newStroke);
        }
        currentSegment.clear();
    };

    for (size_t i = 0; i < smoothedPoints.size(); i++) {
        const Point& p1 = smoothedPoints[i];
        bool p1Inside = isPointInsideEraser(p1, eraserPoint, eraserRadius);

        if (i > 0) {
            const Point& p0 = smoothedPoints[i - 1];
            bool p0Inside = isPointInsideEraser(p0, eraserPoint, eraserRadius);

            if (p0Inside && p1Inside) {
                continue;
            } else if (!p0Inside && !p1Inside) {
                if (segmentIntersectsEraser(p0, p1, eraserPoint, eraserRadius)) {
                    float tClosest = closestPointOnSegmentT(p0, p1, eraserPoint);

                    float tEntry = findIntersectionT(p0, p1, eraserPoint, eraserRadius,
                                                      0.0f, tClosest, false);
                    Point entryPoint = interpolatePoint(p0, p1, tEntry);

                    float tExit = findIntersectionT(p0, p1, eraserPoint, eraserRadius,
                                                     tClosest, 1.0f, true);
                    Point exitPoint = interpolatePoint(p0, p1, tExit);

                    currentSegment.push_back(entryPoint);
                    saveSegment();
                    currentSegment.push_back(exitPoint);
                }
            } else if (p0Inside && !p1Inside) {
                float t = findIntersectionT(p0, p1, eraserPoint, eraserRadius, 0.0f, 1.0f, true);
                Point intersection = interpolatePoint(p0, p1, t);
                currentSegment.push_back(intersection);
            } else {
                float t = findIntersectionT(p0, p1, eraserPoint, eraserRadius, 0.0f, 1.0f, false);
                Point intersection = interpolatePoint(p0, p1, t);
                currentSegment.push_back(intersection);
                saveSegment();
            }
        }

        if (!p1Inside) {
            currentSegment.push_back(p1);
        }
    }

    saveSegment();

    // If no segments created and stroke wasn't touched, return original
    if (result.empty()) {
        bool anyTouched = false;
        for (size_t i = 0; i < smoothedPoints.size(); i++) {
            if (isPointInsideEraser(smoothedPoints[i], eraserPoint, eraserRadius)) {
                anyTouched = true;
                break;
            }
            if (i > 0 && segmentIntersectsEraser(smoothedPoints[i-1],
                                                   smoothedPoints[i],
                                                   eraserPoint, eraserRadius)) {
                anyTouched = true;
                break;
            }
        }
        if (!anyTouched) {
            result.push_back(originalStroke);
        }
    }

    return result;
}

} // namespace nativedrawing
