# Changelog

All notable changes to `@mathnotes/mobile-ink` will be documented here.

## [0.1.0] - 2026-05-07

Initial public release.

- Added `NativeInkCanvas`, the low-level native Skia/Metal drawing surface for React Native.
- Added `ZoomableInkViewport` for pinch zoom, focal-point zoom, momentum scroll, and Pencil/finger gesture routing.
- Added `ContinuousEnginePool` for fixed-pool native canvas reuse in continuous notebooks.
- Added `InfiniteInkCanvas`, a reusable continuous notebook shell with page growth, serialization, and local save/reload support.
- Added native iOS bridge helpers for page export, notebook parsing, and continuous-window compose/decompose.
- Added an Expo dev-client example app for trying the engine outside MathNotes.
