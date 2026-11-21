// sprites.h — the pieces behind ui_lvgl's pulse sprites (docs/decisions.md D-053): render an
// area of the screen into memory once, then animate by composing small opaque RGB565 tiles in
// tight integer loops, so LVGL only ever copies them.
//
// On the panel LVGL draws arcs, anti-aliased lines and translucent shapes at about 1 us per
// pixel, and copies an opaque same-format image (plus the flush) at under 0.1 us per pixel. A
// pulse is therefore pre-rendered: the scene under it is captured once (render_area()), the
// pulse's own shape is reduced to a coverage value per pixel (distance tables, coverage masks),
// and each frame is a blend per pixel from look-up tables into the tile LVGL shows. Precomputed
// tables use float; per-frame work is integer only (ruling R11).
#pragma once

#include <cstdint>
#include <vector>

#include "hearthmap/ui/sprite_composer.h"
#include "lvgl.h"

namespace hearthmap::ui::detail {

using ui::blend565;
using ui::ComposeJob;
using ui::cpu_sprite_composer;
using ui::Overlay;
using ui::rgb565;
using ui::RingLayer;
using ui::SpriteComposer;

/// Renders what `subtree` (an object and its children; nothing above or beside it) shows in `area`
/// (screen coordinates) into `out`, `w x h` RGB565 pixels, row-major, where w and h are the
/// area's size. Uses LVGL's own renderer, the same path a refresh takes, without touching the
/// display: nothing is invalidated or flushed. Pixels nothing in the subtree covers are
/// `background` (RGB565). Returns false if LVGL could not finish the drawing.
bool render_area(lv_obj_t* subtree, const lv_area_t& area, std::uint16_t* out,
                 std::uint16_t background = 0);

/// LV_EVENT_COVER_CHECK handler for an opaque RGB565 image drawn unscaled at full opacity: it
/// covers any area inside it. LVGL 9.6's image widget never says so without an opaque
/// background of its own, so without this LVGL draws everything under such an image first.
void opaque_image_cover_cb(lv_event_t* e);

/// Distance from each pixel centre of a `w x h` block at (x0, y0) to the point (cx, cy), in
/// 1/16 px, saturated at 65535.
void distance_table(std::int32_t x0, std::int32_t y0, std::int32_t w, std::int32_t h, float cx,
                    float cy, std::vector<std::uint16_t>& out);

/// Alpha (0..255) by distance in 1/16 px for an anti-aliased ring of `radius` and `width` at
/// opacity `opa`; `size` entries.
void ring_lut(float radius, float width, std::uint8_t opa, std::size_t size,
              std::vector<std::uint8_t>& out);

/// Coverage (0..255) of a `width` px line from (ax, ay) to (bx, by) with round caps, anti-aliased,
/// over each pixel of a `w x h` block at (x0, y0). Combined with what `cov` already holds by
/// taking the maximum, so a polyline's segments can be accumulated into one mask.
void add_segment_coverage(std::int32_t x0, std::int32_t y0, std::int32_t w, std::int32_t h,
                          float ax, float ay, float bx, float by, float width,
                          std::vector<std::uint8_t>& cov);

}  // namespace hearthmap::ui::detail
