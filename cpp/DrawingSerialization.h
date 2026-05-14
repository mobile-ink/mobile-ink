#pragma once

#include <vector>
#include <cstdint>
#include "DrawingTypes.h"

namespace nativedrawing {

/**
 * DrawingSerialization - Handles stroke data serialization/deserialization
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Provides binary serialization format for saving and loading drawings.
 *
 * Format version history:
 * - Version 1: Initial format with strokes, points, paint properties
 * - Version 2: Added originalAlphaMod field for consistent opacity after stroke splitting
 * - Version 3: Added toolType field for specialized rendering (e.g., crayon texture)
 * - Version 4: Added erasedBy per-stroke eraser circles for pixel-perfect erasing
 */
class DrawingSerialization {
public:
    static constexpr uint32_t CURRENT_VERSION = 4;

    DrawingSerialization() = default;

    // Serialize strokes to binary format
    std::vector<uint8_t> serialize(const std::vector<Stroke>& strokes);

    // Deserialize binary data to strokes
    // Returns true on success, false on error
    bool deserialize(
        const std::vector<uint8_t>& data,
        std::vector<Stroke>& strokes
    );

private:
    // Helper to recreate path from points
    void smoothPath(const std::vector<Point>& points, SkPath& path);

    // Write helpers
    void writeUint32(std::vector<uint8_t>& buffer, uint32_t value);
    void writeFloat(std::vector<uint8_t>& buffer, float value);
    void writeByte(std::vector<uint8_t>& buffer, uint8_t value);

    // Read helpers
    bool readUint32(const std::vector<uint8_t>& data, size_t& offset, uint32_t& value);
    bool readFloat(const std::vector<uint8_t>& data, size_t& offset, float& value);
    bool readByte(const std::vector<uint8_t>& data, size_t& offset, uint8_t& value);
};

} // namespace nativedrawing
