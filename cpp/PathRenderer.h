#pragma once

#include <vector>
#include <unordered_map>
#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>
#include <include/core/SkPaint.h>
#include <include/core/SkShader.h>
#include "DrawingTypes.h"

namespace nativedrawing {

/**
 * PathRenderer - Handles variable-width stroke path rendering
 *
 * Extracted from SkiaDrawingEngine for maintainability.
 * Provides pressure-sensitive path rendering using:
 * - Catmull-Rom spline interpolation for smooth curves
 * - Variable-width filled polygons based on pressure/tilt
 * - Pressure-based alpha modulation for expressiveness
 * - Specialized renderers for different tools (e.g., crayon with texture)
 */
// Result from incremental rendering - provides edge data for seamless continuation
struct IncrementalResult {
    SkPoint lastLeftEdge;
    SkPoint lastRightEdge;
    float lastPressure;
    float lastAngle;
    float lastHalfWidth;  // For calligraphy width continuity
    size_t smoothedPointsRendered;
};

class PathRenderer {
public:
    PathRenderer() = default;

    // Draw a variable-width path based on point data
    // applyPressureAlpha: If true, modulates alpha based on average pressure
    //                     Set to false when alpha is pre-applied (e.g., from stored originalAlphaMod)
    void drawVariableWidthPath(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        bool applyPressureAlpha = true
    );

    // Draw a crayon-style path with waxy texture using Perlin noise
    // Pressure affects texture coverage: light = more gaps, heavy = more solid
    void drawCrayonPath(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        bool applyPressureAlpha = true
    );

    // ===== INCREMENTAL RENDERING METHODS =====
    // These render only a portion of the stroke, connecting seamlessly to previous segments

    // Incremental crayon rendering - renders segment and returns edge data
    IncrementalResult drawCrayonPathIncremental(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        const SkPoint& prevLeftEdge,
        const SkPoint& prevRightEdge,
        bool isFirstSegment
    );

    // Draw crayon end caps (start and end semicircles with texture)
    // Call this after incremental rendering to complete the stroke
    void drawCrayonEndCaps(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint
    );

    // Draw just the crayon start cap (for live rendering)
    void drawCrayonStartCap(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint
    );

    // Draw just the crayon end cap (for final tail rendering)
    void drawCrayonEndCap(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint
    );

    // Draw crayon "tail" - recent points not yet finalized (no end cap)
    void drawCrayonPathTail(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        const SkPoint& prevLeftEdge,
        const SkPoint& prevRightEdge,
        bool hasPreviousEdge
    );

    // Incremental variable-width path rendering
    IncrementalResult drawVariableWidthPathIncremental(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        const SkPoint& prevLeftEdge,
        const SkPoint& prevRightEdge,
        bool isFirstSegment
    );

    // Draw variable-width end caps (start and end semicircles)
    // Call this after incremental rendering to complete the stroke
    void drawVariableWidthEndCaps(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint
    );

    // Draw just the start cap (for live rendering)
    void drawVariableWidthStartCap(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint
    );

    // Draw just the end cap (for live rendering tail)
    void drawVariableWidthEndCap(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint
    );

    // Draw variable-width "tail" - recent points not yet finalized
    void drawVariableWidthPathTail(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        const SkPoint& prevLeftEdge,
        const SkPoint& prevRightEdge,
        bool hasPreviousEdge
    );

    // ===== CALLIGRAPHY BRUSH METHODS =====
    // Pointed/flex nib style: thin upstrokes, thick downstrokes
    // Width varies based on pressure, velocity, and stroke direction

    // Draw a calligraphy-style path with direction-sensitive width
    void drawCalligraphyPath(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        bool applyPressureAlpha = true
    );

    // Incremental calligraphy rendering
    // initialHalfWidth: previous segment's final width for continuity (-1 for default)
    IncrementalResult drawCalligraphyPathIncremental(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        const SkPoint& prevLeftEdge,
        const SkPoint& prevRightEdge,
        bool isFirstSegment,
        float initialHalfWidth = -1.0f
    );

