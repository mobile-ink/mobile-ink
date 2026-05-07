#include "SkiaDrawingEngine.h"
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
constexpr long kHoldToShapeDurationMs = 300;
constexpr float kMinimumShapeDiagonal = 30.0f;
constexpr float kPi = 3.14159265358979323846f;

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

bool canRecognizeShapeForTool(const std::string& toolType) {
    return toolType == "pen"
        || toolType == "pencil"
        || toolType == "marker"
        || toolType == "highlighter"
        || toolType == "crayon"
        || toolType == "calligraphy";
}

float distanceBetween(float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

float distanceBetween(const Point& a, const Point& b) {
    return distanceBetween(a.x, a.y, b.x, b.y);
}

float averageCalculatedWidth(const std::vector<Point>& points) {
    if (points.empty()) {
        return 1.0f;
    }

    float total = 0.0f;
    for (const auto& point : points) {
        total += std::max(0.5f, point.calculatedWidth);
    }
    return total / static_cast<float>(points.size());
}

float averagePressure(const std::vector<Point>& points) {
    if (points.empty()) {
        return 1.0f;
    }

    float total = 0.0f;
    for (const auto& point : points) {
        total += std::max(0.1f, std::min(1.0f, point.pressure));
    }
    return total / static_cast<float>(points.size());
}

float pathLength(const std::vector<Point>& points) {
    if (points.size() < 2) {
        return 0.0f;
    }

    float total = 0.0f;
    for (size_t i = 1; i < points.size(); ++i) {
        total += distanceBetween(points[i - 1], points[i]);
    }
    return total;
}

SkRect boundsForPoints(const std::vector<Point>& points) {
    if (points.empty()) {
        return SkRect::MakeEmpty();
    }

    float minX = points.front().x;
    float maxX = points.front().x;
    float minY = points.front().y;
    float maxY = points.front().y;

    for (const auto& point : points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }

    return SkRect::MakeLTRB(minX, minY, maxX, maxY);
}

struct ShapePointStyle {
    float pressure = 1.0f;
    float azimuthAngle = 0.0f;
    float altitude = 1.57f;
    float calculatedWidth = 1.0f;
    long timestamp = 0;
};

ShapePointStyle styleForShapePoints(const std::vector<Point>& points) {
    ShapePointStyle style;
    if (points.empty()) {
        return style;
    }

    style.pressure = averagePressure(points);
    style.calculatedWidth = averageCalculatedWidth(points);
    style.azimuthAngle = points.back().azimuthAngle;
    style.altitude = points.back().altitude;
    style.timestamp = points.back().timestamp;
    return style;
}

Point makeShapePoint(float x, float y, const ShapePointStyle& style) {
    Point point;
    point.x = x;
    point.y = y;
    point.pressure = style.pressure;
    point.azimuthAngle = style.azimuthAngle;
    point.altitude = style.altitude;
    point.calculatedWidth = style.calculatedWidth;
    point.timestamp = style.timestamp;
    return point;
}

void appendLinePoints(
    std::vector<Point>& output,
    float x1,
    float y1,
    float x2,
    float y2,
    const ShapePointStyle& style,
    float spacing,
    bool includeStart
) {
    const float length = distanceBetween(x1, y1, x2, y2);
    const int steps = std::max(1, static_cast<int>(std::ceil(length / std::max(2.0f, spacing))));
    const int startStep = includeStart ? 0 : 1;

    for (int step = startStep; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        output.push_back(makeShapePoint(
            x1 + (x2 - x1) * t,
            y1 + (y2 - y1) * t,
            style
        ));
    }
}

struct ShapeCandidate {
    bool recognized = false;
    std::string toolType;
    std::vector<Point> points;
};

ShapeCandidate makeLineCandidate(const std::vector<Point>& points) {
    ShapeCandidate candidate;
    candidate.recognized = true;
    candidate.toolType = "shape-line";
    const ShapePointStyle style = styleForShapePoints(points);
    candidate.points.push_back(makeShapePoint(points.front().x, points.front().y, style));
    candidate.points.push_back(makeShapePoint(points.back().x, points.back().y, style));
    return candidate;
}

ShapeCandidate makeRectangleCandidate(const std::vector<Point>& points, const SkRect& bounds) {
    ShapeCandidate candidate;
    candidate.recognized = true;
    candidate.toolType = "shape-rectangle";

    const ShapePointStyle style = styleForShapePoints(points);
    const float spacing = std::max(4.0f, style.calculatedWidth * 1.6f);

    appendLinePoints(candidate.points, bounds.left(), bounds.top(), bounds.right(), bounds.top(), style, spacing, true);
    appendLinePoints(candidate.points, bounds.right(), bounds.top(), bounds.right(), bounds.bottom(), style, spacing, false);
    appendLinePoints(candidate.points, bounds.right(), bounds.bottom(), bounds.left(), bounds.bottom(), style, spacing, false);
    appendLinePoints(candidate.points, bounds.left(), bounds.bottom(), bounds.left(), bounds.top(), style, spacing, false);

    return candidate;
}

ShapeCandidate makeEllipseCandidate(const std::vector<Point>& points, const SkRect& bounds) {
    ShapeCandidate candidate;
    candidate.recognized = true;

    const ShapePointStyle style = styleForShapePoints(points);
    const float width = bounds.width();
    const float height = bounds.height();
    const float aspectError = std::fabs(width - height) / std::max(width, height);
    const bool shouldForceCircle = aspectError <= 0.18f;

    float centerX = bounds.centerX();
    float centerY = bounds.centerY();
    float radiusX = width / 2.0f;
    float radiusY = height / 2.0f;

    if (shouldForceCircle) {
        const float radius = (radiusX + radiusY) / 2.0f;
        radiusX = radius;
        radiusY = radius;
        candidate.toolType = "shape-circle";
    } else {
        candidate.toolType = "shape-ellipse";
    }

    const float circumferenceEstimate = 2.0f * kPi * std::sqrt((radiusX * radiusX + radiusY * radiusY) / 2.0f);
    const int segments = std::max(40, std::min(128, static_cast<int>(circumferenceEstimate / std::max(3.0f, style.calculatedWidth * 1.4f))));

    candidate.points.reserve(static_cast<size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const float theta = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(segments);
        candidate.points.push_back(makeShapePoint(
            centerX + std::cos(theta) * radiusX,
            centerY + std::sin(theta) * radiusY,
            style
        ));
    }

    return candidate;
}

std::vector<Point> transformedShapePoints(
    const std::vector<Point>& basePoints,
    const Point& anchor,
    float referenceAngle,
    float referenceDistance,
    float targetX,
    float targetY,
    float scaleSensitivity,
    float rotationSensitivity
) {
    if (basePoints.empty()) {
        return {};
    }

    const float targetDx = targetX - anchor.x;
    const float targetDy = targetY - anchor.y;
    const float targetDistance = std::sqrt(targetDx * targetDx + targetDy * targetDy);
    if (targetDistance < 0.001f || referenceDistance < 0.001f) {
        return basePoints;
    }

    const float rawScale = targetDistance / referenceDistance;
    const float scale = std::max(
        0.08f,
        1.0f + (rawScale - 1.0f) * std::max(0.0f, std::min(1.0f, scaleSensitivity))
    );
    const float rawRotation = std::atan2(targetDy, targetDx) - referenceAngle;
    const float rotation = rawRotation * std::max(0.0f, std::min(1.0f, rotationSensitivity));
    const float cosRotation = std::cos(rotation);
    const float sinRotation = std::sin(rotation);

    std::vector<Point> transformed;
    transformed.reserve(basePoints.size());
    for (const auto& point : basePoints) {
        const float localX = (point.x - anchor.x) * scale;
        const float localY = (point.y - anchor.y) * scale;
        Point next = point;
        next.x = anchor.x + (localX * cosRotation - localY * sinRotation);
        next.y = anchor.y + (localX * sinRotation + localY * cosRotation);
        transformed.push_back(next);
    }

    return transformed;
}

Point centerPointForPoints(const std::vector<Point>& points) {
    if (points.empty()) {
        return makeShapePoint(0.0f, 0.0f, ShapePointStyle{});
    }

    float minX = points.front().x;
    float maxX = points.front().x;
    float minY = points.front().y;
    float maxY = points.front().y;

    for (const auto& point : points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }

    Point center = points.front();
    center.x = (minX + maxX) * 0.5f;
    center.y = (minY + maxY) * 0.5f;
    return center;
}

float clampedSignedScale(float rawScale) {
    if (!std::isfinite(rawScale)) {
        return 1.0f;
    }

    const float magnitude = std::max(0.08f, std::fabs(rawScale));
    return rawScale < 0.0f ? -magnitude : magnitude;
}

std::vector<Point> transformedShapePointsCenterLockedToTarget(
    const std::vector<Point>& basePoints,
    const Point& center,
    float referenceAngle,
    float referenceDistance,
    float targetX,
    float targetY
) {
    if (basePoints.empty()) {
        return {};
    }

    const float refDx = std::cos(referenceAngle) * referenceDistance;
    const float refDy = std::sin(referenceAngle) * referenceDistance;
    const float targetDx = targetX - center.x;
    const float targetDy = targetY - center.y;
    const float targetDistance = std::sqrt(targetDx * targetDx + targetDy * targetDy);
    const float fallbackScale = referenceDistance > 0.001f
        ? targetDistance / referenceDistance
        : 1.0f;

    const float scaleX = std::fabs(refDx) > 2.0f
        ? clampedSignedScale(targetDx / refDx)
        : clampedSignedScale(fallbackScale);
    const float scaleY = std::fabs(refDy) > 2.0f
        ? clampedSignedScale(targetDy / refDy)
        : clampedSignedScale(fallbackScale);

    std::vector<Point> transformed;
    transformed.reserve(basePoints.size());
    for (const auto& point : basePoints) {
        Point next = point;
        next.x = center.x + ((point.x - center.x) * scaleX);
        next.y = center.y + ((point.y - center.y) * scaleY);
        transformed.push_back(next);
    }

    return transformed;
}

float distanceFromPointToSegment(const Point& point, const Point& start, const Point& end) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float lengthSq = dx * dx + dy * dy;

    if (lengthSq < 0.001f) {
        return distanceBetween(point, start);
    }

    const float t = std::max(0.0f, std::min(
        1.0f,
        ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSq
    ));

    return distanceBetween(
        point.x,
        point.y,
        start.x + (dx * t),
        start.y + (dy * t)
    );
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

void simplifyPolylineRecursive(
    const std::vector<Point>& points,
    size_t first,
    size_t last,
    float epsilon,
    std::vector<size_t>& outputIndices
) {
    if (last <= first + 1) {
        return;
    }

    float maxDistance = 0.0f;
    size_t maxIndex = first;
    for (size_t i = first + 1; i < last; ++i) {
        const float distance = distanceFromPointToSegment(points[i], points[first], points[last]);
        if (distance > maxDistance) {
            maxDistance = distance;
            maxIndex = i;
        }
    }

    if (maxDistance <= epsilon || maxIndex == first) {
        return;
    }

    simplifyPolylineRecursive(points, first, maxIndex, epsilon, outputIndices);
    outputIndices.push_back(maxIndex);
    simplifyPolylineRecursive(points, maxIndex, last, epsilon, outputIndices);
}

std::vector<Point> decimatedShapePoints(const std::vector<Point>& points, float minimumSpacing) {
    std::vector<Point> decimated;
    decimated.reserve(points.size());

    for (const auto& point : points) {
        if (decimated.empty() || distanceBetween(decimated.back(), point) >= minimumSpacing) {
            decimated.push_back(point);
        }
    }

    if (decimated.size() > 3 && distanceBetween(decimated.front(), decimated.back()) < minimumSpacing) {
        decimated.pop_back();
    }

    return decimated;
}

std::vector<Point> simplifyPolyline(const std::vector<Point>& points, float epsilon) {
    if (points.size() <= 2) {
        return points;
    }

    std::vector<size_t> indices;
    indices.reserve(points.size());
    indices.push_back(0);
    simplifyPolylineRecursive(points, 0, points.size() - 1, epsilon, indices);
    indices.push_back(points.size() - 1);
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    std::vector<Point> simplified;
    simplified.reserve(indices.size());
    for (size_t index : indices) {
        simplified.push_back(points[index]);
    }

    return simplified;
}

std::vector<Point> removeNearlyCollinearVertices(
    const std::vector<Point>& vertices,
    float tolerance
) {
    if (vertices.size() <= 3) {
        return vertices;
    }

    std::vector<Point> pruned = vertices;
    bool removed = true;
    while (removed && pruned.size() > 3) {
        removed = false;
        for (size_t i = 0; i < pruned.size(); ++i) {
            const Point& previous = pruned[(i + pruned.size() - 1) % pruned.size()];
            const Point& current = pruned[i];
            const Point& next = pruned[(i + 1) % pruned.size()];

            if (distanceFromPointToSegment(current, previous, next) <= tolerance) {
                pruned.erase(pruned.begin() + static_cast<long>(i));
                removed = true;
                break;
            }
        }
    }

    return pruned;
}

float polygonFitMeanError(
    const std::vector<Point>& sourcePoints,
    const std::vector<Point>& vertices,
    float* maxErrorOut
) {
    if (vertices.size() < 3 || sourcePoints.empty()) {
        if (maxErrorOut) {
            *maxErrorOut = std::numeric_limits<float>::max();
        }
        return std::numeric_limits<float>::max();
    }

    float totalError = 0.0f;
    float maxError = 0.0f;
    for (const auto& sourcePoint : sourcePoints) {
        float nearestDistance = std::numeric_limits<float>::max();
        for (size_t i = 0; i < vertices.size(); ++i) {
            const Point& start = vertices[i];
            const Point& end = vertices[(i + 1) % vertices.size()];
            nearestDistance = std::min(
                nearestDistance,
                distanceFromPointToSegment(sourcePoint, start, end)
            );
        }
        totalError += nearestDistance;
        maxError = std::max(maxError, nearestDistance);
    }

    if (maxErrorOut) {
        *maxErrorOut = maxError;
    }
    return totalError / static_cast<float>(sourcePoints.size());
}

struct PolygonRoughFit {
    size_t vertexCount = 0;
    float meanError = std::numeric_limits<float>::max();
    float maxError = std::numeric_limits<float>::max();
};

PolygonRoughFit roughPolygonFitForStroke(
    const std::vector<Point>& points,
    const SkRect& bounds,
    float averageWidth
) {
    PolygonRoughFit fit;

    const float diagonal = std::sqrt(bounds.width() * bounds.width() + bounds.height() * bounds.height());
    const float decimationSpacing = std::max(3.0f, averageWidth * 1.2f);
    const std::vector<Point> sourcePoints = decimatedShapePoints(points, decimationSpacing);
    if (sourcePoints.size() < 6) {
        return fit;
    }

    const float epsilon = std::max(8.0f, std::max(averageWidth * 2.2f, diagonal * 0.028f));
    std::vector<Point> vertices = simplifyPolyline(sourcePoints, epsilon);
    if (vertices.size() > 3 && distanceBetween(vertices.front(), vertices.back()) <= std::max(12.0f, averageWidth * 3.0f)) {
        vertices.pop_back();
    }
    vertices = removeNearlyCollinearVertices(vertices, std::max(5.0f, averageWidth * 1.7f));

    fit.vertexCount = vertices.size();
    if (vertices.size() >= 3) {
        fit.meanError = polygonFitMeanError(sourcePoints, vertices, &fit.maxError);
    }

    return fit;
}

std::vector<Point> reducePolygonVerticesByFit(
    const std::vector<Point>& sourcePoints,
    const std::vector<Point>& vertices,
    float averageWidth,
    float diagonal
) {
    if (vertices.size() <= 3) {
        return vertices;
    }

    const float meanTolerance = std::max(10.0f, std::max(averageWidth * 2.8f, diagonal * 0.045f));
    const float maxTolerance = std::max(24.0f, std::max(averageWidth * 5.0f, diagonal * 0.14f));
    std::vector<Point> reduced = vertices;

    while (reduced.size() > 3) {
        size_t bestRemoveIndex = std::numeric_limits<size_t>::max();
        float bestScore = std::numeric_limits<float>::max();
        std::vector<Point> bestVertices;

        for (size_t i = 0; i < reduced.size(); ++i) {
            std::vector<Point> trial;
            trial.reserve(reduced.size() - 1);
            for (size_t j = 0; j < reduced.size(); ++j) {
                if (j != i) {
                    trial.push_back(reduced[j]);
                }
            }

            float maxError = 0.0f;
            const float meanError = polygonFitMeanError(sourcePoints, trial, &maxError);
            if (meanError > meanTolerance || maxError > maxTolerance) {
                continue;
            }

            const float score = meanError + maxError * 0.08f;
            if (score < bestScore) {
                bestScore = score;
                bestRemoveIndex = i;
                bestVertices = std::move(trial);
            }
        }

        if (bestRemoveIndex == std::numeric_limits<size_t>::max()) {
            break;
        }

        reduced = std::move(bestVertices);
    }

    return reduced;
}

ShapeCandidate makePolygonCandidate(
    const std::vector<Point>& points,
    const SkRect& bounds,
    float averageWidth
) {
    ShapeCandidate candidate;

    const float diagonal = std::sqrt(bounds.width() * bounds.width() + bounds.height() * bounds.height());
    const float decimationSpacing = std::max(3.0f, averageWidth * 1.2f);
    const std::vector<Point> sourcePoints = decimatedShapePoints(points, decimationSpacing);
    if (sourcePoints.size() < 6) {
        return candidate;
    }

    const float epsilon = std::max(8.0f, std::max(averageWidth * 2.2f, diagonal * 0.028f));
    std::vector<Point> vertices = simplifyPolyline(sourcePoints, epsilon);
    if (vertices.size() > 3 && distanceBetween(vertices.front(), vertices.back()) <= std::max(12.0f, averageWidth * 3.0f)) {
        vertices.pop_back();
    }
    vertices = removeNearlyCollinearVertices(vertices, std::max(5.0f, averageWidth * 1.7f));
    vertices = reducePolygonVerticesByFit(sourcePoints, vertices, averageWidth, diagonal);
    vertices = removeNearlyCollinearVertices(vertices, std::max(5.0f, averageWidth * 1.7f));

    if (vertices.size() < 3 || vertices.size() > 12) {
        return candidate;
    }

    float maxError = 0.0f;
    const float meanError = polygonFitMeanError(sourcePoints, vertices, &maxError);
    const float meanTolerance = std::max(10.0f, std::max(averageWidth * 2.8f, diagonal * 0.045f));
    const float maxTolerance = std::max(24.0f, std::max(averageWidth * 5.0f, diagonal * 0.14f));
    if (meanError > meanTolerance || maxError > maxTolerance) {
        return candidate;
    }

    const ShapePointStyle style = styleForShapePoints(points);
    candidate.recognized = true;
    candidate.toolType = "shape-polygon";
    candidate.points.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        candidate.points.push_back(makeShapePoint(vertex.x, vertex.y, style));
    }
    return candidate;
}

