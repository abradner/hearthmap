#include "sprites.h"

#include <algorithm>
#include <cmath>

// render_area() reuses LVGL's refresh internals (layers, the draw dispatcher), exactly as LVGL's
// own snapshot does. The root of the LVGL tree is on the include path in both builds.
#include "lvgl_private.h"

namespace hearthmap::ui::detail {

namespace {

constexpr std::uint32_t kMaxStalledDispatches = 512;

std::uint32_t pending_tasks(const lv_layer_t* layer) {
  std::uint32_t n = 0;
  for (; layer != nullptr; layer = layer->next) {
    for (const lv_draw_task_t* t = layer->draw_task_head; t != nullptr; t = t->next) ++n;
  }
  return n;
}

void discard_tasks(lv_layer_t* layer) {
  lv_draw_task_t* t = layer->draw_task_head;
  while (t != nullptr) {
    lv_draw_task_t* next = t->next;
    if (t->type == LV_DRAW_TASK_TYPE_LAYER) {
      auto* dsc = static_cast<lv_draw_image_dsc_t*>(t->draw_dsc);
      auto* sub = static_cast<lv_layer_t*>(const_cast<void*>(dsc->src));
      if (sub != nullptr) discard_tasks(sub);
    }
    lv_draw_cleanup_task(t);
    t = next;
  }
  layer->draw_task_head = nullptr;
}

std::uint8_t clamp255(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 255.0f) return 255;
  return static_cast<std::uint8_t>(v + 0.5f);
}

}  // namespace

// Follows LVGL 9.6's lv_snapshot_take_to_draw_buf() (src/draw/snapshot/lv_snapshot.c), with an
// arbitrary screen area instead of one object's area: find the top-most object that covers the
// area, draw it and everything above it into a layer over our buffer, and run the dispatcher
// until the layer's tasks are done. The same thing a refresh does for one area, minus the flush.
bool render_area(lv_obj_t* subtree, const lv_area_t& area, std::uint16_t* out) {
  const std::int32_t w = lv_area_get_width(&area);
  const std::int32_t h = lv_area_get_height(&area);
  if (subtree == nullptr || out == nullptr || w <= 0 || h <= 0) return false;
  lv_obj_t* screen = subtree;
  lv_obj_update_layout(lv_obj_get_screen(subtree));

  lv_draw_buf_t buf;
  if (lv_draw_buf_init(&buf, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                       LV_COLOR_FORMAT_RGB565, static_cast<std::uint32_t>(w) * 2U, out,
                       static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(h) * 2U) !=
      LV_RESULT_OK) {
    return false;
  }

  lv_obj_t* top = lv_refr_get_top_obj(&area, screen);
  if (top == nullptr) {
    lv_draw_buf_clear(&buf, nullptr);
    top = screen;
  }

  lv_layer_t layer;
  lv_layer_init(&layer);
  layer.draw_buf = &buf;
  layer.display = lv_obj_get_display(screen);
  layer.buf_area = area;
  layer.color_format = LV_COLOR_FORMAT_RGB565;
  layer._clip_area = area;
  layer.phy_clip_area = area;
  lv_draw_unit_send_event(nullptr, LV_EVENT_CHILD_CREATED, &layer);

  lv_display_t* disp_old = lv_refr_get_disp_refreshing();
  lv_display_t* disp = lv_obj_get_display(screen);
  lv_layer_t* layer_old = disp->layer_head;
  disp->layer_head = &layer;
  lv_refr_set_disp_refreshing(disp);

  if (top == screen) {
    lv_obj_redraw(&layer, top);
  } else {
    lv_obj_refr(&layer, top);
    lv_obj_t* parent = lv_obj_get_parent(top);
    lv_obj_t* border = top;
    // Up to the subtree's root only: its younger siblings (an overlay, the HUD) are not drawn.
    while (parent != nullptr && border != subtree) {
      bool go = false;
      const std::uint32_t n = lv_obj_get_child_count(parent);
      for (std::uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = parent->spec_attr->children[i];
        if (!go) {
          go = child == border;
        } else {
          lv_obj_refr(&layer, child);
        }
      }
      lv_obj_send_event(parent, LV_EVENT_DRAW_POST_BEGIN, &layer);
      lv_obj_send_event(parent, LV_EVENT_DRAW_POST, &layer);
      lv_obj_send_event(parent, LV_EVENT_DRAW_POST_END, &layer);
      border = parent;
      parent = lv_obj_get_parent(parent);
    }
  }
  layer.all_tasks_added = true;

  bool ok = true;
  std::uint32_t stalled = 0;
  std::uint32_t last = pending_tasks(&layer);
  while (layer.draw_task_head != nullptr) {
    lv_draw_dispatch();
    const std::uint32_t now = pending_tasks(&layer);
    if (now != last) {
      last = now;
      stalled = 0;
    } else if (++stalled > kMaxStalledDispatches) {
      lv_draw_wait_for_finish();
      discard_tasks(&layer);
      ok = false;
      break;
    }
  }

  disp->layer_head = layer_old;
  lv_refr_set_disp_refreshing(disp_old);
  if (ok) lv_draw_unit_send_event(nullptr, LV_EVENT_SCREEN_LOAD_START, &layer);
  lv_draw_unit_send_event(nullptr, LV_EVENT_CHILD_DELETED, &layer);
  return ok;
}

