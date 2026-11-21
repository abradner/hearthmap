#include "pulse_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>

namespace hearthmap::ui::detail {

namespace {

constexpr std::int32_t kHaloWidth = PulseLayer::kRingWidth + theme::kPulseHaloExtra;
constexpr std::int32_t kRingReach = PulseLayer::kRingTo + kHaloWidth / 2 + 2;
constexpr std::size_t kRingLutSize =
    static_cast<std::size_t>(PulseLayer::kRingTo + kHaloWidth + 2) * 16U;
constexpr std::int32_t kRoomHaloWidth = PulseLayer::kRoomWidth + theme::kPulseHaloExtra;
constexpr std::uint32_t kRingFrames = PulseLayer::kCycleMs / PulseLayer::kRingFrameMs;
constexpr std::uint32_t kRoomSteps = PulseLayer::kCycleMs / PulseLayer::kRoomStepMs;
constexpr std::uint32_t kMaxDivisor = 4;
// Phases are whole multiples of the coarsest frame time, so every tile at any divisor shows its
// neighbours exactly as they show themselves.
constexpr std::uint32_t kRingPhaseMs = PulseLayer::kRingFrameMs * kMaxDivisor;
constexpr std::uint32_t kRoomPhaseMs = PulseLayer::kRoomStepMs * kMaxDivisor;

std::uint8_t halo_opa(std::uint8_t opa) {
  return static_cast<std::uint8_t>((static_cast<std::uint32_t>(opa) * theme::kPulseHaloOpa + 127U) /
                                   255U);
}

bool intersects(const lv_area_t& a, const lv_area_t& b) {
  return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

// Out to whole kTileAlign blocks (D-053): a driver's rounding of a repaint inside then stays in.
lv_area_t align_out(lv_area_t a) {
  constexpr std::int32_t k = theme::kTileAlign;
  auto down = [](std::int32_t v) { return v >= 0 ? v / k * k : -((-v + k - 1) / k * k); };
  a.x1 = down(a.x1);
  a.y1 = down(a.y1);
  a.x2 = down(a.x2 + k) - 1;
  a.y2 = down(a.y2 + k) - 1;
  return a;
}

// The largest aligned area inside `a`.
lv_area_t align_in(lv_area_t a) {
  constexpr std::int32_t k = theme::kTileAlign;
  auto up = [](std::int32_t v) { return v >= 0 ? (v + k - 1) / k * k : -((-v) / k * k); };
  a.x1 = up(a.x1);
  a.y1 = up(a.y1);
  a.x2 = up(a.x2 + 1 - (k - 1)) - 1;
  a.y2 = up(a.y2 + 1 - (k - 1)) - 1;
  return a;
}

bool clip(lv_area_t& a, const lv_area_t& to) {
  a.x1 = std::max(a.x1, to.x1);
  a.y1 = std::max(a.y1, to.y1);
  a.x2 = std::min(a.x2, to.x2);
  a.y2 = std::min(a.y2, to.y2);
  return a.x1 <= a.x2 && a.y1 <= a.y2;
}

// The extent of a pin container and everything it draws (value pill, badge), screen px.
lv_area_t pin_extent(lv_obj_t* pin) {
  lv_area_t a;
  lv_obj_get_coords(pin, &a);
  const std::uint32_t n = lv_obj_get_child_count(pin);
  for (std::uint32_t i = 0; i < n; ++i) {
    lv_area_t c;
    lv_obj_get_coords(lv_obj_get_child(pin, static_cast<std::int32_t>(i)), &c);
    a.x1 = std::min(a.x1, c.x1);
    a.y1 = std::min(a.y1, c.y1);
    a.x2 = std::max(a.x2, c.x2);
    a.y2 = std::max(a.y2, c.y2);
  }
  a.x1 -= 2;
  a.y1 -= 2;
  a.x2 += 2;
  a.y2 += 2;
  return a;
}

// Whether the segment a-b passes through `r` (Liang-Barsky clipping); `t0`..`t1` is the part
// inside, as fractions of a-b.
bool segment_meets(const lv_area_t& r, float ax, float ay, float bx, float by, float& t0,
                   float& t1) {
  t0 = 0.0f;
  t1 = 1.0f;
  const float dx = bx - ax, dy = by - ay;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {ax - static_cast<float>(r.x1), static_cast<float>(r.x2) - ax,
                      ay - static_cast<float>(r.y1), static_cast<float>(r.y2) - ay};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0.0f) {
      if (q[i] < 0.0f) return false;
      continue;
    }
    const float t = q[i] / p[i];
    if (p[i] < 0.0f) {
      t0 = std::max(t0, t);
    } else {
      t1 = std::min(t1, t);
    }
    if (t0 > t1) return false;
  }
  return true;
}

// The box, inside `cell`, of a stroke along a-b reaching `pad` px either side (empty: false).
bool stroke_box(const lv_area_t& cell, float ax, float ay, float bx, float by, std::int32_t pad,
                lv_area_t& out) {
  const lv_area_t reach{cell.x1 - pad, cell.y1 - pad, cell.x2 + pad, cell.y2 + pad};
  float t0 = 0.0f, t1 = 1.0f;
  if (!segment_meets(reach, ax, ay, bx, by, t0, t1)) return false;
  const float x0 = ax + (bx - ax) * t0, y0 = ay + (by - ay) * t0;
  const float x1 = ax + (bx - ax) * t1, y1 = ay + (by - ay) * t1;
  out = {static_cast<std::int32_t>(std::floor(std::min(x0, x1))) - pad,
         static_cast<std::int32_t>(std::floor(std::min(y0, y1))) - pad,
         static_cast<std::int32_t>(std::ceil(std::max(x0, x1))) + pad,
         static_cast<std::int32_t>(std::ceil(std::max(y0, y1))) + pad};
  return clip(out, cell);
}

// `area` (screen px) as a box of the tile at `tile` (empty when they do not meet).
TileBox local_box(lv_area_t area, const lv_area_t& tile) {
  if (!clip(area, tile)) return TileBox{};
  return TileBox{area.x1 - tile.x1, area.y1 - tile.y1, area.x2 - tile.x1 + 1,
                 area.y2 - tile.y1 + 1};
}

}  // namespace