bool appendUniqueSnapPoint(std::vector<Point>& output, const Point& point, float minimumDistance) {
    for (const auto& existing : output) {
        if (distanceBetween(existing, point) <= minimumDistance) {
            return false;
        }
    }

    output.push_back(point);
    return true;
}

void collectSnapPointsForStroke(const Stroke& stroke, std::vector<Point>& output) {
    if (stroke.points.empty() || stroke.isEraser) {
        return;
    }

    const float minimumDistance = std::max(8.0f, averageCalculatedWidth(stroke.points) * 1.5f);

    if (stroke.toolType == "shape-line") {
        appendUniqueSnapPoint(output, stroke.points.front(), minimumDistance);
        appendUniqueSnapPoint(output, stroke.points.back(), minimumDistance);
        return;
    }

    if (stroke.toolType == "shape-polygon") {
        for (const auto& point : stroke.points) {
            appendUniqueSnapPoint(output, point, minimumDistance);
        }
        return;
    }

    if (stroke.toolType == "shape-rectangle") {
        const SkRect bounds = boundsForPoints(stroke.points);
        const ShapePointStyle style = styleForShapePoints(stroke.points);
        appendUniqueSnapPoint(output, makeShapePoint(bounds.left(), bounds.top(), style), minimumDistance);
        appendUniqueSnapPoint(output, makeShapePoint(bounds.right(), bounds.top(), style), minimumDistance);
        appendUniqueSnapPoint(output, makeShapePoint(bounds.right(), bounds.bottom(), style), minimumDistance);
        appendUniqueSnapPoint(output, makeShapePoint(bounds.left(), bounds.bottom(), style), minimumDistance);
    }
}

