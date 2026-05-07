# Media

The repo should launch with real iPad media. Keep raw recordings out of git; commit only compressed clips, GIFs, and screenshots that are useful in the README or docs.

## Included Assets

- `docs/assets/mobile-ink-cover.jpg`: README cover image drawn inside the canvas.
- `docs/assets/mobile-ink-sketch.gif`: short inline drawing/tools preview.
- `docs/assets/selection-editing-demo.gif`: short inline selection preview.
- `docs/assets/large-notebook-performance.gif`: short inline large-notebook preview.
- `docs/assets/tools-and-shapes-demo.mp4`: compressed drawing, tools, and shape demo.
- `docs/assets/selection-editing-demo.mp4`: compressed selection/editing demo.
- `docs/assets/large-notebook-performance.mp4`: compressed large-notebook interaction demo.

The raw `.mov` screen recordings used to generate these assets are intentionally not committed.

## Capture Checklist

Use the example app for public media whenever possible. Real stress notebooks can be used for performance footage if they contain no private or user-identifying data.

1. Pencil drawing on a blank page, then page auto-growth when writing on the trailing blank page.
2. Pinch zoom at the top and sides of the page, showing stable focal-point zoom.
3. Fast momentum scroll across multiple pages, showing smooth page assignment.
4. Selection, shape recognition, and eraser basics.
5. Save, app reload, and local notebook restore in the example.

## Still Images

- Main screenshot: continuous canvas with Pencil strokes, shapes, and selection primitives.
- Architecture diagram exported from `docs/architecture.md`.
- Optional memory/performance graphic: fixed three-engine pool across many pages.

## Suggested Formats

- README hero GIF: 8-12 seconds, under 8 MB if possible.
- Full demo video: 45-90 seconds, linked from README.
- Screenshots: PNG, 2x resolution.

Do not show secrets, customer data, private notes, or identifying account information in public media.