PulseLayer::PulseLayer(lv_obj_t* map, lv_point_t map_origin, lv_point_t map_size,
                       const std::vector<std::vector<lv_point_precise_t>>& room_outlines,
                       const std::vector<lv_point_t>& pin_centres, std::vector<lv_obj_t*> pins,
                       PulseBudget budget, const SpriteComposer& ops)
    : map_(map), budget_(budget), ops_(&ops), origin_(map_origin), pins_(std::move(pins)) {
  map_area_ = {map_origin.x, map_origin.y, map_origin.x + map_size.x - 1,
               map_origin.y + map_size.y - 1};
  // Tiles live inside the aligned part of the map (a pulse within kTileAlign px of an unaligned
  // map edge is cut there).
  tile_area_ = align_in(map_area_);
  // Ring frames: radius 26 -> 52 px, opacity 1 -> 0, linearly over the cycle (the UI spec's
  // ripple); room steps: opacity 0.35 -> 1 -> 0.35 on a cosine ease.
  ring_alpha_.resize(kRingFrames);
  ring_halo_.resize(kRingFrames);
  for (std::uint32_t f = 0; f < kRingFrames; ++f) {
    const float t = static_cast<float>(f) / static_cast<float>(kRingFrames);
    const float radius =
        static_cast<float>(kRingFrom) + static_cast<float>(kRingTo - kRingFrom) * t;
    const auto opa = static_cast<std::uint8_t>(255.0f * (1.0f - t) + 0.5f);
    ring_lut(radius, static_cast<float>(kRingWidth), opa, kRingLutSize, ring_alpha_[f]);
    ring_lut(radius, static_cast<float>(kHaloWidth), halo_opa(opa), kRingLutSize, ring_halo_[f]);
  }
  room_opa_.resize(kRoomSteps);
  for (std::uint32_t s = 0; s < kRoomSteps; ++s) {
    const float t = static_cast<float>(s) / static_cast<float>(kRoomSteps);
    const float ease = 0.5f - 0.5f * std::cos(6.2831853f * t);
    const float lo = static_cast<float>(theme::kRoomPulseMinOpa);
    room_opa_[s] = static_cast<std::uint8_t>(lo + (255.0f - lo) * ease + 0.5f);
  }

  // Room outlines. Each room has an empty marker (its state, for the embedder); the outline
  // tiles are the cells of a kCellPx grid over the tile area that some room's stroke (with its
  // keyline) passes through, each cut down to the aligned box of the strokes inside it. A cell
  // shared by two rooms is one tile composing both, so no outline tile overlaps another and LVGL
  // draws nothing after one but the rings (D-058: a tile something later reaches into cannot be
  // presented).
  constexpr std::int32_t pad = kRoomHaloWidth;
  constexpr std::int32_t cell = kCellPx;
  static_assert(cell % theme::kTileAlign == 0 && cell <= 128, "cells must stay presentable");
  std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> cell_at;
  auto cell_rect = [this](std::int32_t cx, std::int32_t cy) {
    const std::int32_t x = tile_area_.x1 + cx * cell, y = tile_area_.y1 + cy * cell;
    return lv_area_t{x, y, x + cell - 1, y + cell - 1};
  };
  rooms_.resize(room_outlines.size());
  for (std::size_t r = 0; r < room_outlines.size(); ++r) {
    const auto& pts = room_outlines[r];
    Room& room = rooms_[r];
    room.phase_ms =
        static_cast<std::uint32_t>(((r * 5U) % (kCycleMs / kRoomPhaseMs)) * kRoomPhaseMs);
    std::int32_t x1 = LV_COORD_MAX, y1 = LV_COORD_MAX, x2 = -LV_COORD_MAX, y2 = -LV_COORD_MAX;
    for (const auto& p : pts) {
      x1 = std::min(x1, static_cast<std::int32_t>(p.x));
      y1 = std::min(y1, static_cast<std::int32_t>(p.y));
      x2 = std::max(x2, static_cast<std::int32_t>(p.x));
      y2 = std::max(y2, static_cast<std::int32_t>(p.y));
    }
    if (pts.empty()) x1 = y1 = x2 = y2 = 0;
    room.extent = {origin_.x + x1 - pad, origin_.y + y1 - pad, origin_.x + x2 + pad,
                   origin_.y + y2 + pad};
    // Created before the outline container, so drawn before every tile (and it draws nothing).
    lv_obj_t* marker = lv_obj_create(map_);
    lv_obj_remove_style_all(marker);
    lv_obj_set_clickable(marker, false);
    lv_obj_set_scrollable(marker, false);
    lv_obj_set_size(marker, 0, 0);
    lv_obj_set_hidden(marker, true);
    room_marker_.push_back(marker);
    for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
      Segment seg;
      seg.ax = static_cast<float>(origin_.x + static_cast<std::int32_t>(pts[k].x));
      seg.ay = static_cast<float>(origin_.y + static_cast<std::int32_t>(pts[k].y));
      seg.bx = static_cast<float>(origin_.x + static_cast<std::int32_t>(pts[k + 1].x));
      seg.by = static_cast<float>(origin_.y + static_cast<std::int32_t>(pts[k + 1].y));
      room.segments.push_back(seg);
      lv_area_t box{static_cast<std::int32_t>(std::min(seg.ax, seg.bx)) - pad,
                    static_cast<std::int32_t>(std::min(seg.ay, seg.by)) - pad,
                    static_cast<std::int32_t>(std::max(seg.ax, seg.bx)) + pad,
                    static_cast<std::int32_t>(std::max(seg.ay, seg.by)) + pad};
      if (!clip(box, tile_area_)) continue;
      for (std::int32_t cy = (box.y1 - tile_area_.y1) / cell; cy <= (box.y2 - tile_area_.y1) / cell;
           ++cy) {
        for (std::int32_t cx = (box.x1 - tile_area_.x1) / cell;
             cx <= (box.x2 - tile_area_.x1) / cell; ++cx) {
          const lv_area_t c = cell_rect(cx, cy);
          lv_area_t part;
          if (!stroke_box(c, seg.ax, seg.ay, seg.bx, seg.by, pad, part)) continue;
          const auto [it, fresh] = cell_at.emplace(std::make_pair(cx, cy), cells_.size());
          if (fresh) {
            cells_.emplace_back();
            cells_.back().t.area = part;
            cells_.back().rect = c;
            clip(cells_.back().rect, tile_area_);
          }
          Cell& cl = cells_[it->second];
          lv_area_t& a = cl.t.area;
          a = {std::min(a.x1, part.x1), std::min(a.y1, part.y1), std::max(a.x2, part.x2),
               std::max(a.y2, part.y2)};
          if (cl.rooms.empty() || cl.rooms.back() != r) cl.rooms.push_back(r);
          if (std::find(room.cells.begin(), room.cells.end(), it->second) == room.cells.end()) {
            room.cells.push_back(it->second);
          }
        }
      }
    }
  }
  // One container for every outline tile, the tile area's size: LVGL looks for a covering object
  // only inside parents that contain the whole repaint.
  outline_box_ = lv_obj_create(map_);
  lv_obj_remove_style_all(outline_box_);
  lv_obj_set_clickable(outline_box_, false);
  lv_obj_set_scrollable(outline_box_, false);
  lv_obj_set_pos(outline_box_, tile_area_.x1 - origin_.x, tile_area_.y1 - origin_.y);
  lv_obj_set_size(outline_box_, lv_area_get_width(&tile_area_), lv_area_get_height(&tile_area_));
  lv_obj_add_event_cb(outline_box_, opaque_tiles_cover_cb, LV_EVENT_COVER_CHECK, nullptr);
  for (const auto& [key, index] : cell_at) {
    Cell& cl = cells_[index];
    // Aligned out (stays inside its cell, which is aligned), and inside the tile area.
    cl.t.area = align_out(cl.t.area);
    clip(cl.t.area, cell_rect(key.first, key.second));
    clip(cl.t.area, tile_area_);
    make_tile(cl.t, outline_box_, lv_point_t{tile_area_.x1, tile_area_.y1});
  }

  // Rings: one tile per pin, the ring's whole reach, clipped to the map.
  rings_.resize(pin_centres.size());
  for (std::size_t i = 0; i < pin_centres.size(); ++i) {
    Ring& ring = rings_[i];
    ring.cx = static_cast<float>(origin_.x + pin_centres[i].x);
    ring.cy = static_cast<float>(origin_.y + pin_centres[i].y);
    ring.t.phase_ms =
        static_cast<std::uint32_t>(((i * 7U) % (kCycleMs / kRingPhaseMs)) * kRingPhaseMs);
    const std::int32_t cx = origin_.x + pin_centres[i].x;
    const std::int32_t cy = origin_.y + pin_centres[i].y;
    ring.t.area = {cx - kRingReach, cy - kRingReach, cx + kRingReach - 1, cy + kRingReach - 1};
    ring.t.area = align_out(ring.t.area);
    if (!clip(ring.t.area, tile_area_)) ring.t.area = align_out(lv_area_t{cx, cy, cx, cy});
    make_tile(ring.t, map_, origin_);
    ring_img_.push_back(ring.t.img);
  }
}

