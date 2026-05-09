# Contributing

Thanks for helping improve Mobile Ink. This repo contains the reusable drawing engine and canvas primitives.

We are genuinely grateful for anyone who spends time making this project better. You do not need to arrive with a major native-rendering patch. Repro cases, documentation fixes, example app polish, API feedback, device testing, small bug fixes, and careful questions all help build a better open-source ink engine.

If you are new to the project, issues labeled `good first issue` are meant to be approachable. If an issue is missing context, ask there and we will fill in the details. The best contributions here are clear, focused, and honest about what they changed and how they were tested.

## Development Setup

```sh
npm ci
npm ci --prefix example
npm run typecheck
npm test
npm run build
```

To run the example:

```sh
cd example
npx expo run:ios --device
```

Expo Go is not supported because the package includes native iOS code.

## Pull Request Expectations

- Keep changes focused on the engine, bridge, or reusable React Native primitives.
- Add tests for pure logic and serialization changes.
- Run the relevant local checks when touching package or native code.
- Do not include private notes, screenshots with user data, credentials, or generated build artifacts.

## Native Changes

Native iOS changes require a rebuild and should include manual verification notes. If a change affects memory lifecycle, mention how allocation behavior was checked.
