#include "SkiaDrawingEngine.h"

#include "ActiveStrokeRenderer.h"
#include "BackgroundRenderer.h"
#include "BatchExporter.h"
#include "DrawingSelection.h"
#include "PathRenderer.h"
#include "ShapeRecognition.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>
#include <include/encode/SkPngEncoder.h>

namespace nativedrawing {

namespace {

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

}  // namespace

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

} // namespace nativedrawing
