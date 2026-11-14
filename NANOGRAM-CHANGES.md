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

### The iOS app's litehtml and Apple-layer edits (VENDOR.md edits 1 to 91)
- What: the Nanogram iOS app kept a litehtml fork whose 91 numbered edits (third-party/twidget/VENDOR.md there, base
  b5878f9) are carried here, on top of the Android line and the macOS branch. Where this branch already had the same
  feature (the Android line and the macOS branch implement many of them independently), this branch's version stays and
  the iOS copy is not carried. Provenance: each commit message names its edits; `git log` plus VENDOR.md there is the
  edit-by-edit record. The iOS history was squashed, so the work is grouped by area, not one commit per edit.
- Method: `git merge-file` of the iOS tree against the pristine b5878f9 copy and this branch, with the iOS app's pure
  deletions left out (it removed the macOS template-patching path, `restoreLocked` and similar; the shared code keeps
  them), then hand resolution of every overlap (about 110 regions) and a sweep of the merged text for the superseded edits.
- Carried (behaviour from the iOS app): 1-3, 5-7, 10-27, 29-30, 31.4-31.5 (and 31.3's `appearance`), 32.4, 38.1 (the
  `<use>` instance budget, on this branch's `<use>`), 39, 41-43, 51-54, 55 (shorthand table), 59-65, 68, 71,
  73-91, the exported SVG functions of 72 (`svg_path_bounds`, `svg_element_transform`) and 30.2 (`svg_computed_property`),
  the external DOM (`telegram_external.{h,cpp}`), the master CSS in C++ (`telegram_master_css.{h,cpp}`), the host
  profile hooks (`document_container`), `LHHostSupport.h`, and the iOS guards of the Apple layer.
- Not carried because this branch has the same feature: 4 (pre-existing), 8, 9, 28, 31.1-31.2, 32.1-32.3, 33, 34, 35,
  36, 37, 38.2, 40, 44, 45, 46, 48, 49, 50, 56, 57, 58, 66, 67, 69, 70, 72 (the fill-box painter). The iOS copies of
  these are dropped, not merged: where the two disagree the macOS and Android behaviour wins.
- Adapted, not copied:
  - `:lang()` and `:empty` (31.4, 31.5): the iOS rules replace this branch's (the patch-in-place matching needs
    `m_children_for_matching`). Behaviour change for macOS and Android: `:lang()` no longer falls back to the container's
    default language, `:empty` counts comments as content.
  - `document::apply_retag`: the iOS procedure keeps the name; the macOS template path's version is now
    `apply_template_retag`.
  - The colour scheme: both `media_features::dark_color_scheme` (macOS) and `color_scheme` (iOS) are read; the Apple
    container sets both. The iOS `prefers-color-scheme` table entry and case are dropped for this branch's.
  - Edit 19 (pseudo-element content caps, `el_pseudo_image`) and edit 21 (the style-byte budget for var() splices,
    `find_custom_property`, `charge_splice`) are re-expressed on this branch's content parser and its token budget
    (`max_var_substitution_tokens`), which stays as well: the stricter limit of the two applies.
  - Apple layer: the iOS image policy and `maxPixelBytes` are merged into this branch's `LoadImageFromData` (PNG row
    recovery, SVG aspect); `CreateFeatureFont` takes a `CTFontRef`, so iOS gets font features too; the box-shadow
    downsampling (65) uses `TWClipPath`.
  - SVG: the iOS `<use>` budget (`kMaxUseElements` 8,192 shared per document) is added to this branch's `draw_use`.
- NOT carried although the iOS app has it (owner decisions or follow-ups):
  - Edit 47: UAX #9 bidi through the host's `document_container::bidi_levels`. This branch has its own, simplified
    `reorder_bidi` (a word is right to left by its first strong character), which stays; the iOS virtual is declared (it
    defaults to "no levels") so an iOS host that overrides it still compiles. Using the host's levels when it reports
    them, with this branch's as the fallback, is a small follow-up.
  - Edits 48, 49, 56 (calc): this branch's `css_units_calc` registry (4,096 interned expressions per process, nesting 16)
    replaces the iOS trees (64 nodes each, shared by an atomic count). The iOS limit is per length and never fills up; the
    registry's is a process-wide ceiling, after which later calc expressions are refused. Owner decision before the iOS
    pin: keep, or raise the ceiling, or port the trees.
  - The iOS `fork_test.cpp` checks for the dropped edits are not ported (they live in the iOS repo); the conformance and
    Twidget counts must be rebaselined by the iOS integrator.
- Stricter cap wins: edit 14 (component nesting 64), 15 (list caps), 10 (text pieces), 19, 21 (style bytes) and the
  gumbo limits are the iOS values, since this branch had none of them; where this branch has its own cap (the var()
  token budget 16,384, calc 4,096, `<use>` depth 8) both apply.
- Nothing here was built or run. `nanogram` is a usable iOS pin only after the main session's build and test run below.
- Obsolete: the local branch `wip/ios-delta-clean` held a partial first attempt; it is superseded by this port and is
  never to be pushed or pinned.

### Build and test steps still owed
- iOS: the combined iOS build, the 10 twidget/gumbo cc_tests, TwidgetRenderingTests, FidelityRenderTests, WidgetRuntime tests.
- macOS: the Mac app build; apple/Tests (V030LayoutTests, CanvasSizingTests, WidgetSafeAreaTests, WidgetPaintPolicyTests).
- Android: `ng check` with the widget corpus; `core/` tests; the CMake globs now compile `telegram_external.cpp` and
  `telegram_master_css.cpp` on every platform.
