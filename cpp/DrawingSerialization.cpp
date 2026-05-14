#include "DrawingSerialization.h"
#include "ShapeRecognition.h"
#include <include/core/SkBlendMode.h>
#include <include/core/SkPathMeasure.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace nativedrawing {

namespace {
constexpr uint32_t MAX_STROKES = 100000;
constexpr uint32_t MAX_POINTS_PER_STROKE = 1000000;
constexpr uint32_t MAX_AFFECTED_INDICES = 1000000;
constexpr uint32_t MAX_TOOL_TYPE_LENGTH = 1024;
constexpr uint32_t MAX_ERASED_BY_CIRCLES = 1000000;
}

std::vector<uint8_t> DrawingSerialization::serialize(const std::vector<Stroke>& strokes) {
    std::vector<uint8_t> buffer;

    // Write header: version number
    writeUint32(buffer, CURRENT_VERSION);

    // Write number of strokes
    writeUint32(buffer, static_cast<uint32_t>(strokes.size()));

    // Write each stroke
    for (const auto& stroke : strokes) {
        // Write isEraser flag
        writeByte(buffer, stroke.isEraser ? 1 : 0);

        // Write paint color (ARGB)
        SkColor color = stroke.paint.getColor();
        writeUint32(buffer, color);

        // Write stroke width
        writeFloat(buffer, stroke.paint.getStrokeWidth());

        // Write alpha (uint8_t)
        writeByte(buffer, stroke.paint.getAlpha());

        // Write number of points
        writeUint32(buffer, static_cast<uint32_t>(stroke.points.size()));

        // Write each point
        for (const auto& point : stroke.points) {
            writeFloat(buffer, point.x);
            writeFloat(buffer, point.y);
            writeFloat(buffer, point.pressure);
            writeFloat(buffer, point.azimuthAngle);
            writeFloat(buffer, point.altitude);
            writeFloat(buffer, point.calculatedWidth);
        }

        // Write affectedStrokeIndices count
        writeUint32(buffer, static_cast<uint32_t>(stroke.affectedStrokeIndices.size()));

        // Write each affected index in SORTED order. The container is
        // std::unordered_set whose iteration order is undefined, so writing
        // in iteration order produced different bytes for identical logical
        // state across runs. That non-determinism cascaded all the way up
        // through decomposeContinuousWindow -> JS pages[].data -> JS
        // serializeNotebookData -> the autosave's "did data change?" check
        // in onDataChange, which kept flipping setUnsavedChanges(true) and
        // restarting the autosave loop while the user was idle. Linear
        // memory growth and the eventual OOM all trace back to this loop.
        std::vector<size_t> sortedIndices(
            stroke.affectedStrokeIndices.begin(),
            stroke.affectedStrokeIndices.end()
        );
        std::sort(sortedIndices.begin(), sortedIndices.end());
        for (size_t idx : sortedIndices) {
            writeUint32(buffer, static_cast<uint32_t>(idx));
        }

        // Write originalAlphaMod (version 2+)
        writeFloat(buffer, stroke.originalAlphaMod);

        // Write toolType string (version 3+)
        writeUint32(buffer, static_cast<uint32_t>(stroke.toolType.length()));
        for (char c : stroke.toolType) {
            writeByte(buffer, static_cast<uint8_t>(c));
        }

        // Write erasedBy circles (version 4+)
        writeUint32(buffer, static_cast<uint32_t>(stroke.erasedBy.size()));
        for (const auto& circle : stroke.erasedBy) {
            writeFloat(buffer, circle.x);
            writeFloat(buffer, circle.y);
            writeFloat(buffer, circle.radius);
        }
    }

    return buffer;
}

