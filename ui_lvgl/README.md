# ui_lvgl/

Lands in **PR 5**. Turns a `hearthmap::ViewModel` (`core/include/hearthmap/view_model.h`) into an
LVGL 9.6 object tree and diffs consecutive view models into updates: the heat field drawn into an
`lv_image` buffer (PSRAM on the device), pins, pulsing rooms as LVGL animations, the HUD column,
layer tabs and the detail overlay. Touch: an LVGL event becomes a core `UiEvent`, core's `apply()`
returns the next `UiState`.

Depends on `core` and LVGL only: no ESPHome, no Home Assistant, no clock, no file I/O
(`AGENTS.md` §4). It decides nothing a test would want to check; that lives in core.
