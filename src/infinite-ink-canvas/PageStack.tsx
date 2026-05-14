import React, { memo } from "react";
import { StyleSheet, Text, View } from "react-native";
import NativeInkPageBackground from "../NativeInkPageBackground";
import type { NotebookPage } from "../types";
import { styles } from "./styles";

export type PageBackgroundsProps = {
  pages: NotebookPage[];
  pageWidth: number;
  pageHeight: number;
  backgroundType: string;
  pdfBackgroundBaseUri?: string;
  showPageLabels: boolean;
};

export const PageBackgrounds = memo(function PageBackgrounds({
  pages,
  pageWidth,
  pageHeight,
  backgroundType,
  pdfBackgroundBaseUri,
  showPageLabels,
}: PageBackgroundsProps) {
  return (
    <>
      {pages.map((page, pageIndex) => {
        const pdfBackgroundUri = backgroundType === "pdf" && pdfBackgroundBaseUri
          ? `${pdfBackgroundBaseUri}#page=${page.pdfPageNumber || pageIndex + 1}`
          : undefined;

        return (
          <View
            key={`mobile-ink-page-background-${page.id}`}
            pointerEvents="none"
            style={[
              styles.page,
              {
                top: pageIndex * pageHeight,
                width: pageWidth,
                height: pageHeight,
              },
            ]}
          >
            <NativeInkPageBackground
              style={StyleSheet.absoluteFillObject}
              backgroundType={backgroundType}
              pdfBackgroundUri={pdfBackgroundUri}
            />
            {showPageLabels ? (
              <Text style={styles.pageLabel}>{pageIndex + 1}</Text>
            ) : null}
          </View>
        );
      })}
    </>
  );
});

export type PageBreaksProps = {
  pages: NotebookPage[];
  pageHeight: number;
};

export const PageBreaks = memo(function PageBreaks({
  pages,
  pageHeight,
}: PageBreaksProps) {
  return (
    <>
      {pages.slice(1).map((page, pageIndex) => (
        <View
          key={`mobile-ink-page-break-${page.id}`}
          pointerEvents="none"
          style={[
            styles.pageBreak,
            { top: (pageIndex + 1) * pageHeight - StyleSheet.hairlineWidth / 2 },
          ]}
        />
      ))}
    </>
  );
});