    // Draw calligraphy "tail" - recent points not yet finalized
    void drawCalligraphyPathTail(
        SkCanvas* canvas,
        const std::vector<Point>& points,
        const SkPaint& basePaint,
        const SkPoint& prevLeftEdge,
        const SkPoint& prevRightEdge,
        bool hasPreviousEdge,
        float initialHalfWidth = -1.0f
    );

    // Calculate stroke width based on pressure and altitude.
    // The live iOS pen path can opt into a Pencil-tuned profile without affecting
    // legacy rendering or other tools.
    float calculateWidth(
        float pressure,
        float altitude,
        float baseWidth,
        const std::string& toolType = "pen",
        bool useEnhancedPenProfile = false
    );

    // Create smooth path from points using Catmull-Rom splines
    void smoothPath(const std::vector<Point>& points, SkPath& path);

    // Clear cached shaders (call when switching colors or resetting)
    void clearShaderCache();

private:
    // Number of interpolated points between each input point pair
    static constexpr int SEGMENTS_PER_SPAN = 12;
    static constexpr int LIVE_SEGMENTS_PER_SPAN = 8;

    // Tension for Catmull-Rom splines (lower = smoother)
    static constexpr float SPLINE_TENSION = 0.04f;
    static constexpr float LIVE_SPLINE_TENSION = 0.05f;

    // Create crayon noise shader for waxy texture effect
    // pressure: 0.0-1.0, affects noise intensity (low = more gaps)
    // width: stroke width, affects noise frequency scaling
    // strokeAngle: average stroke direction in radians, noise rotates to follow
    sk_sp<SkShader> createCrayonShader(SkColor baseColor, float pressure, float width, float strokeAngle);

    // ===== SHADER CACHING FOR PERFORMANCE =====
    // Cache key combines: color (32 bits) + pressure level (5 bits) + angle bucket (4 bits)
    // This allows reusing shaders for similar pressure/angle batches
    std::unordered_map<uint64_t, sk_sp<SkShader>> shaderCache_;

    // Generate cache key for shader lookup
    uint64_t getShaderCacheKey(SkColor color, float pressure, float angle);

    // Helper for Catmull-Rom spline interpolation
    // Generates smoothed intermediate points from input points
    // interpolatePressure: if true, also interpolates pressure values
    void interpolateSplinePoints(
        const std::vector<Point>& points,
        std::vector<Point>& smoothedPoints,
        bool interpolatePressure = false,
        int segmentsPerSpan = SEGMENTS_PER_SPAN,
        float splineTension = SPLINE_TENSION
    );

    // Build left/right edge points from smoothed points
    // Returns false if not enough points to build edges
    struct EdgePoint {
        SkPoint left;
        SkPoint right;
        float pressure;
        float angle;
    };
    bool buildEdgePoints(
        const std::vector<Point>& smoothedPoints,
        std::vector<EdgePoint>& edges
    );

    // ===== CALLIGRAPHY HELPERS =====
    // Calculate velocity between two points (pixels per second)
    static float calculateVelocity(const Point& current, const Point& previous);

    // Calculate vertical direction (-1 = upstroke, +1 = downstroke, 0 = horizontal)
    static float calculateVerticalDirection(const Point& current, const Point& previous);

    // Calculate calligraphy width based on pressure, velocity, and direction
    float calculateCalligraphyWidth(
        const Point& current,
        const Point& previous,
        float baseWidth,
        float velocity,
        float verticalDirection
    );

    // Build edge points with calligraphy-specific width calculation
    // initialHalfWidth: starting width for rate limiting (use -1 for default baseWidth/2)
    // outFinalHalfWidth: receives the final half width for continuity (can be nullptr)
    bool buildCalligraphyEdgePoints(
        const std::vector<Point>& smoothedPoints,
        std::vector<EdgePoint>& edges,
        float baseWidth,
        float initialHalfWidth = -1.0f,
        float* outFinalHalfWidth = nullptr
    );
};

} // namespace nativedrawing
