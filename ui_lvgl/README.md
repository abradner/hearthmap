# ui_lvgl/

Turns core's `ViewModel` (`core/include/hearthmap/view_model.h`) into an LVGL 9.6 object tree
for the 1280 x 800 landscape screen, and keeps it up to date by diffing. Depends on core and LVGL
only: no ESPHome, no Home Assistant, no SDL, no clock, no file I/O (`AGENTS.md` §4). It draws; it
decides nothing a test would want to check (principle 2).

Its visual contract is the approved UI spec (mockup A); the sample render in
[`docs/img/ui-thirsty-many-soil.png`](../docs/img/ui-thirsty-many-soil.png) shows the result.

## Interface

| Header | What |
|---|---|
| `hearthmap/ui/screen.h` | `Screen`: builds the tree once per config, `update(vm, header, now[, status])` diffs; `SourceStatus` carries what the view model does not (offline, re-stamped times, a heat image behind the layer, D-046) |
| `hearthmap/ui/ports.h` | `EventSink` (taps become core `UiEvent`s) and `ImageProvider` (plant photos) |
| `hearthmap/ui/heat_image.h` | `HeatRasterizer` (cells to pixels, room-clipped) and `colour_cells` (core's `ColourLut`) |
| `hearthmap/ui/fonts.h` | `Fonts`: the only place font choice lives |
| `hearthmap/ui/format.h` | Value text, moisture percent, ring length, verdicts, status colours, the house summary's words |
| `hearthmap/ui/theme.h` | The spec's colours and fixed geometry |

The embedder owns the heat image buffer (`HeatRasterizer::pixel_count()` RGB565 pixels) and its
`lv_image_dsc_t`, the UI state (core's `UiState`), and the clock. A frame is:

```
build_view_model(config, readings, now, ui_state, vm)            // core, reuse form
heat_samples(...); compute_heat_field(...); colour_cells(...);   // only when the layer or data changed
rasterizer.render(field, cell_colours, heat_pixels); screen.invalidate_heat();
screen.update(vm, header_text, now);            // header_text.summary = summary_text(vm.summary)
```

`host/src/app.cpp` is the reference composition on the host. The device
(`components/hearthmap/`) splits the heat work from the view update so taps never wait for it,
and passes a `SourceStatus` (D-046, D-047).

## Behaviour worth knowing

- **Diffing.** Each widget's shown state (text, colours, flags, ring length) is computed and
  compared with the previous update; LVGL is touched only where it changed. `Screen::writes()`
  counts the touches and the tests pin it. A value change below display precision writes nothing.
- **Touch.** Pins are not click targets: a tap in the map area is `TapMap` with the tap point,
  and core's `hit_test()` decides what was hit. Tabs send `SelectLayer`, HUD cards
  `TapHudItem`. While the overlay is open its scrim takes every tap outside the card (it draws nothing and is
  only the card's size, but its click area reaches past every screen edge, D-052): a tap on it
  sends `CloseOverlay`, a tap on the card sends nothing (`docs/decisions.md` D-013, amended).
- **Heat image.** Colour is resolved per heat cell through core's `ColourLut`, never per pixel.
  The rasterizer blends neighbouring cells of the pixel's own room in integer arithmetic (so the
  image is bit-identical across compilers), draws outdoor rooms in the outdoor fill, and draws
  rooms with no data as stripes. It allocates only in its constructor.
- **Layout rules for real configs.** HUD names, HUD room names and the overlay title stay on
  one line and end in an ellipsis (a DOTS label with content height wraps, so the height is
  pinned to one line). Room labels are limited to their room's width, dropped when fewer than
  three characters fit, and placed in the first corner of the room's bounding box (top-left,
  top-right, bottom-left, bottom-right) that is inside the room and clear of every pin, badge and
  the widest value pill a plain pin may show; else top-left. This depends on the config only, so
  updates never move a label. The HUD gap shrinks from 10 px (never below 4) so `max_items`
  cards and the "+N more" line end 12 px above the bottom edge; the spec's cap of 7 fits.
- **Pulses are stepped, not animated** (D-051). One LVGL timer (50 ms) shows each running
  pulse's step for the current time: a danger ring steps through radius 30, 40 and 50 px at
  falling opacity, then a gap, every 1.6 s; a room outline steps through opacity 1, 0.67, 0.35,
  0.67 every 2.8 s. Objects are offset over eight slots so their steps fall in different
  refreshes. A room outline is one short line object per edge, sitting on that edge, so a step
  invalidates thin strips along the walls only. The timer stops itself when nothing pulses. The
  embedder needs nothing but LVGL's timer handler; `host/tests/perf_proxy_test.cpp` bounds what
  this costs per second.
- **The heat image is 16 strip images** over slices of the embedder's buffer (D-052). Each reports
  that it covers its area when opaque (LVGL 9.6's image widget never does without an opaque
  background of its own), so a refresh inside the map starts at the image instead of filling the
  background under it first. While the image is behind its layer every strip is hidden (the map
  shows the background under walls, labels and pins); when it catches up the strips come back
  one per 30 ms, so the new image is revealed in bands.
- **LVGL 9.6 clipping.** `overflow_visible` alone no longer lets children draw outside their
  parent: they are clipped to the parent's extended draw area. Containers whose children stick
  out (pin value labels, badges) set one (`allow_overflow()` in `screen.cpp`); a render test
  catches a regression.

## Fonts

`builtin_fonts()` (`src/fonts_builtin.cpp`) uses LVGL's built-in **Montserrat Medium**, licensed
under the **SIL Open Font License 1.1**; LVGL's generated font files also embed glyphs from
**Font Awesome Free** (font licence SIL OFL 1.1). Built-in sizes are even only, so the spec's
21 px text uses 20 px. A device build that wants other fonts passes its own `Fonts` and builds
with `-DHEARTHMAP_UI_BUILTIN_FONTS=OFF`. The built-in glyph set is ASCII plus the degree sign, so
PM2.5 units read "ug/m3".

## Memory at 1280 x 800 (for the device's PSRAM budget)

| Buffer | Bytes | Notes |
|---|---|---|
| Heat image (example map area 864 x 720, RGB565) | 1,244,160 | Owned by the embedder |
| Rasterizer working set | 643,008 | Per-pixel room index (1 byte) plus small tables |
| LVGL draw buffer, full frame RGB565 | 2,048,000 | The host uses one; a device can use partial buffers (e.g. 1/10 screen = 204,800 each) |

Software rotation on the device (800 x 1280 panel shown as 1280 x 800) needs its own buffer on
top of these; that is the display driver's, not this library's.

## Tests

`ui_lvgl/tests/` (ctest `ui.unit`) draws hand-built view models on a headless display: no core
logic runs, so a failure is a drawing or diffing failure. Build with
`-DHEARTHMAP_BUILD_UI=ON`.