bool snapPointToNearest(
    Point& point,
    const std::vector<Stroke>& strokes,
    float snapThreshold
) {
    std::vector<Point> snapPoints;
    for (const auto& stroke : strokes) {
        collectSnapPointsForStroke(stroke, snapPoints);
    }

    float bestDistance = snapThreshold;
    const Point* bestPoint = nullptr;
    for (const auto& snapPoint : snapPoints) {
        const float distance = distanceBetween(point, snapPoint);
        if (distance <= bestDistance) {
            bestDistance = distance;
            bestPoint = &snapPoint;
        }
    }

    if (!bestPoint) {
        return false;
    }

    point.x = bestPoint->x;
    point.y = bestPoint->y;
    return true;
}

void snapRecognizedShapeCandidateToStrokes(
    ShapeCandidate& candidate,
    const std::vector<Stroke>& strokes,
    float averageWidth
) {
    if (!candidate.recognized || strokes.empty()) {
        return;
    }

    if (candidate.toolType != "shape-line" && candidate.toolType != "shape-polygon") {
        return;
    }

    const float snapThreshold = std::max(18.0f, averageWidth * 4.0f);
    if (candidate.toolType == "shape-line") {
        if (!candidate.points.empty()) {
            snapPointToNearest(candidate.points.front(), strokes, snapThreshold);
            snapPointToNearest(candidate.points.back(), strokes, snapThreshold);
        }
        return;
    }

    for (auto& point : candidate.points) {
        snapPointToNearest(point, strokes, snapThreshold);
    }
}

