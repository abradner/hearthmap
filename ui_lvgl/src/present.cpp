#include "hearthmap/ui/present.h"

#include <initializer_list>

#include "lvgl_private.h"

namespace hearthmap::ui {

namespace {

bool inside(const lv_area_t& inner, const lv_area_t& outer) {
  return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 &&
         inner.y2 <= outer.y2;
}

bool overlaps(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

bool transformed_style(const lv_obj_t* o) {
  return lv_obj_get_style_transform_scale_x(o, LV_PART_MAIN) != LV_SCALE_NONE ||
         lv_obj_get_style_transform_scale_y(o, LV_PART_MAIN) != LV_SCALE_NONE ||
         lv_obj_get_style_transform_rotation(o, LV_PART_MAIN) != 0 ||
         lv_obj_get_style_transform_skew_x(o, LV_PART_MAIN) != 0 ||
         lv_obj_get_style_transform_skew_y(o, LV_PART_MAIN) != 0;
}

// Draw order walk: an object draws, then its children in order. `after` turns true once the
// tile has been passed; any visible object after that reaching into `area` covers it.
struct CoverWalk {
  const lv_obj_t* tile;
  lv_area_t area;
  bool after = false;
  bool covered = false;

  void visit(lv_obj_t* o, bool check_self) {
    if (covered || lv_obj_is_hidden(o)) return;
    if (o == tile) {
      after = true;
      return;  // its own children are refused separately (HasChildren)
    }
    if (after && check_self) {
      lv_area_t a;
      lv_obj_get_coords(o, &a);
      const int32_t ext = lv_obj_get_ext_draw_size(o);
      a.x1 -= ext;
      a.y1 -= ext;
      a.x2 += ext;
      a.y2 += ext;
      if (overlaps(a, area)) {
        covered = true;
        return;
      }
    }
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n && !covered; ++i) {
      visit(lv_obj_get_child(o, static_cast<int32_t>(i)), true);
    }
  }
};

}  // namespace

PresentBlock present_check(lv_obj_t* image, const lv_image_dsc_t& dsc) {
  if (image == nullptr || !lv_obj_check_type(image, &lv_image_class) ||
      lv_image_get_src(image) != &dsc) {
    return PresentBlock::NotImage;
  }
  if (dsc.header.cf != LV_COLOR_FORMAT_RGB565 || dsc.header.w == 0 || dsc.header.h == 0 ||
      dsc.header.stride != dsc.header.w * 2U || dsc.data == nullptr) {
    return PresentBlock::NotRgb565;
  }
  lv_display_t* disp = lv_obj_get_display(image);
  if (disp != nullptr && disp->rendering_in_progress) return PresentBlock::Refreshing;
  lv_obj_t* screen = lv_obj_get_screen(image);
  if (disp == nullptr || screen != lv_display_get_screen_active(disp)) {
    return PresentBlock::NotOnActiveScreen;
  }
  lv_obj_update_layout(image);

  for (const lv_obj_t* o = image; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_is_hidden(o)) return PresentBlock::Hidden;
  }
  if (lv_obj_get_style_opa_recursive(image, LV_PART_MAIN) < LV_OPA_COVER ||
      lv_obj_get_style_image_opa(image, LV_PART_MAIN) < LV_OPA_COVER ||
      lv_obj_get_style_image_recolor_opa(image, LV_PART_MAIN) > LV_OPA_TRANSP ||
      lv_obj_get_style_blend_mode(image, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL) {
    return PresentBlock::NotOpaque;
  }
  if (lv_image_get_scale_x(image) != LV_SCALE_NONE ||
      lv_image_get_scale_y(image) != LV_SCALE_NONE || lv_image_get_rotation(image) != 0 ||
      lv_image_get_offset_x(image) != 0 || lv_image_get_offset_y(image) != 0) {
    return PresentBlock::Transformed;
  }
  for (const lv_obj_t* o = image; o != nullptr; o = lv_obj_get_parent(o)) {
    if (transformed_style(o)) return PresentBlock::Transformed;
  }

  if (lv_obj_get_child_count(image) != 0) return PresentBlock::HasChildren;

  lv_area_t area;
  lv_obj_get_coords(image, &area);
  lv_area_t content;
  lv_obj_get_content_coords(image, &content);
  if (lv_area_get_width(&area) != static_cast<int32_t>(dsc.header.w) ||
      lv_area_get_height(&area) != static_cast<int32_t>(dsc.header.h) || content.x1 != area.x1 ||
      content.y1 != area.y1 || content.x2 != area.x2 || content.y2 != area.y2 ||
      lv_obj_get_style_border_width(image, LV_PART_MAIN) != 0) {
    return PresentBlock::SizeMismatch;
  }

  const lv_area_t screen_area{0, 0, lv_display_get_horizontal_resolution(disp) - 1,
                              lv_display_get_vertical_resolution(disp) - 1};
  if (!inside(area, screen_area)) return PresentBlock::Clipped;
  for (const lv_obj_t* p = lv_obj_get_parent(image); p != nullptr; p = lv_obj_get_parent(p)) {
    if (lv_obj_is_overflow_visible(p)) continue;
    lv_area_t pa;
    lv_obj_get_coords(p, &pa);
    if (!inside(area, pa)) return PresentBlock::Clipped;
  }
  CoverWalk walk{image, area};
  walk.visit(screen, false);
  // The top and system layers draw over every screen. The layer objects themselves are
  // transparent and screen-sized, so only what is in them counts.
  for (lv_obj_t* layer : {lv_display_get_layer_top(disp), lv_display_get_layer_sys(disp)}) {
    if (layer == nullptr) continue;
    lv_obj_update_layout(layer);
    walk.after = true;
    walk.visit(layer, false);
  }
  if (walk.covered) return PresentBlock::Covered;
  return PresentBlock::Ok;
}

TileShown show_tile_frame(lv_obj_t* image, lv_image_dsc_t& dsc, const std::uint16_t* frame,
                          Presenter& presenter, PresentBlock* why) {
  // The image cache is off in these builds (components/hearthmap/README.md), so pointing the
  // descriptor at new pixels is all LVGL needs to draw them on its next repaint.
  dsc.data = reinterpret_cast<const std::uint8_t*>(frame);
  const PresentBlock verdict = present_check(image, dsc);
  if (why != nullptr) *why = verdict;
  if (verdict == PresentBlock::Ok) {
    lv_area_t a;
    lv_obj_get_coords(image, &a);
    const int w = static_cast<int>(dsc.header.w);
    const View565 tile{frame, w, static_cast<int>(dsc.header.h), w};
    if (presenter.present(a.x1, a.y1, tile)) return TileShown::Presented;
  }
  if (image != nullptr) lv_obj_invalidate(image);
  return TileShown::Invalidated;
}

}  // namespace hearthmap::ui