void opaque_image_cover_cb(lv_event_t* e) {
  auto* info = static_cast<lv_cover_check_info_t*>(lv_event_get_param(e));
  if (info == nullptr || info->res == LV_COVER_RES_MASKED) return;
  lv_obj_t* img = lv_event_get_current_target_obj(e);
  if (lv_obj_is_hidden(img)) return;
  const void* src = lv_image_get_src(img);
  if (src == nullptr || lv_image_src_get_type(src) != LV_IMAGE_SRC_VARIABLE) return;
  const auto* dsc = static_cast<const lv_image_dsc_t*>(src);
  if (dsc->header.cf != LV_COLOR_FORMAT_RGB565 || dsc->data == nullptr) return;
  if (lv_obj_get_style_image_opa(img, LV_PART_MAIN) < LV_OPA_MAX) return;
  if (lv_obj_get_style_opa(img, LV_PART_MAIN) < LV_OPA_MAX) return;
  if (lv_image_get_scale_x(img) != LV_SCALE_NONE || lv_image_get_scale_y(img) != LV_SCALE_NONE ||
      lv_image_get_rotation(img) != 0)
    return;
  // Drawn where it stands: an offset (or a tiled or stretched alignment) would leave pixels of
  // the widget the image does not cover.
  if (lv_image_get_offset_x(img) != 0 || lv_image_get_offset_y(img) != 0) return;
  const lv_image_align_t align = lv_image_get_inner_align(img);
  if (align != LV_IMAGE_ALIGN_DEFAULT && align != LV_IMAGE_ALIGN_CENTER &&
      align != LV_IMAGE_ALIGN_TOP_LEFT)
    return;
  lv_area_t coords;
  lv_obj_get_coords(img, &coords);
  if (static_cast<std::int32_t>(dsc->header.w) != lv_area_get_width(&coords) ||
      static_cast<std::int32_t>(dsc->header.h) != lv_area_get_height(&coords))
    return;
  if (lv_area_is_in(info->area, &coords, 0)) info->res = LV_COVER_RES_COVER;
}

