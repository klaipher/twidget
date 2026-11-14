# twidget (Nanogram fork) changelog

Branch `nanogram`. Base: Telegram's 4769d72. Newest first.

## Unreleased

- litehtml: UAX #9 bidi through the host (`document_container::bidi_levels`, rule L2) when the host answers; the simplified first-strong-character reordering remains the fallback.
- litehtml: the calc() registry holds 16,384 distinct expressions per process (was 4,096).
- litehtml and the Apple layer: the Nanogram iOS app's edits (VENDOR.md edits 1 to 91 there) are carried, so one litehtml serves the iOS, macOS and Android apps: an external-DOM builder and the master CSS in C++, patches in place (text, attributes, structure, pseudo-classes, counters, environment, sheets, document element), a style-byte budget, caps on nesting, lists, text pieces and pseudo-element content, a `<use>` instance budget, `:lang()` and `:empty` per Blink, getComputedStyle lookups, host profile hooks, iOS image policy and no-network counters. Features this branch already had (the macOS and Android versions of fill-box, calc, CSS-wide keywords, object-fit, text-overflow, direction and bidi, colour-stop fix-up, dash metrics, grid-template and others) stay as they were. See NANOGRAM-CHANGES.md for what is and is not carried.
- litehtml: a `<canvas>` takes its intrinsic size from its width and height attributes (default 300x150) and they no longer become CSS sizes, so a responsive canvas keeps its aspect ratio in the Swift renderer; the core renderer's `tw-canvas:` path still works.
- gumbo: parsing is linear in the input and bounded in memory (13 limits and fixes).
- Swift renderer: widget-sandbox v0.6.0 paint limits, content safe areas, host-enabled text selection, percentage image
  heights, exact `fill-box` extrema; new layout, canvas, safe-area and paint-policy tests; opt-in stress-kit runner.
