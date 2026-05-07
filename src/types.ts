import type { NativeSyntheticEvent } from "react-native";

export type InkToolType =
  | "pen"
  | "highlighter"
  | "eraser"
  | "select"
  | "crayon"
  | "calligraphy"
  | "text"
  | "insert"
  | "snip"
  | "mark";

export type ToolType = InkToolType;

export type NativeSelectionBounds = {
  x: number;
  y: number;
  width: number;
  height: number;
};

export type InkTextBox = {
  id: string;
  x: number;
  y: number;
  width: number;
  height?: number;
  content: string;
  color: string;
  fontSize?: number;
  isEditing?: boolean;
};

export type InsertedElement = {
  id: string;
  type: string;
  x: number;
  y: number;
  width?: number;
  height?: number;
  rotation?: number;
  locked?: boolean;
  zIndex?: number;
  createdAt?: string;
  updatedAt?: string;
  sourceUri?: string;
  aspectRatio?: number;
  cropRect?: {
    x: number;
    y: number;
    width: number;
    height: number;
  };
  caption?: string;
  tikzCode?: string;
  renderedImageUri?: string;
  renderStatus?: string;
  renderError?: string;
  templateId?: string;
  shapeType?: string;
  strokeColor?: string;
  fillColor?: string;
  strokeWidth?: number;
  cornerRadius?: number;
  arrowHeadSize?: number;
  endPoint?: { x: number; y: number };
  xRange?: [number, number];
  yRange?: [number, number];
  showGridLines?: boolean;
  showAxisLabels?: boolean;
  gridSpacing?: number;
  axisColor?: string;
  gridColor?: string;
  axisWidth?: number;
  expressions?: string[];
  boundingBox?: [number, number, number, number];
  showGrid?: boolean;
  showAxis?: boolean;
  interactive?: boolean;
};

export type NotebookPage = {
  id: string;
  title: string;
  data?: string;
  dataSignature?: string;
  rotation: number;
  textBoxes?: InkTextBox[];
  insertedElements?: InsertedElement[];
  pdfPageNumber?: number;
  pageType?: string;
  graphState?: unknown;
};

export type TextBox = InkTextBox;

export interface SerializedNotebookData {
  version: "1.0";
  pages: NotebookPage[];
  originalCanvasWidth?: number;
}

export type NativeInkSelectionChangeEvent = {
  nativeEvent: {
    count: number;
    bounds?: NativeSelectionBounds | null;
  };
};

export type NativeInkDrawingBeginEvent = NativeSyntheticEvent<{
  x: number;
  y: number;
}>;

export type NativeInkPencilDoubleTapEvent = NativeSyntheticEvent<{
  sequence: number;
  timestamp: number;
}>;