PulseLayer::~PulseLayer() {
  if (timer_ != nullptr && alive_) lv_timer_delete(timer_);
}

void PulseLayer::forget_objects() {
  alive_ = false;
  if (timer_ != nullptr) lv_timer_delete(timer_);
  timer_ = nullptr;
  for (Ring& r : rings_) r.t.img = nullptr;
  for (Cell& c : cells_) c.t.img = nullptr;
  outline_box_ = nullptr;
  ring_img_.clear();
  room_marker_.clear();
  due_.clear();
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
// lv_image_header_t is a set of bit-fields; a tile is at most the screen's size.
#pragma GCC diagnostic ignored "-Wconversion"
#endif
void PulseLayer::make_tile(Tile& t, lv_obj_t* parent, lv_point_t parent_origin) {
  t.img = lv_image_create(parent);
  lv_obj_remove_style_all(t.img);
  lv_obj_set_clickable(t.img, false);
  lv_obj_add_event_cb(t.img, opaque_image_cover_cb, LV_EVENT_COVER_CHECK, nullptr);
  lv_obj_set_hidden(t.img, true);
  size_tile(t, parent_origin);
}

void PulseLayer::size_tile(Tile& t, lv_point_t parent_origin) {
  t.w = lv_area_get_width(&t.area);
  t.h = lv_area_get_height(&t.area);
  t.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  t.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  t.dsc.header.w = static_cast<std::uint32_t>(t.w);
  t.dsc.header.h = static_cast<std::uint32_t>(t.h);
  t.dsc.header.stride = static_cast<std::uint32_t>(t.w) * 2U;
  t.dsc.data_size = static_cast<std::uint32_t>(t.w) * static_cast<std::uint32_t>(t.h) * 2U;
  if (!alive_ || t.img == nullptr) return;
  lv_obj_set_pos(t.img, t.area.x1 - parent_origin.x, t.area.y1 - parent_origin.y);
  lv_obj_set_size(t.img, t.w, t.h);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void PulseLayer::release(Tile& t) {
  if (job_.tile == &t) {
    std::vector<std::uint16_t>().swap(job_.next.px);
    job_ = Job{};
  }
  if (alive_ && t.img != nullptr) {
    lv_obj_set_hidden(t.img, true);
    lv_image_set_src(t.img, nullptr);
  }
  // Give the memory back (assigning {} would keep the capacity).
  std::vector<std::uint16_t>().swap(t.below);
  std::vector<std::uint16_t>().swap(t.full);
  std::vector<std::uint16_t>().swap(t.shown.px);
  t.shown.count = 0;
  t.dsc.data = nullptr;
  t.shown_index = -1;
  t.dirty = true;
  t.hidden = false;
  for (std::size_t i = 0; i < due_.size();) {
    if (due_[i].dsc == &t.dsc) {
      due_.erase(due_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

std::size_t PulseLayer::tile_frame_bytes(const Tile& t, bool ring) const {
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  const std::uint32_t frames = (ring ? kRingFrames : kRoomSteps) / divisor_;
  return px * 2U * frames;
}

std::size_t PulseLayer::bytes_in_use(bool with_rooms) const {
  // Per allocated tile, what it holds or what it will hold once prepared (captures, two tables or
  // masks, its frames at the current rate), whichever is more: admission reserves for tiles not
  // yet prepared.
  auto tile = [this](const Tile& t, bool ring, std::size_t tables) {
    const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
    const std::size_t held =
        (t.below.capacity() + t.full.capacity() + t.shown.px.capacity()) * 2U + tables;
    const std::size_t expected = px * 8U + tile_frame_bytes(t, ring);
    return std::max(held, expected);
  };
  std::size_t n = job_.next.px.capacity() * 2U;
  for (const Ring& r : rings_) {
    if (!r.allocated) continue;
    std::size_t tables = 0;
    for (const RingRef& ref : r.rings) tables += ref.distance.capacity() * 2U;
    for (const RoomRef& ref : r.rooms) tables += ref.coverage.capacity() + ref.halo.capacity();
    n += tile(r.t, true, tables);
  }
  for (const Cell& c : cells_) {
    if (!with_rooms || !cell_live(c)) continue;
    std::size_t tables = 0;
    for (const RoomRef& ref : c.refs) tables += ref.coverage.capacity() + ref.halo.capacity();
    n += tile(c.t, false, tables);
  }
  return n;
}

// The smallest divisor of frames per cycle at which every allocated pulse fits the budget once
// all its frames exist (captures, tables and one tile's next set included). A change recomposes
// every tile at the new rate, the old frames playing meanwhile.
void PulseLayer::choose_divisor() {
  auto need_at = [this](std::uint32_t div) {
    std::size_t n = 0, largest = 0;
    auto add = [&](const Tile& t, bool ring) {
      const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
      const std::size_t frames = px * 2U * ((ring ? kRingFrames : kRoomSteps) / div);
      // Captures, the frames, and a table or mask per overlapping pulse (2 bytes a pixel each,
      // two assumed).
      n += px * 4U + frames + px * 4U;
      largest = std::max(largest, frames);
    };
    for (const Ring& r : rings_) {
      if (r.allocated) add(r.t, true);
    }
    for (const Cell& c : cells_) {
      if (cell_live(c)) add(c.t, false);
    }
    return n + largest;
  };
  std::uint32_t div = 1;
  while (div < kMaxDivisor && need_at(div) > budget_.memory_bytes) div *= 2;
  if (div != divisor_) {
    divisor_ = div;
    mark_all_dirty(false);
  }
}

void PulseLayer::set_ring(std::size_t pin, bool on) {
  if (pin >= rings_.size() || rings_[pin].on == on) return;
  Ring& r = rings_[pin];
  r.on = on;
  if (on) {
    // Admitted while its captures and frames at the coarsest rate fit.
    const std::size_t px = static_cast<std::size_t>(r.t.w) * static_cast<std::size_t>(r.t.h);
    const std::size_t need = px * 8U + px * 2U * (kRingFrames / kMaxDivisor);
    // Rings come first (D-053): outlines give their memory up, the last room first, and wait.
    // Only when giving them all up would make room: never for nothing.
    if (!fits(need) && bytes_in_use(false) + need <= budget_.memory_bytes) {
      for (std::size_t k = rooms_.size(); k-- > 0 && !fits(need);) {
        if (rooms_[k].allocated) drop_room(k);
      }
    }
    if (fits(need)) {
      r.allocated = true;
      r.t.dirty = true;
      fit_cells_under(r.t.area);  // its tile draws the strokes under it now
    }
  } else if (r.allocated) {
    release(r.t);
    r.allocated = false;
    std::vector<RingRef>().swap(r.rings);
    std::vector<RoomRef>().swap(r.rooms);
    fit_cells_under(r.t.area);  // the cells it covered draw their strokes again
    admit_waiting();
  }
  // Neighbours' tiles show this ring (or stop showing it): recompose those it reaches.
  const lv_area_t reach{
      static_cast<std::int32_t>(r.cx) - kRingReach, static_cast<std::int32_t>(r.cy) - kRingReach,
      static_cast<std::int32_t>(r.cx) + kRingReach, static_cast<std::int32_t>(r.cy) + kRingReach};
  for (Ring& x : rings_) {
    if (&x != &r && x.allocated && intersects(x.t.area, reach)) x.t.dirty = true;
  }
  choose_divisor();
  update_visibility();
  ensure_timer();
}

void PulseLayer::set_room(std::size_t room, bool on, std::uint32_t colour) {
  if (room >= rooms_.size() || !alive_) return;
  Room& r = rooms_[room];
  const std::uint16_t c565 = rgb565(colour);
  if (on) lv_obj_set_style_line_color(room_marker_[room], lv_color_hex(colour), 0);
  if (r.on == on && (!on || r.colour == c565)) return;
  r.on = on;
  r.colour = c565;
  if (on && !r.allocated) {
    if (fits(room_need(room))) {
      r.allocated = true;
      fit_cells(r);
    }
  } else if (!on && r.allocated) {
    drop_room(room);
    admit_waiting();
  }
  // Every tile this outline passes through is recomposed (its own, its neighbours', rings').
  mark_dirty(r.extent);
  choose_divisor();
  update_visibility();
  ensure_timer();
}

void PulseLayer::admit_waiting() {
  // Memory came free: pulses waiting for it get it, rings first, then rooms, in config order.
  for (Ring& x : rings_) {
    const std::size_t px = static_cast<std::size_t>(x.t.w) * static_cast<std::size_t>(x.t.h);
    if (x.on && !x.allocated && fits(px * 8U + px * 2U * (kRingFrames / kMaxDivisor))) {
      x.allocated = true;
      x.t.dirty = true;
      fit_cells_under(x.t.area);
    }
  }
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    Room& x = rooms_[k];
    if (!x.on || x.allocated || !fits(room_need(k))) continue;
    x.allocated = true;
    fit_cells(x);
    mark_dirty(x.extent);
  }
}

void PulseLayer::mark_dirty(const lv_area_t& area) {
  for (Ring& r : rings_) {
    if (intersects(r.t.area, area)) r.t.dirty = true;
  }
  for (Cell& c : cells_) {
    if (intersects(c.t.area, area)) c.t.dirty = true;
  }
  cancel_stale_job();
}

void PulseLayer::cancel_stale_job() {
  // A set being composed from captures that are now out of date is dropped and started again.
  if (job_.tile != nullptr && job_.tile->dirty) {
    std::vector<std::uint16_t>().swap(job_.next.px);
    job_ = Job{};
  }
}

void PulseLayer::mark_all_dirty(bool hide) {
  for (Ring& r : rings_) {
    r.t.dirty = true;
    r.t.hidden = r.t.hidden || hide;
  }
  for (Cell& c : cells_) {
    c.t.dirty = true;
    c.t.hidden = c.t.hidden || hide;
  }
  cancel_stale_job();
  update_visibility();
}

void PulseLayer::set_veiled(bool veiled) {
  if (veiled_ == veiled) return;
  veiled_ = veiled;
  update_visibility();
}

void PulseLayer::update_visibility() {
  if (!alive_) return;
  for (Ring& r : rings_) {
    const bool show = r.on && r.allocated && r.t.shown.count > 0 && !r.t.hidden && !veiled_;
    if (lv_obj_is_hidden(r.t.img) == show) lv_obj_set_hidden(r.t.img, !show);
  }
  // An outline appears whole: a cell shows once some pulsing room through it has frames in
  // every one of its cells (a cell being recomposed keeps playing its old set meanwhile).
  for (Cell& c : cells_) {
    bool show = !veiled_ && !c.gone && !c.t.hidden && c.t.shown.count > 0;
    bool any_ready = false;
    for (std::size_t k : c.rooms) {
      const Room& room = rooms_[k];
      any_ready = any_ready || (room.on && room.allocated && room_ready(room));
    }
    show = show && any_ready;
    if (lv_obj_is_hidden(c.t.img) == show) lv_obj_set_hidden(c.t.img, !show);
  }
  for (std::size_t i = 0; i < rooms_.size(); ++i) {
    const bool on = rooms_[i].on;
    if (lv_obj_is_hidden(room_marker_[i]) == on) lv_obj_set_hidden(room_marker_[i], !on);
  }
}

// One of a tile's two captures (a software render of the tile each, so one per prepare step):
// with the pins, then without the pins that reach into the tile. Whatever differs between the
// two is drawn over the pulses.
bool PulseLayer::capture(Tile& t, bool with_pins) {
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  std::vector<std::uint16_t>& out = with_pins ? t.full : t.below;
  if (out.size() != px) out.assign(px, 0);
  lv_display_t* disp = lv_obj_get_display(map_);
  lv_display_enable_invalidation(disp, false);  // a counter in LVGL 9.6: paired below
  // Pulses are never part of what lies under a pulse.
  std::vector<lv_obj_t*> hidden;
  for (Ring& r : rings_) {
    if (!lv_obj_is_hidden(r.t.img) && intersects(r.t.area, t.area)) hidden.push_back(r.t.img);
  }
  if (outline_box_ != nullptr && !lv_obj_is_hidden(outline_box_)) hidden.push_back(outline_box_);
  for (lv_obj_t* o : never_captured_) {
    if (!lv_obj_is_hidden(o)) hidden.push_back(o);
  }
  if (!with_pins) {
    for (lv_obj_t* p : pins_) {
      if (!lv_obj_is_hidden(p) && intersects(pin_extent(p), t.area)) hidden.push_back(p);
    }
  }
  for (lv_obj_t* o : hidden) lv_obj_set_hidden(o, true);
  // Where nothing in the map draws, the screen's background shows: capture that, not black.
  const bool ok = render_area(map_, t.area, out.data(), rgb565(theme::kBackground));
  for (lv_obj_t* o : hidden) lv_obj_set_hidden(o, false);
  lv_display_enable_invalidation(disp, true);
  if (!with_pins) ++stats_.captures;
  return ok;
}

void PulseLayer::rebuild_ring_geometry(std::size_t i) {
  Ring& r = rings_[i];
  r.rings.clear();
  // Its own ring first, so a cap on layers can only drop another ring's overlap.
  std::vector<std::size_t> order{i};
  for (std::size_t j = 0; j < rings_.size(); ++j) {
    if (j != i) order.push_back(j);
  }
  for (std::size_t j : order) {
    const Ring& o = rings_[j];
    if (!o.on || !o.allocated) continue;
    const lv_area_t reach{
        static_cast<std::int32_t>(o.cx) - kRingReach, static_cast<std::int32_t>(o.cy) - kRingReach,
        static_cast<std::int32_t>(o.cx) + kRingReach, static_cast<std::int32_t>(o.cy) + kRingReach};
    if (!intersects(reach, r.t.area)) continue;
    RingRef ref;
    ref.ring = j;
    ref.box = local_box(reach, r.t.area);
    distance_table(r.t.area.x1, r.t.area.y1, r.t.w, r.t.h, o.cx, o.cy, ref.distance);
    r.rings.push_back(std::move(ref));
  }
  r.rooms.clear();
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    const Room& room = rooms_[k];
    if (!room.on || !room.allocated || !intersects(room.extent, r.t.area)) continue;
    RoomRef ref;
    ref.room = k;
    for (const Segment& e : room.segments) {
      const lv_area_t seg{static_cast<std::int32_t>(std::min(e.ax, e.bx)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::min(e.ay, e.by)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(e.ax, e.bx)) + kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(e.ay, e.by)) + kRoomHaloWidth};
      if (!intersects(seg, r.t.area)) continue;
      add_segment_coverage(r.t.area.x1, r.t.area.y1, r.t.w, r.t.h, e.ax, e.ay, e.bx, e.by,
                           static_cast<float>(kRoomWidth), ref.coverage);
      add_segment_coverage(r.t.area.x1, r.t.area.y1, r.t.w, r.t.h, e.ax, e.ay, e.bx, e.by,
                           static_cast<float>(kRoomHaloWidth), ref.halo);
      lv_area_t b = seg;
      if (clip(b, r.t.area)) {
        const TileBox lb = local_box(b, r.t.area);
        if (ref.box.x1 == ref.box.x0) {
          ref.box = lb;
        } else {
          ref.box.x0 = std::min(ref.box.x0, lb.x0);
          ref.box.y0 = std::min(ref.box.y0, lb.y0);
          ref.box.x1 = std::max(ref.box.x1, lb.x1);
          ref.box.y1 = std::max(ref.box.y1, lb.y1);
        }
      }
    }
    if (!ref.coverage.empty()) r.rooms.push_back(std::move(ref));
  }
}

void PulseLayer::rebuild_cell_geometry(Cell& c) {
  c.refs.clear();
  // Only rooms listed for the cell can reach it: their strokes are what made it a cell.
  for (std::size_t k : c.rooms) {
    const Room& room = rooms_[k];
    if (!room.on || !room.allocated) continue;
    RoomRef ref;
    ref.room = k;
    for (const Segment& seg : room.segments) {
      const lv_area_t box{static_cast<std::int32_t>(std::min(seg.ax, seg.bx)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::min(seg.ay, seg.by)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(seg.ax, seg.bx)) + kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(seg.ay, seg.by)) + kRoomHaloWidth};
      if (!intersects(box, c.t.area)) continue;
      add_segment_coverage(c.t.area.x1, c.t.area.y1, c.t.w, c.t.h, seg.ax, seg.ay, seg.bx, seg.by,
                           static_cast<float>(kRoomWidth), ref.coverage);
      add_segment_coverage(c.t.area.x1, c.t.area.y1, c.t.w, c.t.h, seg.ax, seg.ay, seg.bx, seg.by,
                           static_cast<float>(kRoomHaloWidth), ref.halo);
    }
    ref.box = TileBox{0, 0, c.t.w, c.t.h};
    if (!ref.coverage.empty()) c.refs.push_back(std::move(ref));
  }
}

bool PulseLayer::cell_active(const Cell& c) const {
  for (std::size_t k : c.rooms) {
    if (rooms_[k].on && rooms_[k].allocated) return true;
  }
  return false;
}

bool PulseLayer::room_ready(const Room& r) const {
  for (std::size_t c : r.cells) {
    if (!cells_[c].gone && cells_[c].t.shown.count == 0) return false;
  }
  return true;
}

std::size_t PulseLayer::cell_cost(const lv_area_t& area) {
  // Captures and two tables or masks, and its frames at the coarsest rate.
  const std::size_t px = static_cast<std::size_t>(lv_area_get_width(&area)) *
                         static_cast<std::size_t>(lv_area_get_height(&area));
  return px * 8U + px * 2U * (kRoomSteps / kMaxDivisor);
}

lv_area_t PulseLayer::cell_area(const Cell& c, std::size_t with) const {
  bool any = false;
  lv_area_t u{};
  for (std::size_t k : c.rooms) {
    const Room& room = rooms_[k];
    if (k != with && !(room.on && room.allocated)) continue;
    for (const Segment& seg : room.segments) {
      lv_area_t b;
      if (!stroke_box(c.rect, seg.ax, seg.ay, seg.bx, seg.by, kRoomHaloWidth, b)) continue;
      u = any ? lv_area_t{std::min(u.x1, b.x1), std::min(u.y1, b.y1), std::max(u.x2, b.x2),
                          std::max(u.y2, b.y2)}
              : b;
      any = true;
    }
  }
  if (!any) return c.t.area;
  u = align_out(u);
  clip(u, c.rect);
  return u;
}

bool PulseLayer::trim_by_rings(lv_area_t& a) const {
  // Twice over: trimming by one ring can make another's cover whole.
  for (int pass = 0; pass < 2; ++pass) {
    for (const Ring& r : rings_) {
      if (!r.on || !r.allocated) continue;
      const lv_area_t& b = r.t.area;
      if (!intersects(a, b)) continue;
      const bool spans_x = b.x1 <= a.x1 && b.x2 >= a.x2;
      const bool spans_y = b.y1 <= a.y1 && b.y2 >= a.y2;
      if (spans_x && spans_y) return false;
      if (spans_x && b.y1 <= a.y1) {
        a.y1 = b.y2 + 1;
      } else if (spans_x && b.y2 >= a.y2) {
        a.y2 = b.y1 - 1;
      } else if (spans_y && b.x1 <= a.x1) {
        a.x1 = b.x2 + 1;
      } else if (spans_y && b.x2 >= a.x2) {
        a.x2 = b.x1 - 1;
      }
      // Otherwise the ring's tile sits inside the cell's or across one corner: the cell keeps
      // that part, and is covered there (falls back to invalidating).
    }
  }
  return true;
}

void PulseLayer::fit_cell(Cell& c) {
  if (!cell_active(c)) return;
  lv_area_t want = cell_area(c, rooms_.size());
  const bool gone = !trim_by_rings(want);
  const bool same = want.x1 == c.t.area.x1 && want.y1 == c.t.area.y1 && want.x2 == c.t.area.x2 &&
                    want.y2 == c.t.area.y2;
  if (gone == c.gone && (gone || same)) return;
  release(c.t);  // frames of the old size are no use
  std::vector<RoomRef>().swap(c.refs);
  c.gone = gone;
  if (gone) return;
  c.t.area = want;
  size_tile(c.t, lv_point_t{tile_area_.x1, tile_area_.y1});
}

void PulseLayer::fit_cells(const Room& r) {
  for (std::size_t i : r.cells) fit_cell(cells_[i]);
}

void PulseLayer::fit_cells_under(const lv_area_t& area) {
  for (Cell& c : cells_) {
    if (intersects(c.rect, area)) fit_cell(c);
  }
}

std::size_t PulseLayer::room_need(std::size_t room) const {
  std::size_t need = 0;
  for (std::size_t i : rooms_[room].cells) {
    const Cell& c = cells_[i];
    lv_area_t a = cell_area(c, room);
    const std::size_t after = trim_by_rings(a) ? cell_cost(a) : 0U;
    const std::size_t before = cell_live(c) ? cell_cost(c.t.area) : 0U;
    need += after > before ? after - before : 0U;
  }
  return need;
}

void PulseLayer::drop_room(std::size_t room) {
  Room& r = rooms_[room];
  r.allocated = false;
  r.shown_step = -1;
  for (std::size_t i : r.cells) {
    Cell& c = cells_[i];
    if (cell_active(c)) continue;  // another room still pulses through it
    release(c.t);
    std::vector<RoomRef>().swap(c.refs);
  }
  fit_cells(r);  // the cells it shared shrink to the rooms left
  mark_dirty(r.extent);
}

const std::uint8_t* PulseLayer::opa_lut(std::uint8_t opa) {
  if (!opa_lut_ready_[opa]) {
    for (std::uint32_t c = 0; c < 256; ++c) {
      opa_luts_[opa][c] = static_cast<std::uint8_t>((c * opa + 127U) / 255U);
    }
    opa_lut_ready_[opa] = true;
  }
  return opa_luts_[opa].data();
}

int PulseLayer::ring_frame_at(std::size_t ring, std::uint32_t t_ms) const {
  return static_cast<int>(((t_ms + rings_[ring].t.phase_ms) % kCycleMs) / kRingFrameMs);
}

std::uint8_t PulseLayer::room_opa_at(std::size_t room, std::uint32_t t_ms) const {
  const Room& r = rooms_[room];
  return room_opa_[((t_ms + r.phase_ms) % kCycleMs) / kRoomStepMs];
}

bool PulseLayer::is_ring_tile(const Tile* t) const {
  for (const Ring& r : rings_) {
    if (&r.t == t) return true;
  }
  return false;
}

PulseLayer::Ring* PulseLayer::ring_of(const Tile* t) {
  for (Ring& r : rings_) {
    if (&r.t == t) return &r;
  }
  return nullptr;
}

PulseLayer::Cell* PulseLayer::cell_of(const Tile* t) {
  for (Cell& c : cells_) {
    if (&c.t == t) return &c;
  }
  return nullptr;
}

// Frame k of the tile's next set: the moment it stands for is k frames after the tile's phase,
// and every pulse in the tile is drawn as it is at that moment.
void PulseLayer::compose_frame(Tile& t, std::uint32_t k, std::uint16_t* out) {
  const std::uint32_t t_ms = (k * job_.next.frame_ms + kCycleMs - t.phase_ms % kCycleMs) % kCycleMs;
  const std::uint16_t dark = rgb565(theme::kBackground);
  Overlay overlays[32];
  RingLayer layers[32];
  std::size_t no = 0, nr = 0;
  auto add_rooms = [&](const std::vector<RoomRef>& refs) {
    for (const RoomRef& ref : refs) {
      if (no + 2 > 32) break;
      const Room& room = rooms_[ref.room];
      const std::uint8_t opa = room_opa_at(ref.room, t_ms);
      overlays[no++] = Overlay{ref.halo.data(), opa_lut(halo_opa(opa)), dark, ref.box};
      overlays[no++] = Overlay{ref.coverage.data(), opa_lut(opa), room.colour, ref.box};
    }
  };
  if (Ring* r = ring_of(&t)) {
    add_rooms(r->rooms);
    const std::uint16_t danger = rgb565(theme::kDanger);
    for (const RingRef& ref : r->rings) {
      if (nr + 2 > 32) break;
      const auto f = static_cast<std::size_t>(ring_frame_at(ref.ring, t_ms));
      // A dark keyline, then the ring: it reads on a red room as well as on neutral heat.
      layers[nr++] =
          RingLayer{ref.distance.data(), ring_halo_[f].data(), ring_halo_[f].size(), dark, ref.box};
      layers[nr++] = RingLayer{ref.distance.data(), ring_alpha_[f].data(), ring_alpha_[f].size(),
                               danger, ref.box};
    }
  } else if (Cell* c = cell_of(&t)) {
    add_rooms(c->refs);
  }
  ComposeJob job;
  job.below = t.below.data();
  job.full = t.full.data();
  job.out = out;
  job.stride = t.w;
  job.w = t.w;
  job.h = t.h;
  job.overlays = overlays;
  job.overlay_count = no;
  job.rings = layers;
  job.ring_count = nr;
  ops_->compose(job);
}

void PulseLayer::show_frame(Tile& t, int index) {
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  const std::uint16_t* frame = t.shown.px.data() + static_cast<std::size_t>(index) * px;
  t.shown_index = index;
  if (!alive_ || t.img == nullptr) return;
  if (lv_image_get_src(t.img) != &t.dsc) {
    t.dsc.data = reinterpret_cast<const std::uint8_t*>(frame);
    lv_image_set_src(t.img, &t.dsc);
  }
  if (defer_ && !lv_obj_is_hidden(t.img)) {
    // The embedder shows it (presents it, or invalidates): until then the old frame stays.
    for (Due& d : due_) {
      if (d.dsc == &t.dsc) {
        d.frame = frame;
        return;
      }
    }
    due_.push_back(Due{t.img, &t.dsc, frame});
    return;
  }
  t.dsc.data = reinterpret_cast<const std::uint8_t*>(frame);
  if (!lv_obj_is_hidden(t.img)) lv_obj_invalidate(t.img);
}

void PulseLayer::finish_job() {
  Tile& t = *job_.tile;
  // The new set replaces the old whole; the descriptor moves to it before the old is freed.
  std::swap(t.shown, job_.next);
  std::vector<std::uint16_t>().swap(job_.next.px);
  job_ = Job{};
  t.hidden = false;
  const auto index = static_cast<int>(((lv_tick_get() + t.phase_ms) % kCycleMs) / t.shown.frame_ms);
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  t.dsc.data = reinterpret_cast<const std::uint8_t*>(t.shown.px.data() +
                                                     static_cast<std::size_t>(index) * px);
  for (Due& d : due_) {
    if (d.dsc == &t.dsc) d.frame = reinterpret_cast<const std::uint16_t*>(t.dsc.data);
  }
  if (alive_ && t.img != nullptr) {
    if (lv_image_get_src(t.img) != &t.dsc) lv_image_set_src(t.img, &t.dsc);
    t.shown_index = index;
    t.shown_ms = lv_tick_get();
    update_visibility();
    if (!lv_obj_is_hidden(t.img)) lv_obj_invalidate(t.img);
  }
}

bool PulseLayer::prepare_step() {
  if (!alive_) return false;
  struct Timed {
    PulseLayer& self;
    std::uint32_t t0;
    ~Timed() {
      if (self.budget_.micros != nullptr) self.stats_.prepare_us += self.budget_.micros() - t0;
    }
  } timed{*this, budget_.micros != nullptr ? budget_.micros() : 0U};

  if (job_.tile == nullptr) {
    // The next tile that needs new frames: rings first (the plants), then outlines.
    for (Ring& r : rings_) {
      if (r.allocated && r.t.dirty) {
        job_.tile = &r.t;
        break;
      }
    }
    for (std::size_t k = 0; k < rooms_.size() && job_.tile == nullptr; ++k) {
      if (!rooms_[k].on || !rooms_[k].allocated) continue;
      for (std::size_t c : rooms_[k].cells) {
        if (cells_[c].t.dirty && !cells_[c].gone) {
          job_.tile = &cells_[c].t;
          break;
        }
      }
    }
    if (job_.tile == nullptr) return false;
    job_.stage = Job::Stage::CaptureFull;
    job_.tile->dirty = false;
  }
  Tile& t = *job_.tile;
  if (job_.stage == Job::Stage::CaptureFull) {
    if (!capture(t, true)) {
      job_ = Job{};  // waits for the next change under the tile
      return stats().pending;
    }
    job_.stage = Job::Stage::CaptureBelow;
    return true;
  }
  if (job_.stage == Job::Stage::CaptureBelow) {
    if (!capture(t, false)) {
      job_ = Job{};
      return stats().pending;
    }
    const bool ring = is_ring_tile(&t);
    if (ring) {
      for (std::size_t i = 0; i < rings_.size(); ++i) {
        if (&rings_[i].t == &t) rebuild_ring_geometry(i);
      }
    } else if (Cell* c = cell_of(&t)) {
      rebuild_cell_geometry(*c);
    }
    const std::uint32_t base = ring ? kRingFrameMs : kRoomStepMs;
    job_.next.frame_ms = base * divisor_;
    job_.next.count = kCycleMs / job_.next.frame_ms;
    job_.next.px.assign(
        static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * job_.next.count, 0);
    job_.frame = 0;
    job_.stage = Job::Stage::Compose;
    return true;
  }
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  compose_frame(t, job_.frame, job_.next.px.data() + static_cast<std::size_t>(job_.frame) * px);
  if (++job_.frame >= job_.next.count) finish_job();
  return stats().pending;
}

void PulseLayer::tick(std::uint32_t now_ms) {
  if (!alive_ || veiled_) return;
  struct Timed {
    PulseLayer& self;
    std::uint32_t t0;
    ~Timed() {
      if (self.budget_.micros != nullptr) self.stats_.frame_us += self.budget_.micros() - t0;
    }
  } timed{*this, budget_.micros != nullptr ? budget_.micros() : 0U};
  // LVGL keeps 32 invalidated areas per refresh and repaints the whole screen past that: at most
  // kMaxInvalidationsPerTick tiles are invalidated per tick, the rest show their frame one tick
  // (16 ms) later. Deferred (presented) frames have no such limit.
  std::uint32_t invalidations = 0;
  auto step = [this, now_ms, &invalidations](Tile& t) {
    if (t.shown.count == 0 || t.hidden || t.img == nullptr || lv_obj_is_hidden(t.img)) return;
    if (!defer_ && invalidations >= kMaxInvalidationsPerTick) return;
    const std::uint32_t fm = t.shown.frame_ms;
    const auto index = static_cast<int>(((now_ms + t.phase_ms) % kCycleMs) / fm);
    if (index == t.shown_index && now_ms - t.shown_ms < fm) return;
    if (t.shown_index >= 0) {
      // Frame boundaries passed since the last frame shown (wrap-safe), less the one shown now.
      const std::uint32_t passed = (now_ms - t.shown_ms + fm / 2U) / fm;
      if (passed > 1U) stats_.dropped += passed - 1U;
    }
    t.shown_ms = now_ms;
    if (index == t.shown_index) return;  // whole cycles later: the same picture
    show_frame(t, index);
    ++invalidations;
  };
  // Frames are counted per pulse (a ring, or a room's outline), not per tile.
  for (Ring& r : rings_) {
    if (!r.on || !r.allocated) continue;
    const int before = r.t.shown_index;
    step(r.t);
    if (r.t.shown_index != before) ++stats_.frames;
  }
  for (Cell& c : cells_) {
    if (cell_live(c)) step(c.t);
  }
  for (Room& room : rooms_) {
    if (!room.on || !room.allocated || room.cells.empty()) continue;
    const Tile& first = cells_[room.cells.front()].t;
    if (first.shown.count == 0 || lv_obj_is_hidden(first.img)) continue;
    const auto s = static_cast<int>(((now_ms + room.phase_ms) % kCycleMs) / first.shown.frame_ms);
    if (s != room.shown_step) ++stats_.frames;
    room.shown_step = s;
  }
}

const std::vector<PulseLayer::Due>& PulseLayer::take_due() {
  due_out_.clear();
  std::swap(due_out_, due_);
  return due_out_;
}

void PulseLayer::on_timer(lv_timer_t* t) {
  auto* self = static_cast<PulseLayer*>(lv_timer_get_user_data(t));
  self->tick(lv_tick_get());
  bool any = false;
  for (const Ring& r : self->rings_) any = any || r.on;
  for (const Room& r : self->rooms_) any = any || r.on;
  if (!any) {
    lv_timer_delete(self->timer_);
    self->timer_ = nullptr;
  }
}

void PulseLayer::ensure_timer() {
  if (!alive_ || timer_ != nullptr) return;
  bool any = false;
  for (const Ring& r : rings_) any = any || r.on;
  for (const Room& r : rooms_) any = any || r.on;
  if (any) timer_ = lv_timer_create(on_timer, kTickMs, this);
}

PulseStats PulseLayer::stats() const {
  PulseStats s = stats_;
  s.tile_bytes = bytes_in_use();
  s.rings_shown = 0;
  s.rooms_animated = 0;
  s.frame_divisor = divisor_;
  s.pending = job_.tile != nullptr;
  for (const Ring& r : rings_) {
    if (!r.allocated) continue;
    s.rings_shown += (r.on && r.t.shown.count > 0) ? 1U : 0U;
    s.pending = s.pending || r.t.dirty;
  }
  for (const Cell& c : cells_) s.pending = s.pending || (cell_live(c) && c.t.dirty);
  for (const Room& room : rooms_) {
    s.rooms_animated += (room.on && room.allocated && room_ready(room)) ? 1U : 0U;
  }
  return s;
}

}  // namespace hearthmap::ui::detail
