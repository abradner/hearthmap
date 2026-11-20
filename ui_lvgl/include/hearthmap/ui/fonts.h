// fonts.h — the one place font choice lives.
//
// Every widget takes its font from a Fonts value, so the device build can hand in fonts ESPHome
// generated (any family, any glyph set) and never compile fonts_builtin.cpp. Sizes follow the UI
// spec; where the spec asks for 21 px the built-in set (even sizes only) uses 20 px, which keeps
// the spec's floor of 20 px.
#pragma once

#include "lvgl.h"

namespace hearthmap::ui {

struct Fonts {
  const lv_font_t* px20 = nullptr;  ///< Room names, secondary lines, "+N more", hints (spec 21).
  const lv_font_t* px22 = nullptr;  ///< Pin values, summary, HUD names, tab labels (spec 21-22).
  const lv_font_t* px24 = nullptr;  ///< HUD title.
  const lv_font_t* px26 = nullptr;  ///< Overlay thirst verdict.
  const lv_font_t* px28 = nullptr;  ///< HUD moisture percent.
  const lv_font_t* px30 = nullptr;  ///< Clock, overlay stat values.
  const lv_font_t* px34 = nullptr;  ///< Overlay title.
};

/// LVGL's built-in Montserrat Medium at the sizes above (SIL Open Font License 1.1; see
/// ui_lvgl/README.md). Needs LV_FONT_MONTSERRAT_20, 22, 24, 26, 28, 30 and 34 enabled in
/// lv_conf.h. Defined in src/fonts_builtin.cpp, which a device build may leave out.
Fonts builtin_fonts();

}  // namespace hearthmap::ui
