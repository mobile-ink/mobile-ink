# @mathnotes/mobile-ink

Production-grade React Native ink primitives extracted from the MathNotes canvas.

<p>
  <a href="https://www.npmjs.com/package/@mathnotes/mobile-ink"><img src="https://img.shields.io/npm/v/@mathnotes/mobile-ink.svg" alt="npm version" /></a>
  <a href="https://www.npmjs.com/package/@mathnotes/mobile-ink"><img src="https://img.shields.io/npm/dm/@mathnotes/mobile-ink.svg" alt="npm downloads" /></a>
  <a href="https://github.com/mathnotes-app/mobile-ink/actions/workflows/ci.yml"><img src="https://github.com/mathnotes-app/mobile-ink/actions/workflows/ci.yml/badge.svg" alt="CI status" /></a>
  <a href="https://github.com/mobile-ink/mobile-ink/releases/tag/v0.2.0"><img src="https://img.shields.io/github/v/release/mobile-ink/mobile-ink" alt="GitHub release" /></a>
  <a href="https://github.com/mathnotes-app/mobile-ink/blob/main/LICENSE"><img src="https://img.shields.io/npm/l/@mathnotes/mobile-ink.svg" alt="license" /></a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/mobile-ink-cover.jpg" alt="Mobile Ink cover drawn inside the example canvas" width="680" />
</p>

`@mathnotes/mobile-ink` is an iOS-first native drawing engine for React Native apps. It gives you Apple Pencil input, Skia/Metal rendering, stroke serialization, selection, zoom, momentum scrolling, and a continuous notebook surface backed by a fixed native engine pool.

## A Note To Contributors

Mobile Ink is young, but the goal is serious: make high-quality mobile ink available to every React Native team that needs it. If you try the package, report a bug, improve docs, test it on a real device, suggest an API shape, or think through a native rendering problem with us, thank you. Notes-app canvases are full of sharp edges, and thoughtful feedback from people building real apps is one of the most valuable contributions this project can get.

Beginner questions are welcome here. So are tiny fixes. A clear reproduction, a missing setup note, a confusing API name, or a small example improvement all help move the engine toward something the community can trust.

## Why This Exists

There is still no strong open-source answer for the kind of drawing surface a serious mobile notes app needs. A single canvas is not enough. A notes app has to handle long notebooks, fast page crossing, Pencil latency, zoom, selection, shape editing, serialization, previewing, and native memory pressure without making the JS thread fall over.

MathNotes has been building this engine since May 2025. We are open-sourcing the reusable canvas layer because mobile apps should have a high-performance, community-owned drawing engine instead of every team rebuilding the same hard native stack from scratch.

The goal is simple: make this the best community drawing engine for React Native and native mobile note-taking surfaces.

mobile-ink is currently used in production in MathNotes: https://apps.apple.com/us/app/mathnotes-ai-notes-for-stem/id6751956086

## Capability Snapshot

| Area | Current support |
| --- | --- |
| iOS Apple Pencil drawing | Used in production |
| Native rendering | Custom `MTKView` backed by C++ Skia/Metal |
| Continuous notebooks | Fixed native engine pool with momentum scroll and pinch zoom |
| Tools | Pen, highlighter, crayon, calligraphy, eraser, selection, and shape recognition |
| Serialization | JSON notebook payloads plus native page load/save/export helpers |
| Example app | Expo dev-client app with blank continuous notebook, tools, selection, save/reload, and zoom |
| Android | Not supported yet |
| Expo Go | Not supported because this package includes native code |

## Demos

| Drawing and tools | Selection | Large notebook interaction |
| --- | --- | --- |
| ![Drawing with Mobile Ink](https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/mobile-ink-sketch.gif) | ![Selection editing](https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/selection-editing-demo.gif) | ![Large notebook interaction](https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/large-notebook-performance.gif) |

