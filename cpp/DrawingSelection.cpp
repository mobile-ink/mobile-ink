#include "DrawingSelection.h"
#include "ShapeRecognition.h"
#include <include/effects/SkDashPathEffect.h>
#include <algorithm>
#include <cmath>

namespace nativedrawing {

namespace {

SkColor selectionChromeBlue(uint8_t alpha) {
    // The live MTKView path lands on-screen with red/blue swapped.
    // Ask Skia for the swapped value so the visible chrome is iOS blue.
    return SkColorSetARGB(alpha, 255, 122, 0);
}

}  // namespace

// Helper: Check if a point is within any eraser circle for a stroke
static bool isPointInErasedRegion(float x, float y, const Stroke& stroke) {
    for (const auto& circle : stroke.erasedBy) {
        float dx = x - circle.x;
        float dy = y - circle.y;
        if (dx*dx + dy*dy <= circle.radius * circle.radius) {
            return true;  // Point is inside an eraser circle
        }
    }
    return false;
}

// Helper: Check if a stroke has any visible (non-erased) points
static bool hasVisiblePoints(const Stroke& stroke) {
    if (stroke.erasedBy.empty()) return true;  // No erasure = fully visible

    // Check if any stroke point is outside all eraser circles
    for (const auto& pt : stroke.points) {
        bool pointVisible = true;
        for (const auto& circle : stroke.erasedBy) {
            float dx = pt.x - circle.x;
            float dy = pt.y - circle.y;
            // Use stroke width to check if the rendered point would be visible
            float strokeRadius = pt.calculatedWidth / 2.0f;
            float totalRadius = circle.radius + strokeRadius;
            if (dx*dx + dy*dy <= totalRadius * totalRadius) {
                pointVisible = false;
                break;
            }
        }
        if (pointVisible) return true;  // Found at least one visible point
    }
    return false;  // All points are erased
}

bool DrawingSelection::selectStrokeAtMatching(
    float x, float y,
    std::vector<Stroke>& strokes,
    std::unordered_set<size_t>& selectedIndices,
    bool shapeStrokesOnly
) {
    // Find the closest stroke to the tap point
    int closestIndex = -1;
    float closestDist = std::numeric_limits<float>::max();

    for (size_t i = 0; i < strokes.size(); i++) {
        // Don't select eraser strokes
        if (strokes[i].isEraser) continue;

        if (shapeStrokesOnly && !isRecognizedShapeToolType(strokes[i].toolType)) {
            continue;
        }

        // OPTIMIZATION: Use cached visibility instead of O(m*k) check
        if (!strokes[i].cachedHasVisiblePoints) continue;

        // Don't select if tap point is in an erased region of this stroke
        if (isPointInErasedRegion(x, y, strokes[i])) continue;

        if (isPointNearStroke(x, y, strokes[i], 30.0f)) {
            // Calculate distance to stroke center for tiebreaking
            SkRect bounds = calculateStrokeBounds(strokes[i].points);
            float centerX = (bounds.fLeft + bounds.fRight) / 2.0f;
            float centerY = (bounds.fTop + bounds.fBottom) / 2.0f;
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = std::sqrt(dx * dx + dy * dy);

            if (dist < closestDist) {
                closestDist = dist;
                closestIndex = i;
            }
        }
    }

    if (closestIndex >= 0) {
        // Toggle selection
        if (selectedIndices.count(closestIndex) > 0) {
            selectedIndices.erase(closestIndex);
        } else {
            selectedIndices.insert(closestIndex);
        }
        return true;
    }

    return false;
}

bool DrawingSelection::selectStrokeAt(
    float x, float y,
    std::vector<Stroke>& strokes,
    std::unordered_set<size_t>& selectedIndices
) {
    return selectStrokeAtMatching(x, y, strokes, selectedIndices, false);
}

bool DrawingSelection::selectShapeStrokeAt(
    float x, float y,
    std::vector<Stroke>& strokes,
    std::unordered_set<size_t>& selectedIndices
) {
    return selectStrokeAtMatching(x, y, strokes, selectedIndices, true);
}

void DrawingSelection::clearSelection(std::unordered_set<size_t>& selectedIndices) {
    selectedIndices.clear();
}

void DrawingSelection::deleteSelection(
    std::vector<Stroke>& strokes,
    std::unordered_set<size_t>& selectedIndices,
    const DeltaCommitter& commit
) {
    if (selectedIndices.empty()) return;

    // Build a sorted list of indices to remove and capture (idx, stroke)
    // pairs before mutating strokes_ -- the delta needs them to support
    // undo (re-insert at original indices).
    std::vector<size_t> sortedIndices(selectedIndices.begin(), selectedIndices.end());
    std::sort(sortedIndices.begin(), sortedIndices.end());

    StrokeDelta delta;
    delta.kind = StrokeDelta::Kind::RemoveStrokes;
    delta.removedStrokes.reserve(sortedIndices.size());
    for (size_t idx : sortedIndices) {
        if (idx < strokes.size()) {
            delta.removedStrokes.emplace_back(idx, strokes[idx]);
        }
    }

    // Build a mapping of old indices to new indices
    std::unordered_map<size_t, size_t> oldToNewIndex;
    size_t newIndex = 0;

    std::vector<Stroke> remainingStrokes;
    remainingStrokes.reserve(strokes.size() - selectedIndices.size());

    for (size_t i = 0; i < strokes.size(); i++) {
        if (selectedIndices.count(i) == 0) {
            oldToNewIndex[i] = newIndex++;
            remainingStrokes.push_back(strokes[i]);
        }
    }

    // Update affectedStrokeIndices in remaining eraser strokes
    for (auto& stroke : remainingStrokes) {
        if (stroke.isEraser) {
            std::unordered_set<size_t> updatedIndices;
            for (size_t oldIdx : stroke.affectedStrokeIndices) {
                // Only keep references to strokes that weren't deleted
                if (oldToNewIndex.count(oldIdx) > 0) {
                    updatedIndices.insert(oldToNewIndex[oldIdx]);
                }
            }
            stroke.affectedStrokeIndices = updatedIndices;
        }
    }

    strokes = remainingStrokes;
    selectedIndices.clear();

    if (commit) commit(std::move(delta));
}

void DrawingSelection::copySelection(
    const std::vector<Stroke>& strokes,
    const std::unordered_set<size_t>& selectedIndices,
    std::vector<Stroke>& copiedStrokes
) {
    copiedStrokes.clear();

    // First, copy all selected strokes
    for (size_t idx : selectedIndices) {
        if (idx < strokes.size()) {
            copiedStrokes.push_back(strokes[idx]);
        }
    }

    // Then, copy any eraser strokes that affect the selected strokes
    for (size_t i = 0; i < strokes.size(); ++i) {
        if (!strokes[i].isEraser) continue;

        // Check if this eraser affects any selected stroke
        for (size_t selectedIdx : selectedIndices) {
            if (strokes[i].affectedStrokeIndices.count(selectedIdx) > 0) {
                copiedStrokes.push_back(strokes[i]);
                break;
            }
        }
    }
}

void DrawingSelection::pasteSelection(
    std::vector<Stroke>& strokes,
    const std::vector<Stroke>& copiedStrokes,
    float offsetX, float offsetY,
    const DeltaCommitter& commit
) {
    if (copiedStrokes.empty()) return;

    size_t pasteStartIndex = strokes.size();

    // Map old indices to new indices for updating eraser references
    std::unordered_map<size_t, size_t> oldToNewIndex;
    size_t baseIndex = strokes.size();

    // First pass: Add all copied strokes with offset
    for (size_t i = 0; i < copiedStrokes.size(); ++i) {
        Stroke newStroke = copiedStrokes[i];

        // Offset all points
        for (auto& point : newStroke.points) {
            point.x += offsetX;
            point.y += offsetY;
        }

        // Offset per-stroke eraser circles (so erased regions stay with stroke)
        for (auto& circle : newStroke.erasedBy) {
            circle.x += offsetX;
            circle.y += offsetY;
        }

        // Recreate path with offset
        if (!buildRecognizedShapePath(newStroke.toolType, newStroke.points, newStroke.path)) {
            smoothPath(newStroke.points, newStroke.path);
        }

        // Clear affected stroke indices for now, will update in second pass
        if (newStroke.isEraser) {
            newStroke.affectedStrokeIndices.clear();
        }

        strokes.push_back(newStroke);
    }

    // Second pass: Update eraser stroke references
    size_t copiedStrokeIdx = 0;
    for (size_t i = 0; i < copiedStrokes.size(); ++i) {
        if (!copiedStrokes[i].isEraser) {
            oldToNewIndex[copiedStrokeIdx] = baseIndex + i;
            copiedStrokeIdx++;
        }
    }

    // Update eraser strokes to reference the new pasted stroke indices
    for (size_t i = baseIndex; i < strokes.size(); ++i) {
        if (strokes[i].isEraser && i - baseIndex < copiedStrokes.size()) {
            // Mark all regular pasted strokes as affected
            for (size_t j = 0; j < copiedStrokes.size(); ++j) {
                if (!copiedStrokes[j].isEraser) {
                    strokes[i].affectedStrokeIndices.insert(baseIndex + j);
                }
            }
        }
    }

    // Build a delta describing the appended strokes (from pasteStartIndex
    // to end). On undo, the engine pops_back N strokes; on redo, it
    // re-pushes them. The eraser-strokes' affectedStrokeIndices were
    // built during the paste itself and persist across undo/redo as
    // part of the snapshotted Stroke data in addedStrokes.
    StrokeDelta delta;
    delta.kind = StrokeDelta::Kind::AddStrokes;
    delta.addedStrokes.reserve(strokes.size() - pasteStartIndex);
    for (size_t i = pasteStartIndex; i < strokes.size(); ++i) {
        delta.addedStrokes.push_back(strokes[i]);
    }
    if (commit) commit(std::move(delta));
}

void DrawingSelection::moveSelection(
    std::vector<Stroke>& strokes,
    const std::unordered_set<size_t>& selectedIndices,
    float dx, float dy
) {
    if (selectedIndices.empty()) return;

    // First, find all eraser strokes that affect any selected strokes
    std::unordered_set<size_t> erasersToMove;
    for (size_t i = 0; i < strokes.size(); ++i) {
        if (!strokes[i].isEraser) continue;

        // Check if this eraser affects any selected stroke
        for (size_t selectedIdx : selectedIndices) {
            if (strokes[i].affectedStrokeIndices.count(selectedIdx) > 0) {
                erasersToMove.insert(i);
                break;
            }
        }
    }

    // Move selected strokes
    for (size_t idx : selectedIndices) {
        if (idx < strokes.size()) {
            // Offset all points in the stroke
            for (auto& point : strokes[idx].points) {
                point.x += dx;
                point.y += dy;
            }

            // Move per-stroke eraser circles (so erased regions move with stroke)
            for (auto& circle : strokes[idx].erasedBy) {
                circle.x += dx;
                circle.y += dy;
            }

            // OPTIMIZATION: Translate cached eraser path instead of invalidating
            // This is O(1) instead of O(n) path ops for rebuild
            if (strokes[idx].cachedEraserCount > 0 && !strokes[idx].cachedEraserPath.isEmpty()) {
                strokes[idx].cachedEraserPath.offset(dx, dy);
            }

            // Recreate path with new positions
            if (!buildRecognizedShapePath(strokes[idx].toolType, strokes[idx].points, strokes[idx].path)) {
                smoothPath(strokes[idx].points, strokes[idx].path);
            }
        }
    }

    // Move associated eraser strokes
    for (size_t idx : erasersToMove) {
        if (idx < strokes.size()) {
            for (auto& point : strokes[idx].points) {
                point.x += dx;
                point.y += dy;
            }
            if (!buildRecognizedShapePath(strokes[idx].toolType, strokes[idx].points, strokes[idx].path)) {
                smoothPath(strokes[idx].points, strokes[idx].path);
            }
        }
    }
}

void DrawingSelection::finalizeMove(
    const std::vector<Stroke>& strokes,
    const std::unordered_set<size_t>& selectedIndices,
    float totalDx, float totalDy,
    const DeltaCommitter& commit
) {
    if (selectedIndices.empty()) return;
    (void)strokes; // unused; included for parity with previous signature

    // Build a MoveStrokes delta. We also need to capture the eraser
    // strokes that moved alongside (for parity with moveSelection's
    // logic that translates erasers affecting the selection). Engine
    // applies (dx, dy) for redo, (-dx, -dy) for undo, and re-runs
    // smoothPath on each affected stroke in either direction so the
    // cached SkPath ends up consistent.
    StrokeDelta delta;
    delta.kind = StrokeDelta::Kind::MoveStrokes;
    delta.moveDx = totalDx;
    delta.moveDy = totalDy;
    delta.moveIndices.reserve(selectedIndices.size());
    for (size_t idx : selectedIndices) {
        delta.moveIndices.push_back(idx);
    }
    if (commit) commit(std::move(delta));
}

int DrawingSelection::getSelectionCount(const std::unordered_set<size_t>& selectedIndices) const {
    return static_cast<int>(selectedIndices.size());
}

std::vector<float> DrawingSelection::getSelectionBounds(
    const std::vector<Stroke>& strokes,
    const std::unordered_set<size_t>& selectedIndices
) {
    if (selectedIndices.empty()) {
        return {0, 0, 0, 0};
    }

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();

    for (size_t idx : selectedIndices) {
        if (idx < strokes.size()) {
            SkRect bounds = calculateStrokeBounds(strokes[idx].points);
            minX = std::min(minX, bounds.fLeft);
            minY = std::min(minY, bounds.fTop);
            maxX = std::max(maxX, bounds.fRight);
            maxY = std::max(maxY, bounds.fBottom);
        }
    }

    // Add padding around selection
    const float padding = 20.0f;
    return {minX - padding, minY - padding, maxX + padding, maxY + padding};
}

bool DrawingSelection::isPointNearStroke(
    float x, float y,
    const Stroke& stroke,
    float tolerance
) {
    if (stroke.points.empty()) return false;

    // First check bounding box for quick rejection
    SkRect bounds = calculateStrokeBounds(stroke.points);
    bounds.outset(tolerance, tolerance);

    if (!bounds.contains(x, y)) {
        return false;
    }

    auto isPointNearSegment = [&](const Point& p1, const Point& p2) -> bool {
        // Calculate distance from point to line segment
        float dx = p2.x - p1.x;
        float dy = p2.y - p1.y;
        float lengthSq = dx * dx + dy * dy;

        if (lengthSq < 0.001f) return false; // Skip near-zero length segments

        float t = std::max(0.0f, std::min(1.0f, ((x - p1.x) * dx + (y - p1.y) * dy) / lengthSq));
        float nearestX = p1.x + t * dx;
        float nearestY = p1.y + t * dy;

        float distX = x - nearestX;
        float distY = y - nearestY;
        float dist = std::sqrt(distX * distX + distY * distY);

        // Account for stroke width
        float strokeWidth = p1.calculatedWidth;
        if (dist <= tolerance + strokeWidth / 2.0f) {
            return true;
        }

        return false;
    };

    // Check distance to each line segment
    for (size_t i = 0; i < stroke.points.size() - 1; i++) {
        if (isPointNearSegment(stroke.points[i], stroke.points[i + 1])) {
            return true;
        }
    }

    // Recognized closed shapes render the final edge via path.close().
    // Keep hit-testing in lockstep with rendering so triangles/polygons
    // remain selectable after serialization reloads and engine reassignment.
    if (
        stroke.points.size() > 2 &&
        isRecognizedShapeToolType(stroke.toolType) &&
        stroke.toolType != "shape-line"
    ) {
        if (isPointNearSegment(stroke.points.back(), stroke.points.front())) {
            return true;
        }
    }

    return false;
}

void DrawingSelection::renderSelection(
    SkCanvas* canvas,
    const std::vector<Stroke>& strokes,
    const std::unordered_set<size_t>& selectedIndices
) {
    if (selectedIndices.empty()) return;

    const std::vector<float> bounds = getSelectionBounds(strokes, selectedIndices);

    if (bounds.size() >= 4 && bounds[2] > bounds[0] && bounds[3] > bounds[1]) {
        SkRect frame = SkRect::MakeLTRB(bounds[0], bounds[1], bounds[2], bounds[3]);

        SkPaint framePaint;
        framePaint.setStyle(SkPaint::kStroke_Style);
        framePaint.setColor(selectionChromeBlue(225));
        framePaint.setStrokeWidth(2.0f);
        framePaint.setAntiAlias(true);
        canvas->drawRect(frame, framePaint);

        SkPaint fillPaint;
        fillPaint.setStyle(SkPaint::kFill_Style);
        fillPaint.setColor(selectionChromeBlue(255));
        fillPaint.setAntiAlias(true);

        SkPaint borderPaint;
        borderPaint.setStyle(SkPaint::kStroke_Style);
        borderPaint.setColor(SK_ColorWHITE);
        borderPaint.setStrokeWidth(2.0f);
        borderPaint.setAntiAlias(true);

        const float centerX = (frame.fLeft + frame.fRight) * 0.5f;
        const float centerY = (frame.fTop + frame.fBottom) * 0.5f;
        const SkPoint handles[] = {
            SkPoint::Make(frame.fLeft, frame.fTop),
            SkPoint::Make(centerX, frame.fTop),
            SkPoint::Make(frame.fRight, frame.fTop),
            SkPoint::Make(frame.fLeft, centerY),
            SkPoint::Make(frame.fRight, centerY),
            SkPoint::Make(frame.fLeft, frame.fBottom),
            SkPoint::Make(centerX, frame.fBottom),
            SkPoint::Make(frame.fRight, frame.fBottom),
        };

        constexpr float handleRadius = 7.0f;
        for (const SkPoint& handle : handles) {
            canvas->drawCircle(handle.x(), handle.y(), handleRadius, fillPaint);
            canvas->drawCircle(handle.x(), handle.y(), handleRadius, borderPaint);
        }
    }
}

SkRect DrawingSelection::calculateStrokeBounds(const std::vector<Point>& points) {
    if (points.empty()) {
        return SkRect::MakeEmpty();
    }

    float minX = points[0].x;
    float minY = points[0].y;
    float maxX = points[0].x;
    float maxY = points[0].y;

    for (const auto& p : points) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }

