#include "map_bake.h"

#include <algorithm>

#include "sprites.h"

namespace hearthmap::ui::detail {

MapBake::MapBake(lv_obj_t* map, const lv_area_t& area, std::vector<lv_obj_t*> draw,
                 std::vector<lv_obj_t*> hide)
    : map_(map),
      area_(area),
      w_(lv_area_get_width(&area)),
      h_(lv_area_get_height(&area)),
      draw_(std::move(draw)),
      hide_(std::move(hide)),
      render_([](lv_obj_t* o, const lv_area_t& a, std::uint16_t* out, std::uint16_t bg) {
        return render_area(o, a, out, bg);
      }) {}

std::size_t MapBake::bytes_needed() const {
  const std::size_t px = static_cast<std::size_t>(std::max<std::int32_t>(0, w_)) *
                         static_cast<std::size_t>(std::max<std::int32_t>(0, h_));
  return px * 3U + static_cast<std::size_t>(std::max<std::int32_t>(0, w_)) * kBandRows * 2U;
}

void MapBake::start() {
  if (started_ || w_ <= 0 || h_ <= 0) return;
  const std::size_t px = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
  premul_.assign(px, 0);
  through_.assign(px, 255);
  scratch_.assign(static_cast<std::size_t>(w_) * kBandRows, 0);
  started_ = true;
}

bool MapBake::prepare_step() {
  if (!started_ || ready_ || failed_ || !alive_) return false;
  const std::int32_t y0 = next_row_;
  const std::int32_t y1 = std::min(h_, y0 + kBandRows);
  const lv_area_t band{area_.x1, area_.y1 + y0, area_.x2, area_.y1 + y1 - 1};

  lv_display_t* disp = lv_obj_get_display(map_);
  lv_display_enable_invalidation(disp, false);  // a counter in LVGL 9.6: paired below
  std::vector<lv_obj_t*> hidden;
  for (lv_obj_t* o : hide_) {
    if (!lv_obj_is_hidden(o)) hidden.push_back(o);
  }
  std::vector<lv_obj_t*> shown;  // baked objects already hidden are drawn for the bake
  for (lv_obj_t* o : draw_) {
    if (lv_obj_is_hidden(o)) shown.push_back(o);
  }
  for (lv_obj_t* o : hidden) lv_obj_set_hidden(o, true);
  for (lv_obj_t* o : shown) lv_obj_set_hidden(o, false);

  // Over black: the map has no background, and render_area() leaves uncovered pixels at zero.
  std::uint16_t* black =
      premul_.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(w_);
  bool ok = render_(map_, band, black, 0);
  // Over white: the map gets a white background for the moment.
  const lv_color_t bg = lv_obj_get_style_bg_color(map_, LV_PART_MAIN);
  const lv_opa_t bg_opa = lv_obj_get_style_bg_opa(map_, LV_PART_MAIN);
  lv_obj_set_style_bg_color(map_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(map_, LV_OPA_COVER, 0);
  ok = ok && render_(map_, band, scratch_.data(), 0);
  lv_obj_set_style_bg_color(map_, bg, 0);
  lv_obj_set_style_bg_opa(map_, bg_opa, 0);

  for (lv_obj_t* o : shown) lv_obj_set_hidden(o, true);
  for (lv_obj_t* o : hidden) lv_obj_set_hidden(o, false);
  lv_display_enable_invalidation(disp, true);

  if (!ok) {
    // Never publish a band that did not render: try it again, and after kMaxBandFailures in a
    // row give the bake up (the objects stay; nothing half-baked reaches a heat image).
    if (++band_failures_ >= kMaxBandFailures) {
      failed_ = true;
      std::vector<std::uint16_t>().swap(premul_);
      std::vector<std::uint8_t>().swap(through_);
      std::vector<std::uint16_t>().swap(scratch_);
      return false;
    }
    return true;
  }
  band_failures_ = 0;
  // What shows through, from the green channel (6 bits): over white minus over black is
  // (1 - coverage) x white.
  const std::size_t n = static_cast<std::size_t>(y1 - y0) * static_cast<std::size_t>(w_);
  std::uint8_t* through =
      through_.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(w_);
  for (std::size_t i = 0; i < n; ++i) {
    const std::int32_t gb = (black[i] >> 5) & 0x3F;
    const std::int32_t gw = (scratch_[i] >> 5) & 0x3F;
    const std::int32_t t = std::max<std::int32_t>(0, gw - gb);
    through[i] = static_cast<std::uint8_t>((t * 255 + 31) / 63);
  }
  next_row_ = y1;
  if (next_row_ >= h_) {
    ready_ = true;
    scratch_ = {};
  }
  return !ready_;
}

bool MapBake::apply(std::uint16_t* image, std::int32_t stride, std::int32_t row_begin,
                    std::int32_t row_end) const {
  if (!ready_ || image == nullptr) return false;
  row_begin = std::max<std::int32_t>(0, row_begin);
  row_end = std::min(h_, row_end);
  for (std::int32_t y = row_begin; y < row_end; ++y) {
    std::uint16_t* row = image + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
    const std::size_t base = static_cast<std::size_t>(y) * static_cast<std::size_t>(w_);
    for (std::int32_t x = 0; x < w_; ++x) {
      const std::size_t i = base + static_cast<std::size_t>(x);
      const std::uint32_t t = through_[i];
      if (t == 255) continue;  // nothing drawn here: most of the map
      const std::uint32_t p = premul_[i];
      const std::uint32_t h = row[x];
      const std::uint32_t r =
          std::min<std::uint32_t>(31U, ((p >> 11) & 31U) + (((h >> 11) & 31U) * t + 127U) / 255U);
      const std::uint32_t g =
          std::min<std::uint32_t>(63U, ((p >> 5) & 63U) + (((h >> 5) & 63U) * t + 127U) / 255U);
      const std::uint32_t b =
          std::min<std::uint32_t>(31U, (p & 31U) + ((h & 31U) * t + 127U) / 255U);
      row[x] = static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
    }
  }
  return true;
}

void MapBake::set_objects_hidden(bool hidden) {
  if (!alive_) return;
  for (lv_obj_t* o : draw_) {
    if (lv_obj_is_hidden(o) != hidden) lv_obj_set_hidden(o, hidden);
  }
}

void MapBake::forget_objects() {
  alive_ = false;
  draw_.clear();
  hide_.clear();
}

}  // namespace hearthmap::ui::detail
