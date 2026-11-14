# How this twidget differs from Telegram's

Upstream: https://github.com/TelegramMessenger/twidget. Base: 4769d72 (the Android line, on the reorganised layout).
Ours: branch `nanogram`, used by Nanogram for iOS, macOS and Android. History: [CHANGELOG.md](CHANGELOG.md).

### Swift renderer (from the macOS app's branch)
- What: text selection hosts can switch on, content safe areas, paint limits of widget-sandbox v0.6.0 (at most 16
  heavy boxes, 2 backdrop filters; `prefers-color-scheme` follows the chat theme in litehtml too), percentage image
  heights against an indefinite container, exact curve extrema for `transform-box: fill-box`. Tests: V030LayoutTests,
  CanvasSizingTests, WidgetSafeAreaTests, WidgetPaintPolicyTests, and the opt-in stress-kit runner.
- Where: `apple/Sources/TwidgetRenderer/`, `apple/Sources/LiteHTML/`, `litehtml/`, `core/src/renderer.cpp`, `apple/Tests/`.
- Upstream: no. The macOS grid and aspect-ratio fixes are not carried: the Android line already has its own. The canvas-size fix is carried in
  `litehtml/src/el_image.cpp`, written to work with Android's `core/src/renderer.cpp` fix (db9afca) too: a canvas with a
  width or height attribute uses them (default 300x150) and does not map them to CSS; one with neither attribute but a
  `tw-canvas:` source asks the container for the drawn size.

### gumbo limits (from the iOS app's fork)
- What: the bundled gumbo parses in linear time and bounded memory: limits for tree depth, attributes, nodes,
  formatting elements, total attributes and bytes; tree freed without recursion; two tokenizer and tag-name fixes.
- Where: `litehtml/src/gumbo/`, `litehtml/gumbo-include/`.
- Upstream: no.

### Not yet carried
- The iOS app's litehtml and Apple-layer edits (90 numbered edits). They are being ported.