float lineFitScore(const std::vector<Point>& points, float endDistance, float strokeLength, float averageWidth) {
    if (points.size() < 2 || endDistance < 0.001f) {
        return 999.0f;
    }

    const Point& start = points.front();
    const Point& end = points.back();
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    float totalDeviation = 0.0f;
    float maxDeviation = 0.0f;

    for (const auto& point : points) {
        const float deviation = std::fabs(dy * point.x - dx * point.y + end.x * start.y - end.y * start.x) / endDistance;
        totalDeviation += deviation;
        maxDeviation = std::max(maxDeviation, deviation);
    }

    const float meanDeviation = totalDeviation / static_cast<float>(points.size());
    const float meanTolerance = std::max(10.0f, std::max(averageWidth * 3.0f, endDistance * 0.060f));
    const float maxTolerance = std::max(18.0f, std::max(averageWidth * 6.0f, endDistance * 0.16f));
    const float lengthRatio = strokeLength / endDistance;

    if (meanDeviation > meanTolerance || maxDeviation > maxTolerance || lengthRatio > 1.55f) {
        return 999.0f;
    }

    return (meanDeviation / std::max(1.0f, endDistance)) + std::max(0.0f, lengthRatio - 1.0f) * 0.35f;
}

float ellipseCoverage(const std::vector<Point>& points, float centerX, float centerY) {
    if (points.size() < 4) {
        return 0.0f;
    }

    std::vector<float> angles;
    angles.reserve(points.size());
    for (const auto& point : points) {
        angles.push_back(std::atan2(point.y - centerY, point.x - centerX));
    }

    std::sort(angles.begin(), angles.end());

    float largestGap = 0.0f;
    for (size_t i = 1; i < angles.size(); ++i) {
        largestGap = std::max(largestGap, angles[i] - angles[i - 1]);
    }
    largestGap = std::max(largestGap, (angles.front() + 2.0f * kPi) - angles.back());

    return (2.0f * kPi) - largestGap;
}

