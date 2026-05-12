import React, { useEffect, useRef, useState } from "react";
import {
  SafeAreaView,
  StyleSheet,
  Switch,
  Text,
  TouchableOpacity,
  View,
} from "react-native";
import AsyncStorage from "@react-native-async-storage/async-storage";
import {
  InfiniteInkCanvas,
  type InfiniteInkCanvasRef,
  type NativeInkRenderBackend,
  type SerializedNotebookData,
} from "@mathnotes/mobile-ink";
import {
  tools,
  type ToolConfig,
} from "./benchmark";
import { BenchmarkScreen } from "./BenchmarkScreen";

type AppMode = "draw" | "benchmark";

const STORAGE_KEY = "mobile-ink-example-notebook";

export default function App() {
  const canvasRef = useRef<InfiniteInkCanvasRef | null>(null);
  const [mode, setMode] = useState<AppMode>("draw");
  const [activeTool, setActiveTool] = useState<ToolConfig>(tools[0]);
  const [drawWithFinger, setDrawWithFinger] = useState(false);
  const [renderBackend, setRenderBackend] = useState<NativeInkRenderBackend>("ganesh");
  const [savedNotebook, setSavedNotebook] = useState<SerializedNotebookData | null>(null);
  const [storageStatus, setStorageStatus] = useState("not saved");
  const [currentPageIndex, setCurrentPageIndex] = useState(0);
  const [pageCount, setPageCount] = useState(1);
  const [isMoving, setIsMoving] = useState(false);

  useEffect(() => {
    let isMounted = true;
    AsyncStorage.getItem(STORAGE_KEY)
      .then((rawNotebook) => {
        if (!isMounted || !rawNotebook) {
          return;
        }

        setSavedNotebook(JSON.parse(rawNotebook) as SerializedNotebookData);
        setStorageStatus("saved on disk");
      })
      .catch(() => {
        if (isMounted) {
          setStorageStatus("save unavailable");
        }
      });

    return () => {
      isMounted = false;
    };
  }, []);

  const applyTool = (tool: ToolConfig) => {
    setActiveTool(tool);
    canvasRef.current?.setTool(tool);
  };

  const save = async () => {
    const notebookData = await canvasRef.current?.getNotebookData();
    if (!notebookData) {
      return;
    }

    await AsyncStorage.setItem(STORAGE_KEY, JSON.stringify(notebookData));
    setSavedNotebook(notebookData);
    setStorageStatus("saved on disk");
  };

  const reload = async () => {
    const notebookData = savedNotebook;
    if (notebookData) {
      await canvasRef.current?.loadNotebookData(notebookData);
    }
  };

  return (
    <SafeAreaView style={styles.screen}>
      <View style={styles.toolbar}>
        <View style={styles.segmentGroup}>
          {(["draw", "benchmark"] as AppMode[]).map((item) => (
            <TouchableOpacity
              key={item}
              style={[styles.button, mode === item && styles.activeButton]}
              onPress={() => setMode(item)}
            >
              <Text style={styles.buttonText}>{item === "draw" ? "Draw" : "Benchmark"}</Text>
            </TouchableOpacity>
          ))}
        </View>

        {mode === "draw" ? (
          <>
            <View style={styles.segmentGroup}>
              {(["ganesh", "cpu"] as NativeInkRenderBackend[]).map((item) => (
                <TouchableOpacity
                  key={item}
                  style={[styles.button, renderBackend === item && styles.activeButton]}
                  onPress={() => setRenderBackend(item)}
                >
                  <Text style={styles.buttonText}>{item === "ganesh" ? "Ganesh" : "CPU"}</Text>
                </TouchableOpacity>
              ))}
            </View>

            <View style={styles.toggleRow}>
              <Text style={styles.toggleLabel}>Draw with finger</Text>
              <Switch value={drawWithFinger} onValueChange={setDrawWithFinger} />
            </View>

            {tools.map((tool) => (
              <TouchableOpacity
                key={tool.label}
                style={[styles.button, activeTool.label === tool.label && styles.activeButton]}
                onPress={() => applyTool(tool)}
              >
                <Text style={styles.buttonText}>{tool.label}</Text>
              </TouchableOpacity>
            ))}

            <TouchableOpacity style={styles.button} onPress={() => canvasRef.current?.undo()}>
              <Text style={styles.buttonText}>Undo</Text>
            </TouchableOpacity>
            <TouchableOpacity style={styles.button} onPress={() => canvasRef.current?.redo()}>
              <Text style={styles.buttonText}>Redo</Text>
            </TouchableOpacity>
            <TouchableOpacity style={styles.button} onPress={() => canvasRef.current?.clearCurrentPage()}>
              <Text style={styles.buttonText}>Clear</Text>
            </TouchableOpacity>
            <TouchableOpacity style={styles.button} onPress={() => canvasRef.current?.resetViewport(true)}>
              <Text style={styles.buttonText}>Reset View</Text>
            </TouchableOpacity>
            <TouchableOpacity style={styles.button} onPress={save}>
              <Text style={styles.buttonText}>Save</Text>
            </TouchableOpacity>
            <TouchableOpacity style={styles.button} onPress={reload}>
              <Text style={styles.buttonText}>Reload</Text>
            </TouchableOpacity>
          </>
        ) : null}
      </View>

      {mode === "draw" ? (
        <View style={styles.statusBar}>
          <Text style={styles.statusText}>Page {currentPageIndex + 1} / {pageCount}</Text>
          <Text style={styles.statusText}>{isMoving ? "moving" : "settled"}</Text>
          <Text style={styles.statusText}>{storageStatus}</Text>
          <Text style={styles.statusText}>Backend {renderBackend}</Text>
        </View>
      ) : null}

      <View style={styles.contentArea}>
        <InfiniteInkCanvas
          ref={canvasRef}
          style={styles.canvasHost}
          initialPageCount={1}
          pageWidth={820}
          pageHeight={1061}
          backgroundType="plain"
          renderBackend={renderBackend}
          fingerDrawingEnabled={mode === "benchmark" ? false : drawWithFinger}
          toolState={activeTool}
          minScale={1}
          maxScale={5}
          onCurrentPageChange={setCurrentPageIndex}
          onPagesChange={(pages) => setPageCount(pages.length)}
          onMotionStateChange={setIsMoving}
        />

        {mode === "benchmark" ? (
          <BenchmarkScreen
            backend={renderBackend}
            canvasRef={canvasRef}
            activeTool={activeTool}
            applyTool={applyTool}
            setBackend={setRenderBackend}
          />
        ) : null}
      </View>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: "#F4F6F8",
  },
  toolbar: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 8,
    padding: 12,
    backgroundColor: "#FFFFFF",
    borderBottomColor: "#D7DEE8",
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  segmentGroup: {
    flexDirection: "row",
    flexWrap: "wrap",
    gap: 6,
  },
  toggleRow: {
    alignItems: "center",
    flexDirection: "row",
    gap: 8,
    minHeight: 40,
    paddingHorizontal: 8,
  },
  toggleLabel: {
    color: "#17202A",
    fontWeight: "600",
  },
  button: {
    borderRadius: 8,
    paddingHorizontal: 12,
    paddingVertical: 8,
    backgroundColor: "#EDF1F5",
  },
  activeButton: {
    backgroundColor: "#CFE1FF",
  },
  buttonText: {
    color: "#17202A",
    fontWeight: "600",
  },
  statusBar: {
    alignItems: "center",
    flexDirection: "row",
    gap: 14,
    minHeight: 34,
    paddingHorizontal: 14,
    backgroundColor: "#F9FAFC",
    borderBottomColor: "#D7DEE8",
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  statusText: {
    color: "#4B5563",
    fontSize: 12,
    fontWeight: "700",
  },
  contentArea: {
    flex: 1,
  },
  canvasHost: {
    flex: 1,
  },
});
