#include "DrawingHistory.h"
#include "PathRenderer.h"
#include <algorithm>
#include <utility>

namespace nativedrawing {

void commitStrokeDelta(
    std::vector<StrokeDelta>& undoStack,
    std::vector<StrokeDelta>& redoStack,
    StrokeDelta&& delta,
    size_t maxHistoryEntries
) {
    redoStack.clear();
    undoStack.push_back(std::move(delta));
    while (undoStack.size() > maxHistoryEntries) {
        undoStack.erase(undoStack.begin());
    }
}

void appendPixelEraseCircleToDelta(
    std::vector<StrokeDelta::PixelEraseEntry>& pendingPixelEraseEntries,
    size_t strokeIndex,
    const EraserCircle& circle
) {
    for (auto& entry : pendingPixelEraseEntries) {
        if (entry.strokeIndex == strokeIndex) {
            entry.addedCircles.push_back(circle);
            return;
        }
    }

    StrokeDelta::PixelEraseEntry entry;
    entry.strokeIndex = strokeIndex;
    entry.addedCircles.push_back(circle);
    pendingPixelEraseEntries.push_back(std::move(entry));
}

void applyStrokeDelta(
    const StrokeDelta& delta,
    std::vector<Stroke>& strokes,
    std::vector<EraserCircle>& eraserCircles,
    PathRenderer& pathRenderer
) {
    switch (delta.kind) {
        case StrokeDelta::Kind::AddStrokes: {
            for (const auto& stroke : delta.addedStrokes) {
                strokes.push_back(stroke);
            }
            break;
        }
        case StrokeDelta::Kind::RemoveStrokes: {
            for (auto it = delta.removedStrokes.rbegin(); it != delta.removedStrokes.rend(); ++it) {
                if (it->first < strokes.size()) {
                    strokes.erase(strokes.begin() + static_cast<long>(it->first));
                }
            }
            break;
        }
        case StrokeDelta::Kind::PixelErase: {
            for (const auto& entry : delta.pixelEraseEntries) {
                if (entry.strokeIndex >= strokes.size()) continue;
                auto& target = strokes[entry.strokeIndex].erasedBy;
                for (const auto& circle : entry.addedCircles) {
                    target.push_back(circle);
                }
                strokes[entry.strokeIndex].cachedEraserCount = 0;
            }
            break;
        }
        case StrokeDelta::Kind::MoveStrokes: {
            for (size_t index : delta.moveIndices) {
                if (index >= strokes.size()) continue;
                auto& stroke = strokes[index];
                for (auto& point : stroke.points) {
                    point.x += delta.moveDx;
                    point.y += delta.moveDy;
                }
                for (auto& circle : stroke.erasedBy) {
                    circle.x += delta.moveDx;
                    circle.y += delta.moveDy;
                }
                pathRenderer.smoothPath(stroke.points, stroke.path);
                stroke.cachedEraserCount = 0;
            }
            break;
        }
        case StrokeDelta::Kind::ReplaceStrokes: {
            for (const auto& [index, stroke] : delta.afterStrokes) {
                if (index < strokes.size()) {
                    strokes[index] = stroke;
                }
            }
            break;
        }
        case StrokeDelta::Kind::Clear: {
            strokes.clear();
            eraserCircles.clear();
            break;
        }
    }
}

void revertStrokeDelta(
    const StrokeDelta& delta,
    std::vector<Stroke>& strokes,
    std::vector<EraserCircle>& eraserCircles,
    PathRenderer& pathRenderer
) {
    switch (delta.kind) {
        case StrokeDelta::Kind::AddStrokes: {
            for (size_t i = 0; i < delta.addedStrokes.size(); ++i) {
                if (!strokes.empty()) {
                    strokes.pop_back();
                }
            }
            break;
        }
        case StrokeDelta::Kind::RemoveStrokes: {
            for (const auto& [index, stroke] : delta.removedStrokes) {
                size_t clamped = std::min(index, strokes.size());
                strokes.insert(strokes.begin() + static_cast<long>(clamped), stroke);
            }
            break;
        }
        case StrokeDelta::Kind::PixelErase: {
            for (const auto& entry : delta.pixelEraseEntries) {
                if (entry.strokeIndex >= strokes.size()) continue;
                auto& target = strokes[entry.strokeIndex].erasedBy;
                for (size_t i = 0; i < entry.addedCircles.size() && !target.empty(); ++i) {
                    target.pop_back();
                }
                strokes[entry.strokeIndex].cachedEraserCount = 0;
            }
            break;
        }
        case StrokeDelta::Kind::MoveStrokes: {
            for (size_t index : delta.moveIndices) {
                if (index >= strokes.size()) continue;
                auto& stroke = strokes[index];
                for (auto& point : stroke.points) {
                    point.x -= delta.moveDx;
                    point.y -= delta.moveDy;
                }
                for (auto& circle : stroke.erasedBy) {
                    circle.x -= delta.moveDx;
                    circle.y -= delta.moveDy;
                }
                pathRenderer.smoothPath(stroke.points, stroke.path);
                stroke.cachedEraserCount = 0;
            }
            break;
        }
        case StrokeDelta::Kind::ReplaceStrokes: {
            for (const auto& [index, stroke] : delta.beforeStrokes) {
                if (index < strokes.size()) {
                    strokes[index] = stroke;
                }
            }
            break;
        }
        case StrokeDelta::Kind::Clear: {
            strokes = delta.clearedStrokes;
            eraserCircles = delta.clearedEraserCircles;
            break;
        }
    }
}

} // namespace nativedrawing
