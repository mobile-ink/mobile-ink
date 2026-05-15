# Changelog

All notable changes to `@mathnotes/mobile-ink` will be documented here.

## [0.3.0] - 2026-05-15

- Added the Android native ink renderer with Skia Ganesh GPU rendering and React Native view integration.
- Brought continuous multi-page notebooks to Android, including pooled native canvas activation, page preview overlays while engines mount, and saved notebook reload in the example app.
- Added Android PDF background loading, notebook serialization support, and native smoke coverage.
- Documented Android setup and parity status for the example app and package consumers.

## [0.2.0] - 2026-05-12

- Defaulted the iOS renderer to the Ganesh/Metal path while keeping CPU selectable for A/B comparison and fallback.
- Added on-device benchmark tooling for replayed strokes, manual notebook recordings, scroll sampling, multi-page suites, eraser, selection, and tool coverage.
- Matched Ganesh output colors to the CPU path for highlighter, selection chrome, and other tool colors.

## [0.1.0] - 2026-05-07

Initial public release.

- Added `NativeInkCanvas`, the low-level native Skia/Metal drawing surface for React Native.
- Added `ZoomableInkViewport` for pinch zoom, focal-point zoom, momentum scroll, and Pencil/finger gesture routing.
- Added `ContinuousEnginePool` for fixed-pool native canvas reuse in continuous notebooks.
- Added `InfiniteInkCanvas`, a reusable continuous notebook shell with page growth, serialization, and local save/reload support.
- Added native iOS bridge helpers for page export, notebook parsing, and continuous-window compose/decompose.
- Added an Expo dev-client example app for trying the engine outside MathNotes.
