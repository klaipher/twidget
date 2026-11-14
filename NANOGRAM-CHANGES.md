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
- The iOS app's litehtml and Apple-layer edits. `nanogram` is NOT a usable pin for the iOS app until they are ported:
  swapping the iOS app's `third-party/twidget` for this branch would drop every one of them. The iOS twidget swap
  (plan W3.4) is blocked on this.
- Count: the iOS app's `third-party/twidget/VENDOR.md` has 91 numbered entries. Entry 25 is "Reserved" (a parked z-index
  patch, never applied), so there are 90 applied edits (1 to 24 and 26 to 91). Earlier notes that said 90 and a grep that
  said 91 are both right.
- State: `wip/ios-delta-clean` (d462eb9, local, not pushed, never to be pinned) merges the iOS edits to 32 files that apply
  cleanly with `git merge-file` against the pristine b5878f9 copy and adds the 7 iOS-only files (`LHHostSupport.h`,
  `calc_node.{h,cpp}`, `telegram_external.{h,cpp}`, `telegram_master_css.{h,cpp}`). 37 more files conflict (128 hunks).
- Why not on `nanogram`: the iOS history is squashed, so the edits cannot be split into one commit per VENDOR.md edit
  without redoing them by hand, and the clean files depend on declarations in the conflicting ones (the new virtuals of
  edits 12 to 29, for example). Landing the clean half alone would give a branch that cannot compile. Nothing here was built.
- To do, in order: port the 37 conflicting files by hand, one commit per VENDOR.md edit with the entry number as
  provenance; where the iOS and macOS or Android versions set different caps, the stricter cap wins (list each such
  case here for the owner); then build and run the iOS TwidgetRenderingTests and the macOS and Android suites.