Full clips:
[drawing, tools, and shapes](https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/tools-and-shapes-demo.mp4),
[selection editing](https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/selection-editing-demo.mp4),
[large notebook performance](https://raw.githubusercontent.com/mathnotes-app/mobile-ink/main/docs/assets/large-notebook-performance.mp4).


## Install

```sh
npm install @mathnotes/mobile-ink \
  @shopify/react-native-skia \
  react-native-gesture-handler \
  react-native-reanimated \
  react-native-worklets
cd ios && pod install
```

For Expo apps, use a dev client or prebuild. Expo Go cannot load this native module.

Your app Babel config must include the Reanimated/Worklets plugin expected by your React Native/Reanimated version. For Expo SDK 54/Reanimated 4:

```js
module.exports = function(api) {
  api.cache(true);
  return {
    presets: ['babel-preset-expo'],
    plugins: ['react-native-worklets/plugin'],
  };
};
```

## Quickstart

```tsx
import React, { useRef, useState } from 'react';
import { SafeAreaView } from 'react-native';
import {
  InfiniteInkCanvas,
  type ContinuousEnginePoolToolState,
  type InfiniteInkCanvasRef,
} from '@mathnotes/mobile-ink';

export function Notebook() {
  const canvasRef = useRef<InfiniteInkCanvasRef | null>(null);
  const [toolState, setToolState] = useState<ContinuousEnginePoolToolState>({
    toolType: 'pen',
    width: 3,
    color: '#111111',
    eraserMode: 'pixel',
  });

  return (
    <SafeAreaView style={{ flex: 1 }}>
      <InfiniteInkCanvas
        ref={canvasRef}
        style={{ flex: 1 }}
        toolState={toolState}
        backgroundType="plain"
        fingerDrawingEnabled={false}
        onDrawingChange={() => {
          // Trigger your own save/debounce pipeline here.
        }}
      />
    </SafeAreaView>
  );
}
```

## Public Surface

- `NativeInkCanvas`: low-level native Skia/Metal drawing view.
- `ZoomableInkViewport`: production pinch, pan, momentum, focal-point zoom, and Apple Pencil/finger gesture routing.
- `ContinuousEnginePool`: fixed-size native canvas pool for continuous notebooks.
- `InfiniteInkCanvas`: full vertical continuous notebook shell with pooled native engines, trailing page creation, dirty-page serialization, zoom, momentum scroll, and generic page backgrounds.
- Native bridge helpers for page export, native notebook body parse, and continuous-window compose/decompose.
- Pure utilities for continuous coordinates, engine-pool range calculation, notebook serialization, and page growth.

## Example App

The `example/` folder is an Expo dev-client app that exercises the reusable canvas stack, not a MathNotes screen. It demos the full continuous canvas path: pencil drawing with finger navigation by default, optional draw-with-finger mode, pinch zoom, momentum scroll, engine-pool page assignment, MathNotes-style one-page trailing blank growth, tools, selection, and local save/reload on a blank page background.

Expo Go cannot run the example because this package includes native Kotlin, C++, and iOS code. Use a dev-client build:

```sh
cd example
npm install
npx expo run:ios --device
```

For a simulator:

```sh
npx expo run:ios
```

For Android:

```sh
npx expo run:android
```

The first Android build compiles the shared C++ drawing engine and can take a while. If the generated native project is stale after Android native changes, run:

```sh
npx expo prebuild --platform android --clean
npx expo run:android
```

The Android example runs the drawing canvas path. The benchmark screen and CPU/Ganesh backend toggle are currently iOS-only. See [example/README.md](example/README.md) for Android prerequisites, Metro/dev-client commands, and smoke checks.

## Documentation

- [Architecture](docs/architecture.md)
- [API Reference](docs/api.md)
- [Changelog](CHANGELOG.md)

## Roadmap

Near-term work is focused on making the public package easier to adopt and easier to contribute to, and achieving Android parity:

- Improve install and troubleshooting docs for React Native and Expo dev-client apps.
- Add more integration recipes for save/load, tool switching, and app-owned storage.
- Tighten selection transform performance for large stroke groups.
- Improve edge-case zoom behavior near page and canvas boundaries.
- Continue hardening the example app as a small regression harness.
- Complete Android parity with iOS. We started this but it is still not quite there.

## Development

```sh
npm ci
npm run typecheck
npm test
npm run test:native:smoke
npm run build
npm pack --dry-run --ignore-scripts
npm ci --prefix example
npm run test:example:typecheck
npm run test:example:export:ios
npm run test:example:export:android
```

`npm run build` creates `lib/` for npm packaging. The React Native entry still points at `src/index.ts` so Metro can transform worklet directives with the consuming app's Babel config.

## License

Apache-2.0. Copyright BuilderPro LLC.
