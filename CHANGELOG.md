# twidget (Nanogram fork) changelog

Branch `nanogram`. Base: Telegram's 4769d72. Newest first.

## Unreleased

- litehtml: a `<canvas>` takes its intrinsic size from its width and height attributes (default 300x150) and they no longer become CSS sizes, so a responsive canvas keeps its aspect ratio in the Swift renderer; the core renderer's `tw-canvas:` path still works.
- gumbo: parsing is linear in the input and bounded in memory (13 limits and fixes).
- Swift renderer: widget-sandbox v0.6.0 paint limits, content safe areas, host-enabled text selection, percentage image
  heights, exact `fill-box` extrema; new layout, canvas, safe-area and paint-policy tests; opt-in stress-kit runner.
