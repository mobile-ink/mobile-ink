# Mobile Ink Example

The example is an Expo dev-client app that loads `@mathnotes/mobile-ink` from the parent folder with `file:..`. It cannot run in Expo Go because the package includes Kotlin, C++, and platform native views.

## Android

Prerequisites:

- Android Studio with an installed SDK, platform tools, emulator, and NDK/CMake support.
- A running Android emulator or a USB device with developer mode enabled.
- `JAVA_HOME` and `ANDROID_HOME` configured for your Android toolchain.

From the repository root:

```sh
npm ci
npm ci --prefix example
cd example
npx expo run:android
```

The first Android run builds the native library, including the shared C++ drawing engine. That build can take a while and should include Gradle tasks such as `:mathnotes_mobile-ink:compileDebugKotlin` and `:mathnotes_mobile-ink:externalNativeBuildDebug`.

If Metro is already running, use the dev-client command in a second terminal:

```sh
cd example
npx expo start --dev-client
```

If native code or Android package metadata changed and the generated native project looks stale, rebuild it from Expo config:

```sh
cd example
npx expo prebuild --platform android --clean
npx expo run:android
```

The generated `example/android/` folder is local build output and is intentionally ignored by git.

## Android Smoke Checks

These checks do not require a connected device:

```sh
npm --prefix example run typecheck
npm --prefix example run export:android
```

If the installed app reports a missing native module at runtime, rebuild the dev client with `npx expo run:android`. Static export validates Metro bundling; it does not compile or install native code.

## iOS

```sh
npm ci
npm ci --prefix example
cd example
npx expo run:ios
```

Use `npx expo run:ios --device` for a physical device.

The benchmark screen and render-backend toggle are iOS-only in this example because the benchmark recorder and CPU/Ganesh backend selector are currently implemented on the iOS native bridge.