void distance_table(std::int32_t x0, std::int32_t y0, std::int32_t w, std::int32_t h, float cx,
                    float cy, std::vector<std::uint16_t>& out) {
  out.resize(static_cast<std::size_t>(std::max<std::int32_t>(0, w)) *
             static_cast<std::size_t>(std::max<std::int32_t>(0, h)));
  std::size_t i = 0;
  for (std::int32_t y = 0; y < h; ++y) {
    const float dy = static_cast<float>(y0 + y) + 0.5f - cy;
    for (std::int32_t x = 0; x < w; ++x, ++i) {
      const float dx = static_cast<float>(x0 + x) + 0.5f - cx;
      const float d = std::sqrt(dx * dx + dy * dy) * 16.0f;
      out[i] = d >= 65535.0f ? 65535 : static_cast<std::uint16_t>(d + 0.5f);
    }
  }
}

void ring_lut(float radius, float width, std::uint8_t opa, std::size_t size,
              std::vector<std::uint8_t>& out) {
  out.assign(size, 0);
  const float half = width * 0.5f;
  for (std::size_t i = 0; i < size; ++i) {
    const float d = static_cast<float>(i) / 16.0f;
    // Coverage of the band [radius - half, radius + half] by a pixel at distance d, linear over
    // one pixel at each edge.
    const float inner = d - (radius - half) + 0.5f;
    const float outer = (radius + half) - d + 0.5f;
    const float c = std::min(1.0f, std::min(inner, outer));
    out[i] = c <= 0.0f ? 0 : clamp255(c * static_cast<float>(opa));
  }
}

void add_segment_coverage(std::int32_t x0, std::int32_t y0, std::int32_t w, std::int32_t h,
                          float ax, float ay, float bx, float by, float width,
                          std::vector<std::uint8_t>& cov) {
  const std::size_t n = static_cast<std::size_t>(std::max<std::int32_t>(0, w)) *
                        static_cast<std::size_t>(std::max<std::int32_t>(0, h));
  if (cov.size() != n) cov.assign(n, 0);
  const float vx = bx - ax;
  const float vy = by - ay;
  const float len2 = vx * vx + vy * vy;
  const float half = width * 0.5f;
  std::size_t i = 0;
  for (std::int32_t y = 0; y < h; ++y) {
    const float py = static_cast<float>(y0 + y) + 0.5f;
    for (std::int32_t x = 0; x < w; ++x, ++i) {
      const float px = static_cast<float>(x0 + x) + 0.5f;
      float t = len2 > 0.0f ? ((px - ax) * vx + (py - ay) * vy) / len2 : 0.0f;
      t = std::min(1.0f, std::max(0.0f, t));
      const float dx = px - (ax + t * vx);
      const float dy = py - (ay + t * vy);
      const float d = std::sqrt(dx * dx + dy * dy);
      const std::uint8_t c = clamp255((half - d + 0.5f) * 255.0f);
      if (c > cov[i]) cov[i] = c;
    }
  }
}

}  // namespace hearthmap::ui::detail

namespace hearthmap::ui {

void SpriteComposer::compose(const ComposeJob& j) const {
  for (std::int32_t y = j.y0; y < j.y1; ++y) {
    const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(j.stride);
    for (std::int32_t x = j.x0; x < j.x1; ++x) {
      const std::size_t i = row + static_cast<std::size_t>(x);
      const std::uint16_t b = j.below[i];
      if (j.full[i] != b) {
        j.out[i] = j.full[i];
        continue;
      }
      std::uint16_t c = b;
      for (std::size_t k = 0; k < j.overlay_count; ++k) {
        const Overlay& o = j.overlays[k];
        if (o.opa == 0) continue;
        const std::uint32_t a = (static_cast<std::uint32_t>(o.coverage[i]) * o.opa + 127U) / 255U;
        c = blend565(c, o.colour, a);
      }
      for (std::size_t k = 0; k < j.ring_count; ++k) {
        const RingLayer& r = j.rings[k];
        if (r.alpha == nullptr) continue;
        const std::size_t d = r.distance[i];
        if (d < r.size) c = blend565(c, j.ring_colour, r.alpha[d]);
      }
      j.out[i] = c;
    }
  }
}

const SpriteComposer& cpu_sprite_composer() {
  static const SpriteComposer composer;
  return composer;
}

}  // namespace hearthmap::ui