bool DrawingSerialization::deserialize(
    const std::vector<uint8_t>& data,
    std::vector<Stroke>& strokes
) {
    if (data.size() < 8) {
        printf("[C++] Drawing payload too small to deserialize: %zu bytes\n", data.size());
        return false;
    }

    size_t offset = 0;

    // Read version
    uint32_t version = 0;
    if (!readUint32(data, offset, version)) {
        printf("[C++] Failed to read drawing serialization version\n");
        return false;
    }

    if (version < 1 || version > 4) {
        printf("[C++] Unsupported serialization version: %d\n", version);
        return false;
    }

    // Read number of strokes
    uint32_t strokeCount = 0;
    if (!readUint32(data, offset, strokeCount)) {
        printf("[C++] Failed to read stroke count\n");
        return false;
    }

    if (strokeCount > MAX_STROKES) {
        printf("[C++] Unreasonable stroke count in drawing payload: %u\n", strokeCount);
        return false;
    }

    // Clear current drawing
    strokes.clear();
    strokes.reserve(strokeCount);

    // Read each stroke
    for (uint32_t i = 0; i < strokeCount; ++i) {
        Stroke stroke;

        // Read isEraser flag
        uint8_t isEraser = 0;
        if (!readByte(data, offset, isEraser)) {
            printf("[C++] Failed to read eraser flag for stroke %u\n", i);
            return false;
        }
        stroke.isEraser = isEraser != 0;

        // Read paint color
        uint32_t colorValue = 0;
        if (!readUint32(data, offset, colorValue)) {
            printf("[C++] Failed to read paint color for stroke %u\n", i);
            return false;
        }
        SkColor color = colorValue;

        // Read stroke width
        float width = 0.0f;
        if (!readFloat(data, offset, width)) {
            printf("[C++] Failed to read stroke width for stroke %u\n", i);
            return false;
        }

        // Read alpha
        uint8_t alpha = 0;
        if (!readByte(data, offset, alpha)) {
            printf("[C++] Failed to read stroke alpha for stroke %u\n", i);
            return false;
        }

        // Setup paint
        stroke.paint.setAntiAlias(true);
        stroke.paint.setStyle(SkPaint::kStroke_Style);
        stroke.paint.setStrokeWidth(width);
        stroke.paint.setColor(color);
        stroke.paint.setAlpha(alpha);
        stroke.paint.setStrokeCap(SkPaint::kRound_Cap);
        stroke.paint.setStrokeJoin(SkPaint::kRound_Join);

        // Set blend mode for eraser strokes
        if (stroke.isEraser) {
            stroke.paint.setBlendMode(SkBlendMode::kDstOut);
        }

        // Read number of points
        uint32_t pointCount = 0;
        if (!readUint32(data, offset, pointCount)) {
            printf("[C++] Failed to read point count for stroke %u\n", i);
            return false;
        }

        const size_t remainingBytesForPoints = data.size() - offset;
        const size_t bytesPerPoint = sizeof(float) * 6;
        if (pointCount > MAX_POINTS_PER_STROKE ||
            pointCount > remainingBytesForPoints / bytesPerPoint) {
            printf("[C++] Invalid point count for stroke %u: %u\n", i, pointCount);
            return false;
        }

        stroke.points.reserve(pointCount);

        // Read each point
        for (uint32_t j = 0; j < pointCount; ++j) {
            Point point;
            if (!readFloat(data, offset, point.x) ||
                !readFloat(data, offset, point.y) ||
                !readFloat(data, offset, point.pressure) ||
                !readFloat(data, offset, point.azimuthAngle) ||
                !readFloat(data, offset, point.altitude) ||
                !readFloat(data, offset, point.calculatedWidth)) {
                printf("[C++] Failed to read point %u for stroke %u\n", j, i);
                return false;
            }
            point.timestamp = 0; // Not serialized
            stroke.points.push_back(point);
        }

        // Read affectedStrokeIndices count
        uint32_t affectedCount = 0;
        if (!readUint32(data, offset, affectedCount)) {
            printf("[C++] Failed to read affected stroke count for stroke %u\n", i);
            return false;
        }

        if (affectedCount > MAX_AFFECTED_INDICES ||
            affectedCount > (data.size() - offset) / sizeof(uint32_t)) {
            printf("[C++] Invalid affected stroke count for stroke %u: %u\n", i, affectedCount);
            return false;
        }

        // Read each affected index
        for (uint32_t j = 0; j < affectedCount; ++j) {
            uint32_t index32 = 0;
            if (!readUint32(data, offset, index32)) {
                printf("[C++] Failed to read affected stroke index %u for stroke %u\n", j, i);
                return false;
            }
            stroke.affectedStrokeIndices.insert(static_cast<size_t>(index32));
        }

        // Read originalAlphaMod (version 2+) or calculate from points (version 1)
        if (version >= 2) {
            if (!readFloat(data, offset, stroke.originalAlphaMod)) {
                printf("[C++] Failed to read original alpha modifier for stroke %u\n", i);
                return false;
            }
        } else {
            // Version 1: Calculate from points for backwards compatibility
            if (!stroke.points.empty()) {
                float avgPressure = 0.0f;
                for (const auto& pt : stroke.points) {
                    avgPressure += pt.pressure;
                }
                avgPressure /= stroke.points.size();
                stroke.originalAlphaMod = 0.7f + (avgPressure * 0.3f);
            }
        }

        // Read toolType (version 3+) or default to "pen"
        if (version >= 3) {
            uint32_t toolTypeLen = 0;
            if (!readUint32(data, offset, toolTypeLen)) {
                printf("[C++] Failed to read tool type length for stroke %u\n", i);
                return false;
            }

            if (toolTypeLen > MAX_TOOL_TYPE_LENGTH || toolTypeLen > data.size() - offset) {
                printf("[C++] Invalid tool type length for stroke %u: %u\n", i, toolTypeLen);
                return false;
            }

            stroke.toolType.clear();
            stroke.toolType.reserve(toolTypeLen);
            for (uint32_t j = 0; j < toolTypeLen; ++j) {
                uint8_t toolTypeByte = 0;
                if (!readByte(data, offset, toolTypeByte)) {
                    printf("[C++] Failed to read tool type byte %u for stroke %u\n", j, i);
                    return false;
                }
                stroke.toolType += static_cast<char>(toolTypeByte);
            }
        } else {
            stroke.toolType = "pen";  // Default for older versions
        }

        // Read erasedBy circles (version 4+) or leave empty for older versions
        if (version >= 4) {
            uint32_t erasedByCount = 0;
            if (!readUint32(data, offset, erasedByCount)) {
                printf("[C++] Failed to read erased-by count for stroke %u\n", i);
                return false;
            }

            const size_t remainingBytesForEraserData = data.size() - offset;
            const size_t bytesPerCircle = sizeof(float) * 3;
            if (erasedByCount > MAX_ERASED_BY_CIRCLES ||
                erasedByCount > remainingBytesForEraserData / bytesPerCircle) {
                printf("[C++] Invalid erased-by count for stroke %u: %u\n", i, erasedByCount);
                return false;
            }

            stroke.erasedBy.clear();
            stroke.erasedBy.reserve(erasedByCount);
            for (uint32_t j = 0; j < erasedByCount; ++j) {
                EraserCircle circle;
                if (!readFloat(data, offset, circle.x) ||
                    !readFloat(data, offset, circle.y) ||
                    !readFloat(data, offset, circle.radius)) {
                    printf("[C++] Failed to read erased-by circle %u for stroke %u\n", j, i);
                    return false;
                }
                stroke.erasedBy.push_back(circle);
            }

            // Compute cached visibility for selection optimization
            if (!stroke.erasedBy.empty()) {
                stroke.cachedHasVisiblePoints = false;
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
                        stroke.cachedHasVisiblePoints = true;
                        break;
                    }
                }
            }
        }
        // Version < 4: erasedBy stays empty (no per-stroke eraser data)

        // Recreate path from points after toolType is known so recognized
        // shapes restore exact geometry instead of a smoothed freehand path.
        if (!buildRecognizedShapePath(stroke.toolType, stroke.points, stroke.path)) {
            smoothPath(stroke.points, stroke.path);
        }

        SkPathMeasure pathMeasure(stroke.path, false);
        stroke.pathLength = pathMeasure.getLength();

        strokes.push_back(stroke);
    }

    if (offset > data.size()) {
        printf("[C++] Drawing payload read past end of buffer\n");
        return false;
    }

    return true;
}

