import React from "react";
import {
  Platform,
  StyleProp,
  UIManager,
  View,
  ViewStyle,
  requireNativeComponent,
} from "react-native";

export type NativeInkPageBackgroundProps = {
  style?: StyleProp<ViewStyle>;
  backgroundType?: "plain" | "lined" | "grid" | "dotted" | "graph" | "pdf" | string;
  pdfBackgroundUri?: string;
};

const ComponentName = "MobileInkBackgroundView";

const MobileInkBackgroundViewNative =
  Platform.OS === "ios" &&
  UIManager.getViewManagerConfig(ComponentName) != null
    ? requireNativeComponent<NativeInkPageBackgroundProps>(ComponentName)
    : null;

export function NativeInkPageBackground({
  style,
  backgroundType = "plain",
  pdfBackgroundUri,
}: NativeInkPageBackgroundProps) {
  if (MobileInkBackgroundViewNative) {
    return (
      <MobileInkBackgroundViewNative
        style={style}
        backgroundType={backgroundType}
        pdfBackgroundUri={pdfBackgroundUri}
      />
    );
  }

  return <View style={[style, { backgroundColor: "#FFFFFF" }]} />;
}

export default NativeInkPageBackground;