float ellipseFitScore(const std::vector<Point>& points, const SkRect& bounds, float closureDistance, float diagonal) {
    const float radiusX = bounds.width() / 2.0f;
    const float radiusY = bounds.height() / 2.0f;
    if (radiusX < 8.0f || radiusY < 8.0f) {
        return 999.0f;
    }

    const float centerX = bounds.centerX();
    const float centerY = bounds.centerY();
    float totalError = 0.0f;

    for (const auto& point : points) {
        const float nx = (point.x - centerX) / radiusX;
        const float ny = (point.y - centerY) / radiusY;
        totalError += std::fabs(std::sqrt(nx * nx + ny * ny) - 1.0f);
    }

    const float meanError = totalError / static_cast<float>(points.size());
    const float coverage = ellipseCoverage(points, centerX, centerY);
    if (meanError > 0.30f || coverage < (kPi * 1.45f)) {
        return 999.0f;
    }

    return meanError
        + (closureDistance / std::max(1.0f, diagonal)) * 0.35f
        + (1.0f - coverage / (2.0f * kPi)) * 0.45f;
}

float rectangleFitScore(const std::vector<Point>& points, const SkRect& bounds, float closureDistance, float diagonal) {
    const float width = bounds.width();
    const float height = bounds.height();
    const float minDimension = std::min(width, height);
    if (minDimension < 16.0f) {
        return 999.0f;
    }

    const float cornerTolerance = std::max(22.0f, diagonal * 0.30f);
    const float sideTolerance = std::max(14.0f, minDimension * 0.16f);
    const float corners[4][2] = {
        { bounds.left(), bounds.top() },
        { bounds.right(), bounds.top() },
        { bounds.right(), bounds.bottom() },
        { bounds.left(), bounds.bottom() },
    };

    bool hasCorner[4] = { false, false, false, false };
    int sideCounts[4] = { 0, 0, 0, 0 };
    float totalSideDistance = 0.0f;

    for (const auto& point : points) {
        for (int i = 0; i < 4; ++i) {
            if (distanceBetween(point.x, point.y, corners[i][0], corners[i][1]) <= cornerTolerance) {
                hasCorner[i] = true;
            }
        }

        const float distances[4] = {
            std::fabs(point.y - bounds.top()),
            std::fabs(point.x - bounds.right()),
            std::fabs(point.y - bounds.bottom()),
            std::fabs(point.x - bounds.left()),
        };

        int nearestSide = 0;
        float nearestDistance = distances[0];
        for (int i = 1; i < 4; ++i) {
            if (distances[i] < nearestDistance) {
                nearestDistance = distances[i];
                nearestSide = i;
            }
        }

        totalSideDistance += nearestDistance;
        if (nearestDistance <= sideTolerance) {
            sideCounts[nearestSide] += 1;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (!hasCorner[i]) {
            return 999.0f;
        }
    }

    const int minimumSideSamples = std::max(2, static_cast<int>(points.size() * 0.035f));
    for (int sideCount : sideCounts) {
        if (sideCount < minimumSideSamples) {
            return 999.0f;
        }
    }

    const float meanSideDistance = totalSideDistance / static_cast<float>(points.size());
    const float normalizedSideError = meanSideDistance / std::max(1.0f, minDimension);
    if (normalizedSideError > 0.13f) {
        return 999.0f;
    }

    return normalizedSideError + (closureDistance / std::max(1.0f, diagonal)) * 0.30f;
}

long endpointHoldDurationMillis(
    const std::vector<Point>& points,
    long endTimestamp,
    float averageWidth
) {
    if (points.empty() || endTimestamp <= 0) {
        return 0;
    }

    const Point& endpoint = points.back();
    if (endpoint.timestamp <= 0) {
        return 0;
    }

    const float holdRadius = std::max(8.0f, averageWidth * 1.5f);
    const float holdRadiusSq = holdRadius * holdRadius;
    long heldSince = endpoint.timestamp;

    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        if (it->timestamp <= 0) {
            break;
        }

        const float dx = it->x - endpoint.x;
        const float dy = it->y - endpoint.y;
        if (dx * dx + dy * dy > holdRadiusSq) {
            break;
        }

        heldSince = it->timestamp;
    }

    return std::max<long>(0, endTimestamp - heldSince);
}

