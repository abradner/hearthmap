// LVGL's built-in Montserrat Medium (SIL OFL 1.1). Leave this file out of a build that supplies
// its own fonts (fonts.h).
#include "hearthmap/ui/fonts.h"

#if !LV_FONT_MONTSERRAT_20 || !LV_FONT_MONTSERRAT_22 || !LV_FONT_MONTSERRAT_24 || \
    !LV_FONT_MONTSERRAT_26 || !LV_FONT_MONTSERRAT_28 || !LV_FONT_MONTSERRAT_30 || \
    !LV_FONT_MONTSERRAT_34
#error "builtin_fonts() needs LV_FONT_MONTSERRAT_20/22/24/26/28/30/34 enabled in lv_conf.h"
#endif

namespace hearthmap::ui {

Fonts builtin_fonts() {
  Fonts f;
  f.px20 = &lv_font_montserrat_20;
  f.px22 = &lv_font_montserrat_22;
  f.px24 = &lv_font_montserrat_24;
  f.px26 = &lv_font_montserrat_26;
  f.px28 = &lv_font_montserrat_28;
  f.px30 = &lv_font_montserrat_30;
  f.px34 = &lv_font_montserrat_34;
  return f;
}

}  // namespace hearthmap::ui