    return SkRect::MakeLTRB(minX, minY, maxX, maxY);
}

void DrawingSelection::smoothPath(const std::vector<Point>& points, SkPath& path) {
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

// =============================================================================
// Lasso Selection Implementation
// =============================================================================

void DrawingSelection::lassoBegin(float x, float y) {
    lassoActive_ = true;
    lassoPath_.reset();
    lassoPath_.moveTo(x, y);
    lassoPoints_.clear();
    lassoPoints_.push_back({x, y, 0, 0, 0, 0, 0});
    lassoPhase_ = 0.0f;
}

void DrawingSelection::lassoMove(float x, float y) {
    if (!lassoActive_) return;

    // Point decimation for performance - skip points too close together
    if (!lassoPoints_.empty()) {
        const Point& last = lassoPoints_.back();
        float dx = x - last.x;
        float dy = y - last.y;
        if (dx * dx + dy * dy < 9.0f) return;  // Skip if < 3px apart
    }

    lassoPoints_.push_back({x, y, 0, 0, 0, 0, 0});
    lassoPath_.lineTo(x, y);
}

void DrawingSelection::lassoEnd(
    std::vector<Stroke>& strokes,
    std::unordered_set<size_t>& selectedIndices
) {
    if (!lassoActive_ || lassoPoints_.size() < 3) {
        cancelLasso();
        return;
    }

    // Close the lasso path to form a region
    SkPath closedLasso = lassoPath_;
    closedLasso.close();

    // Get lasso bounding box for quick rejection
    SkRect lassoBounds = closedLasso.getBounds();

    // Select strokes that the lasso intersects OR encloses
    for (size_t i = 0; i < strokes.size(); ++i) {
        if (strokes[i].isEraser) continue;
        if (!strokes[i].cachedHasVisiblePoints) continue;

        const Stroke& stroke = strokes[i];
        if (stroke.points.empty()) continue;

        SkRect strokeBounds = calculateStrokeBounds(stroke.points);

        // Quick rejection: skip if bounds don't overlap at all
        if (!strokeBounds.intersects(lassoBounds)) continue;

        bool shouldSelect = false;

        // Check containment: count how many stroke points are inside the lasso
        int containedCount = 0;
        for (const auto& pt : stroke.points) {
            if (closedLasso.contains(pt.x, pt.y)) {
                containedCount++;
            }
        }

        if (containedCount == static_cast<int>(stroke.points.size())) {
            // All points contained - stroke is fully enclosed
            shouldSelect = true;
        } else if (containedCount > 0) {
            // Some points contained - lasso intersects the stroke
            shouldSelect = true;
        } else {
            // No points contained - check if lasso path intersects stroke path
            // This handles the case where lasso crosses through a stroke
            // without containing any sample points
            for (const auto& lassoPt : lassoPoints_) {
                if (isPointNearStroke(lassoPt.x, lassoPt.y, stroke, 15.0f)) {
                    shouldSelect = true;
                    break;
                }
            }
        }

        if (shouldSelect) {
            selectedIndices.insert(i);
        }
    }

    cancelLasso();
}

void DrawingSelection::renderLasso(SkCanvas* canvas) {
    if (!lassoActive_ || lassoPoints_.size() < 2) return;

    // Increment phase for marching ants animation
    lassoPhase_ += 0.5f;
    if (lassoPhase_ > 20.0f) lassoPhase_ = 0.0f;  // Reset after full cycle (12+8)

    // Draw dotted outline with marching ants effect
    SkPaint outlinePaint;
    outlinePaint.setStyle(SkPaint::kStroke_Style);
    outlinePaint.setColor(SkColorSetARGB(180, 100, 100, 100));  // Grey
    outlinePaint.setStrokeWidth(2.0f);
    outlinePaint.setAntiAlias(true);
    outlinePaint.setStrokeCap(SkPaint::kRound_Cap);
    outlinePaint.setStrokeJoin(SkPaint::kRound_Join);

    // Dash pattern: 12px dash, 8px gap, with animated phase offset
    const SkScalar intervals[] = {12.0f, 8.0f};
    outlinePaint.setPathEffect(SkDashPathEffect::Make({intervals, 2}, lassoPhase_));

    canvas->drawPath(lassoPath_, outlinePaint);
}

void DrawingSelection::cancelLasso() {
    lassoActive_ = false;
    lassoPath_.reset();
    lassoPoints_.clear();
}

} // namespace nativedrawing
