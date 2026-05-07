# Publishing Checklist

Use this checklist before making the repo public or publishing to npm.

## Local Gates

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
```

Then install the generated tarball into a clean app and verify it bundles.

## MathNotes Dogfood

1. Update MathNotes to consume the packed tarball or the published prerelease.
2. Run `pod install` and rebuild iOS.
3. Manually verify drawing, erasing, shape recognition, selection, text boxes, figures, continuous scrolling, zoom, autosave, reopen, and PDF backgrounds.
4. Confirm memory plateaus after repeated page-cross scrolling.
5. Delete duplicated local engine code from MathNotes only after the package artifact is the active implementation.

## npm Release

1. Set the intended semver version in `package.json` and `MathNotesMobileInk.podspec`.
2. Create and push a matching git tag.
3. Run `npm publish --access public`.
4. Install the published version in the example and MathNotes.
5. Verify the npm README media uses public absolute URLs so images and clips render outside GitHub.

## Public Repo Launch

Before flipping visibility:

- Confirm license and copyright.
- Add final demo video/GIF and screenshots.
- Confirm `SECURITY.md`, `CONTRIBUTING.md`, and issue templates are present.
- Ensure no secrets, customer data, or local build artifacts are in the repo.