void DrawingSerialization::smoothPath(const std::vector<Point>& points, SkPath& path) {
    if (points.empty()) return;

    path.reset();
    path.moveTo(points[0].x, points[0].y);

    if (points.size() == 1) {
        path.lineTo(points[0].x + 0.1f, points[0].y + 0.1f);
        return;
    }

    if (points.size() == 2) {
        path.lineTo(points[1].x, points[1].y);
        return;
    }

    // Use Catmull-Rom splines for ultra-smooth curves
    for (size_t i = 0; i < points.size() - 1; i++) {
        Point p0, p1, p2, p3;

        p1 = points[i];
        p2 = points[i + 1];

        if (i == 0) {
            p0 = p1;
        } else {
            p0 = points[i - 1];
        }

        if (i + 2 >= points.size()) {
            p3 = p2;
        } else {
            p3 = points[i + 2];
        }

        const float tension = 0.08f;

        float c1x = p1.x + (p2.x - p0.x) / 6.0f * tension;
        float c1y = p1.y + (p2.y - p0.y) / 6.0f * tension;
        float c2x = p2.x - (p3.x - p1.x) / 6.0f * tension;
        float c2y = p2.y - (p3.y - p1.y) / 6.0f * tension;

        path.cubicTo(c1x, c1y, c2x, c2y, p2.x, p2.y);
    }
}

void DrawingSerialization::writeUint32(std::vector<uint8_t>& buffer, uint32_t value) {
    buffer.insert(buffer.end(), (uint8_t*)&value, (uint8_t*)&value + 4);
}

void DrawingSerialization::writeFloat(std::vector<uint8_t>& buffer, float value) {
    buffer.insert(buffer.end(), (uint8_t*)&value, (uint8_t*)&value + 4);
}

void DrawingSerialization::writeByte(std::vector<uint8_t>& buffer, uint8_t value) {
    buffer.push_back(value);
}

bool DrawingSerialization::readUint32(const std::vector<uint8_t>& data, size_t& offset, uint32_t& value) {
    if (offset + sizeof(uint32_t) > data.size()) {
        return false;
    }
    std::memcpy(&value, &data[offset], sizeof(uint32_t));
    offset += sizeof(uint32_t);
    return true;
}

bool DrawingSerialization::readFloat(const std::vector<uint8_t>& data, size_t& offset, float& value) {
    if (offset + sizeof(float) > data.size()) {
        return false;
    }
    std::memcpy(&value, &data[offset], sizeof(float));
    offset += sizeof(float);
    return true;
}

bool DrawingSerialization::readByte(const std::vector<uint8_t>& data, size_t& offset, uint8_t& value) {
    if (offset + sizeof(uint8_t) > data.size()) {
        return false;
    }
    value = data[offset++];
    return true;
}

} // namespace nativedrawing