ShapeCandidate recognizeHeldShape(
    const std::vector<Point>& points,
    const std::string& currentTool,
    long endTimestamp
) {
    ShapeCandidate empty;

    if (!canRecognizeShapeForTool(currentTool) || points.size() < 2) {
        return empty;
    }

    const float averageWidth = averageCalculatedWidth(points);
    if (endpointHoldDurationMillis(points, endTimestamp, averageWidth) < kHoldToShapeDurationMs) {
        return empty;
    }

    const SkRect bounds = boundsForPoints(points);
    const float diagonal = std::sqrt(bounds.width() * bounds.width() + bounds.height() * bounds.height());
    if (diagonal < std::max(kMinimumShapeDiagonal, averageWidth * 3.0f)) {
        return empty;
    }

    const float strokeLength = pathLength(points);
    const float closureDistance = distanceBetween(points.front(), points.back());
    const float endDistance = distanceBetween(points.front(), points.back());

    if (endDistance >= std::max(18.0f, averageWidth * 2.0f)) {
        const float score = lineFitScore(points, endDistance, strokeLength, averageWidth);
        if (score < 0.30f) {
            return makeLineCandidate(points);
        }
    }

    const bool isClosedEnough = closureDistance <= std::max(34.0f, diagonal * 0.30f);
    if (!isClosedEnough || points.size() < 8) {
        return empty;
    }

    const float rectangleScore = rectangleFitScore(points, bounds, closureDistance, diagonal);
    const float ellipseScore = ellipseFitScore(points, bounds, closureDistance, diagonal);

    if (rectangleScore < 999.0f && (ellipseScore >= 999.0f || rectangleScore <= ellipseScore * 0.86f)) {
        return makeRectangleCandidate(points, bounds);
    }

    const PolygonRoughFit roughPolygonFit = roughPolygonFitForStroke(points, bounds, averageWidth);
    const float roundMeanThreshold = std::max(averageWidth * 0.85f, diagonal * 0.018f);
    const float roundMaxThreshold = std::max(averageWidth * 2.0f, diagonal * 0.050f);
    const bool highConfidenceEllipse =
        ellipseScore < 0.16f &&
        roughPolygonFit.vertexCount >= 8;
    const bool smoothLoopFitsEllipse =
        highConfidenceEllipse ||
        (
            ellipseScore < 0.24f &&
            roughPolygonFit.vertexCount >= 6 &&
            (
                roughPolygonFit.meanError >= roundMeanThreshold ||
                roughPolygonFit.maxError >= roundMaxThreshold
            )
        );

    if (smoothLoopFitsEllipse) {
        return makeEllipseCandidate(points, bounds);
    }

    ShapeCandidate polygonCandidate = makePolygonCandidate(points, bounds, averageWidth);
    if (polygonCandidate.recognized) {
        return polygonCandidate;
    }

    if (ellipseScore < 999.0f) {
        return makeEllipseCandidate(points, bounds);
    }

    return empty;
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
    // Any new operation invalidates the redo timeline.
    redoStack_.clear();
    undoStack_.push_back(std::move(delta));
    // Cap depth: drop the oldest entry. Each entry is O(opSize) so the
    // cap is purely about bounding the number of undo levels, not
    // memory pressure (delta sizes don't grow with stroke count).
    while (undoStack_.size() > MAX_HISTORY_ENTRIES) {
        undoStack_.erase(undoStack_.begin());
    }
}

