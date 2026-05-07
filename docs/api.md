# API Reference

This is the V1 public API surface. The package provides the ink engine and generic continuous canvas primitives. Host apps bring their own surrounding UI, persistence model, and file lifecycle.

## `InfiniteInkCanvas`

High-level continuous notebook component.

```tsx
<InfiniteInkCanvas
  ref={canvasRef}
  style={{ flex: 1 }}
  toolState={toolState}
  backgroundType="plain"
  fingerDrawingEnabled={false}
/>
```

### Props

- `toolState`: current tool, width, color, and eraser mode.
- `fingerDrawingEnabled`: when `false`, Apple Pencil draws and fingers navigate. When `true`, a finger can draw and navigation uses two-finger gestures.
- `backgroundType`: generic page background, for example `plain`, `grid`, `lined`, `dotted`, `graph`, or `pdf`.
- `pdfBackgroundBaseUri`: base URI used when `backgroundType="pdf"`.
- `initialData`: serialized notebook payload to load at mount.
- `initialPageCount`: number of blank pages to create when no initial data is provided. The default is `1`.
- `pageWidth` / `pageHeight`: logical page size.
- `minScale` / `maxScale`: viewport zoom bounds.
- `showPageLabels`: whether the generic example-style page number labels render.
- `onDrawingChange(pageId)`: called when a page becomes dirty.
- `onCurrentPageChange(pageIndex)`: called when the settled/current page changes.
- `onPagesChange(pages)`: called when page growth or trimming changes the page array.
- `onSelectionChange(pageId, count, bounds)`: native selection event by page.
- `onMotionStateChange(isMoving)`: viewport gesture/momentum state.
- `onPencilDoubleTap(event)`: Apple Pencil double-tap callback.

### Ref

```ts
type InfiniteInkCanvasRef = {
  getNotebookData(): Promise<SerializedNotebookData>;
  loadNotebookData(data: SerializedNotebookData | string): Promise<void>;
  addPage(): Promise<void>;
  undo(): void;
  redo(): void;
  clearCurrentPage(): void;
  setTool(toolState: ContinuousEnginePoolToolState): void;
  resetViewport(animated?: boolean): void;
  getCurrentPageIndex(): number;
  scrollToPage(pageIndex: number, animated?: boolean): void;
};
```

`addPage()` moves to the existing trailing blank page. It does not create arbitrary blank pages, because the continuous notebook invariant is exactly one blank page after the last content page.

## `NativeInkCanvas`

Low-level native drawing surface. Use this if you are building your own page model or viewport.

Important ref methods:

- `setTool(toolType, width, color, eraserMode)`
- `getBase64Data()`
- `loadBase64Data(base64String)`
- `getBase64PngData(scale?)`
- `getBase64JpegData(scale?, compression?)`
- `undo()` / `redo()` / `clear()`
- `performCopy()` / `performPaste()` / `performDelete()`
- `releaseEngine()`

## `ZoomableInkViewport`

Reusable viewport with the MathNotes gesture behavior: focal-point pinch zoom, momentum pan, bounds clamping, horizontal snugness, and Pencil/finger routing.

Use it when your app wants the production viewport without `InfiniteInkCanvas`'s page model.

## `ContinuousEnginePool`

Fixed native-engine pool. It is lower level than `InfiniteInkCanvas` and expects the host app to provide page assignments, dirty-page persistence, and tool routing.

The pool is useful when you already own a custom continuous notebook implementation and only want the no-mount-churn native engine management.

## Data Types

The central page payload is:

```ts
type NotebookPage = {
  id: string;
  title: string;
  data?: string;
  rotation: number;
  textBoxes?: InkTextBox[];
  insertedElements?: InsertedElement[];
  pdfPageNumber?: number;
};
```

Native stroke data lives in `page.data`. Apps may store their own text boxes, inserted elements, graph state, and metadata on the page object.
