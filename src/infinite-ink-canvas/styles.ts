import { StyleSheet } from "react-native";

export const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: "#F4F6F8",
  },
  viewport: {
    flex: 1,
    overflow: "hidden",
  },
  canvasShell: {
    flex: 1,
    alignItems: "center",
    justifyContent: "flex-start",
    zIndex: 1,
  },
  page: {
    position: "absolute",
    left: 0,
    overflow: "hidden",
    backgroundColor: "#FFFFFF",
  },
  pageLabel: {
    position: "absolute",
    top: 12,
    right: 14,
    color: "rgba(71, 85, 105, 0.32)",
    fontSize: 13,
    fontWeight: "700",
  },
  pageBreak: {
    position: "absolute",
    left: 0,
    right: 0,
    height: StyleSheet.hairlineWidth,
    backgroundColor: "rgba(100, 116, 139, 0.18)",
    zIndex: 3,
  },
});
