/* lv_conf.h for the host build of LVGL 9.6.0 (cmake/HearthmapLvgl.cmake).
 *
 * Only settings that differ from lv_conf_internal.h's defaults are listed. The aim is to render
 * the way the panel does: 16-bit colour (the panel is RGB565), software renderer, no OS, and no
 * LVGL theme (ui_lvgl styles every object itself so the device's theme cannot change the look).
 * Not used on the device: ESPHome generates its own lv_conf.h there.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

/* The C library's allocator: LVGL's built-in pool is 64 KB by default, too small for a full
 * screen of widgets, and the host has no reason to emulate it. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE

/* Warnings are compiled in (as on the device, where ESPHome prints them) and go nowhere unless a
 * test registers a print callback: ui_lvgl's tests count them (docs/decisions.md D-039). */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

/* The software renderer's colour formats, exactly as ESPHome 2026.11.0 compiles them for the
 * panel (its lvgl component: the display format RGB565 plus ARGB8888 for layers, and RGB888 for
 * gradients), so host renders and golden images take the device's code paths. A format missing
 * here is missing there: drawing a scaled or rotated RGB565 image, for one, needs RGB565A8 too,
 * which the device does not have (D-039). */
#define LV_DRAW_SW_SUPPORT_RGB565 1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_RGB888 1
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_A8 0
#define LV_DRAW_SW_SUPPORT_I1 0
#define LV_USE_SYSMON 0

/* No theme: every ui_lvgl object starts from lv_obj_remove_style_all(). */
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0

/* Fonts at the UI spec's sizes (ui_lvgl/include/hearthmap/ui/fonts.h). 14 stays as LVGL's
 * default font for objects nobody styled (there should be none). */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 1
#define LV_FONT_MONTSERRAT_34 1

#endif /* LV_CONF_H */
