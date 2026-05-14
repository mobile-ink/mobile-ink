#pragma once

#include "SkiaDrawingEngine.h"
#include <string>
#include <vector>

namespace nativedrawing {

struct ShapeCandidate {
    bool recognized = false;
    std::string toolType;
    std::vector<Point> points;
};

bool isRecognizedShapeToolType(const std::string& toolType);

bool buildRecognizedShapePath(
    const std::string& toolType,
    const std::vector<Point>& points,
    SkPath& path
);

float distanceBetween(const Point& a, const Point& b);
float averageCalculatedWidth(const std::vector<Point>& points);
float averagePressure(const std::vector<Point>& points);

Point centerPointForPoints(const std::vector<Point>& points);
std::vector<Point> transformedShapePoints(
    const std::vector<Point>& basePoints,
    const Point& anchor,
    float referenceAngle,
    float referenceDistance,
    float targetX,
    float targetY,
    float scaleSensitivity,
    float rotationSensitivity
);
std::vector<Point> transformedShapePointsCenterLockedToTarget(
    const std::vector<Point>& basePoints,
    const Point& center,
    float referenceAngle,
    float referenceDistance,
    float targetX,
    float targetY
);

ShapeCandidate recognizeHeldShape(
    const std::vector<Point>& points,
    const std::string& currentTool,
    long endTimestamp
);
void snapRecognizedShapeCandidateToStrokes(
    ShapeCandidate& candidate,
    const std::vector<Stroke>& strokes,
    float averageWidth
);

}  // namespace nativedrawing
