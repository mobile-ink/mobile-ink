#include "ShapeRecognition.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nativedrawing {

constexpr long kHoldToShapeDurationMs = 300;
constexpr float kMinimumShapeDiagonal = 30.0f;
constexpr float kPi = 3.14159265358979323846f;

bool isRecognizedShapeToolType(const std::string& toolType) {
    return toolType == "shape-line"
        || toolType == "shape-rectangle"
        || toolType == "shape-circle"
        || toolType == "shape-ellipse"
        || toolType == "shape-polygon";
}

bool buildRecognizedShapePath(
    const std::string& toolType,
    const std::vector<Point>& points,
    SkPath& path
) {
    if (!isRecognizedShapeToolType(toolType) || points.size() < 2) {
        return false;
    }

    path.reset();

    if (toolType == "shape-line") {
        path.moveTo(points.front().x, points.front().y);
        path.lineTo(points.back().x, points.back().y);
        return true;
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

    if (std::fabs(maxX - minX) < 0.001f || std::fabs(maxY - minY) < 0.001f) {
        return false;
    }

    const SkRect bounds = SkRect::MakeLTRB(minX, minY, maxX, maxY);

    if (toolType == "shape-polygon" || (toolType == "shape-rectangle" && points.size() >= 4)) {
        if (points.size() < 3) {
            return false;
        }

        path.moveTo(points.front().x, points.front().y);
        for (size_t i = 1; i < points.size(); ++i) {
            path.lineTo(points[i].x, points[i].y);
        }
        path.close();
        return true;
    }

    if ((toolType == "shape-circle" || toolType == "shape-ellipse") && points.size() >= 6) {
        path.moveTo(points.front().x, points.front().y);
        for (size_t i = 1; i < points.size(); ++i) {
            path.lineTo(points[i].x, points[i].y);
        }
        path.close();
        return true;
    }

    if (toolType == "shape-rectangle") {
        path.addRect(bounds);
        return true;
    }

    if (toolType == "shape-circle" || toolType == "shape-ellipse") {
        path.addOval(bounds);
        return true;
    }

    return false;
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


}  // namespace nativedrawing