void SkiaDrawingEngine::recordPixelEraseCircleAdded(size_t strokeIndex, const EraserCircle& circle) {
    // Find an existing entry for this stroke in the in-flight pixel
    // erase delta, or create one. Most eraser drags touch a small set
    // of strokes repeatedly so the linear scan is cheap.
    for (auto& entry : pendingPixelEraseEntries_) {
        if (entry.strokeIndex == strokeIndex) {
            entry.addedCircles.push_back(circle);
            return;
        }
    }
    StrokeDelta::PixelEraseEntry entry;
    entry.strokeIndex = strokeIndex;
    entry.addedCircles.push_back(circle);
    pendingPixelEraseEntries_.push_back(std::move(entry));
}

void SkiaDrawingEngine::applyDelta(const StrokeDelta& delta) {
    // Forward direction (used by redo).
    switch (delta.kind) {
        case StrokeDelta::Kind::AddStrokes: {
            for (const auto& s : delta.addedStrokes) {
                strokes_.push_back(s);
            }
            break;
        }
        case StrokeDelta::Kind::RemoveStrokes: {
            // Erase in DESCENDING order so each erase doesn't
            // invalidate the indices of yet-to-process entries.
            for (auto it = delta.removedStrokes.rbegin(); it != delta.removedStrokes.rend(); ++it) {
                if (it->first < strokes_.size()) {
                    strokes_.erase(strokes_.begin() + static_cast<long>(it->first));
                }
            }
            break;
        }
        case StrokeDelta::Kind::PixelErase: {
            for (const auto& entry : delta.pixelEraseEntries) {
                if (entry.strokeIndex >= strokes_.size()) continue;
                auto& target = strokes_[entry.strokeIndex].erasedBy;
                for (const auto& c : entry.addedCircles) target.push_back(c);
                strokes_[entry.strokeIndex].cachedEraserCount = 0; // invalidate
            }
            break;
        }
        case StrokeDelta::Kind::MoveStrokes: {
            for (size_t idx : delta.moveIndices) {
                if (idx >= strokes_.size()) continue;
                auto& s = strokes_[idx];
                for (auto& p : s.points) { p.x += delta.moveDx; p.y += delta.moveDy; }
                for (auto& c : s.erasedBy) { c.x += delta.moveDx; c.y += delta.moveDy; }
                pathRenderer_->smoothPath(s.points, s.path);
                s.cachedEraserCount = 0; // path-stamped circles need re-render
            }
            break;
        }
        case StrokeDelta::Kind::ReplaceStrokes: {
            for (const auto& [idx, stroke] : delta.afterStrokes) {
                if (idx < strokes_.size()) {
                    strokes_[idx] = stroke;
                }
            }
            break;
        }
        case StrokeDelta::Kind::Clear: {
            strokes_.clear();
            eraserCircles_.clear();
            break;
        }
    }
}

