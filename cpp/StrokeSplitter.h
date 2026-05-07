#pragma once

#include <vector>
#include "SkiaDrawingEngine.h"

namespace nativedrawing {

class PathRenderer;

/**
 * StrokeSplitter - Handles splitting strokes at eraser points
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Uses Catmull-Rom spline interpolation to find precise intersection
 * points between eraser circles and stroke paths.
 */
class StrokeSplitter {
public:
    explicit StrokeSplitter(PathRenderer* pathRenderer);

    /**
     * Split a stroke into segments by removing the portion intersecting the eraser
     *
     * @param originalStroke The stroke to split
     * @param eraserPoint Center of the eraser circle
     * @param eraserRadius Radius of the eraser circle
     * @return Vector of resulting stroke segments (may be 0, 1, or multiple)
     */
    std::vector<Stroke> splitStrokeAtPoint(
        const Stroke& originalStroke,
        const Point& eraserPoint,
        float eraserRadius
    );

private:
    PathRenderer* pathRenderer_;

    // Spline interpolation constants
    static constexpr int SEGMENTS_PER_SPAN = 4;
    static constexpr float SPLINE_TENSION = 0.04f;
    static constexpr float MIN_POINT_DIST = 3.0f;

    // Helper functions for eraser intersection detection
    static bool isPointInsideEraser(const Point& p, const Point& eraserCenter, float eraserRadius);
    static Point interpolatePoint(const Point& p0, const Point& p1, float t);
    static float closestPointOnSegmentT(const Point& p0, const Point& p1, const Point& center);
    static bool segmentIntersectsEraser(const Point& p0, const Point& p1,
                                        const Point& eraserCenter, float eraserRadius);
    static float findIntersectionT(const Point& p0, const Point& p1,
                                   const Point& eraserCenter, float eraserRadius,
                                   float tStart, float tEnd, bool startInside);

    // Generate smoothed points using Catmull-Rom splines
    std::vector<Point> generateSmoothedPoints(const std::vector<Point>& origPoints);

    // Downsample points to prevent exponential growth
    static std::vector<Point> downsamplePoints(const std::vector<Point>& points);
};

} // namespace nativedrawing
