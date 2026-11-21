// map_bake.h — the map's static drawing (walls and room labels) baked into the heat image
// (docs/decisions.md D-055).
//
// LVGL draws the walls and labels once, band by band, into memory, over black and over white.
// From the two the bake keeps, per pixel, the drawing premultiplied by its coverage (the render
// over black) and how much of what lies beneath still shows (from the difference). Applying it
// to rows of a freshly rasterised heat image is then one integer blend per pixel, done in the
// embedder's sliced heat work; once the shown image carries it, the wall and label objects are
// hidden and LVGL no longer draws them on any refresh.
#pragma once

#include <cstdint>
#include <vector>

#include "lvgl.h"

namespace hearthmap::ui::detail {

class MapBake {
 public:
  static constexpr std::int32_t kBandRows = 8;  // two renders of 8 x 920 px: well under 20 ms

  /// `map` is the map container at `area` (screen px). `draw` are the objects to bake (walls,
  /// labels); `hide` everything else that may draw over the map (the heat image, pins, pulse
  /// sprites), hidden while a band is rendered.
  MapBake(lv_obj_t* map, const lv_area_t& area, std::vector<lv_obj_t*> draw,
          std::vector<lv_obj_t*> hide);

  /// Bytes the bake needs (3 per map pixel); the embedder decides whether it can afford them.
  std::size_t bytes_needed() const;
  /// Start (allocating the bake). Without it, prepare_step() does nothing and ready() stays false.
  void start();
  /// One band. Returns true while bands remain. A band whose render fails is tried again; after
  /// kMaxBandFailures failures in a row the bake gives up: ready() stays false, the memory is
  /// freed, and the objects stay as they are.
  bool prepare_step();
  bool ready() const { return ready_; }
  bool failed() const { return failed_; }
  static constexpr int kMaxBandFailures = 3;

  /// The renderer (render_area()); tests replace it to make bands fail.
  using RenderFn = bool (*)(lv_obj_t*, const lv_area_t&, std::uint16_t*, std::uint16_t);
  void set_renderer(RenderFn fn) { render_ = fn; }

  /// Lay the walls and labels over rows [row_begin, row_end) of a map-sized RGB565 image with
  /// `stride` pixels per row. Returns false (and writes nothing) until ready().
  bool apply(std::uint16_t* image, std::int32_t stride, std::int32_t row_begin,
             std::int32_t row_end) const;

  /// Show or hide the baked objects (hidden while the shown image carries them).
  void set_objects_hidden(bool hidden);
  /// `o` is hidden while bands are rendered, like the objects given at construction.
  void hide_too(lv_obj_t* o) { hide_.push_back(o); }
  void forget_objects();

 private:
  lv_obj_t* map_;
  lv_area_t area_;
  std::int32_t w_, h_;
  std::vector<lv_obj_t*> draw_;
  std::vector<lv_obj_t*> hide_;
  std::vector<std::uint16_t> premul_;   // the drawing over black
  std::vector<std::uint8_t> through_;   // 0..255: how much of the image beneath shows
  std::vector<std::uint16_t> scratch_;  // one band over white
  std::int32_t next_row_ = 0;
  bool started_ = false;
  bool ready_ = false;
  bool failed_ = false;
  int band_failures_ = 0;
  RenderFn render_ = nullptr;
  bool alive_ = true;
};

}  // namespace hearthmap::ui::detail