void SkiaDrawingEngine::revertDelta(const StrokeDelta& delta) {
    // Backward direction (used by undo).
    switch (delta.kind) {
        case StrokeDelta::Kind::AddStrokes: {
            // Pop the same number we appended. Caller invariant: redo
            // didn't run other ops in between (we cleared redo on commit
            // and applied/reverted strictly in stack order).
            for (size_t i = 0; i < delta.addedStrokes.size(); ++i) {
                if (!strokes_.empty()) strokes_.pop_back();
            }
            break;
        }
        case StrokeDelta::Kind::RemoveStrokes: {
            // Re-insert in ASCENDING order. Earlier insertions shift
            // later targets into the right places.
            for (const auto& [idx, stroke] : delta.removedStrokes) {
                size_t clamped = std::min(idx, strokes_.size());
                strokes_.insert(strokes_.begin() + static_cast<long>(clamped), stroke);
            }
            break;
        }
        case StrokeDelta::Kind::PixelErase: {
            for (const auto& entry : delta.pixelEraseEntries) {
                if (entry.strokeIndex >= strokes_.size()) continue;
                auto& target = strokes_[entry.strokeIndex].erasedBy;
                for (size_t i = 0; i < entry.addedCircles.size() && !target.empty(); ++i) {
                    target.pop_back();
                }
                strokes_[entry.strokeIndex].cachedEraserCount = 0;
            }
            break;
        }
        case StrokeDelta::Kind::MoveStrokes: {
            for (size_t idx : delta.moveIndices) {
                if (idx >= strokes_.size()) continue;
                auto& s = strokes_[idx];
                for (auto& p : s.points) { p.x -= delta.moveDx; p.y -= delta.moveDy; }
                for (auto& c : s.erasedBy) { c.x -= delta.moveDx; c.y -= delta.moveDy; }
                pathRenderer_->smoothPath(s.points, s.path);
                s.cachedEraserCount = 0;
            }
            break;
        }
        case StrokeDelta::Kind::ReplaceStrokes: {
            for (const auto& [idx, stroke] : delta.beforeStrokes) {
                if (idx < strokes_.size()) {
                    strokes_[idx] = stroke;
                }
            }
            break;
        }
        case StrokeDelta::Kind::Clear: {
            strokes_ = delta.clearedStrokes;
            eraserCircles_ = delta.clearedEraserCircles;
            break;
        }
    }
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
