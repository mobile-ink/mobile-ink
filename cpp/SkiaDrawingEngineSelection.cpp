#include "SkiaDrawingEngine.h"

#include "BackgroundRenderer.h"
#include "DrawingSelection.h"
#include "ShapeRecognition.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>

#include <include/core/SkImageInfo.h>

namespace nativedrawing {

namespace {

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

}  // namespace

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

} // namespace nativedrawing
