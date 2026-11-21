#include "pulse_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace hearthmap::ui::detail {

namespace {

constexpr std::int32_t kHaloWidth = PulseLayer::kRingWidth + theme::kPulseHaloExtra;
constexpr std::int32_t kRingReach = PulseLayer::kRingTo + kHaloWidth / 2 + 2;
constexpr std::size_t kRingLutSize =
    static_cast<std::size_t>(PulseLayer::kRingTo + kHaloWidth + 2) * 16U;
constexpr std::int32_t kRoomHaloWidth = PulseLayer::kRoomWidth + theme::kPulseHaloExtra;

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

std::size_t edge_pieces(std::int32_t dx, std::int32_t dy) {
  dx = std::abs(dx);
  dy = std::abs(dy);
  if (dx == 0 || dy == 0) return 1;
  const std::int32_t longer = std::max(dx, dy);
  return static_cast<std::size_t>((longer + PulseLayer::kEdgePieceMax - 1) /
                                  PulseLayer::kEdgePieceMax);
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
  // Badges and shadows of anti-aliasing: a couple of pixels more.
  a.x1 -= 2;
  a.y1 -= 2;
  a.x2 += 2;
  a.y2 += 2;
  return a;
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
  for (std::size_t f = 0; f < kRingFrames; ++f) {
    const float t = static_cast<float>(f) / static_cast<float>(kRingFrames);
    const float radius =
        static_cast<float>(kRingFrom) + static_cast<float>(kRingTo - kRingFrom) * t;
    const auto opa = static_cast<std::uint8_t>(255.0f * (1.0f - t) + 0.5f);
    ring_lut(radius, static_cast<float>(kRingWidth), opa, kRingLutSize, ring_alpha_[f]);
    ring_lut(radius, static_cast<float>(kHaloWidth), halo_opa(opa), kRingLutSize, ring_halo_[f]);
  }
  for (std::size_t s = 0; s < kRoomSteps; ++s) {
    const float t = static_cast<float>(s) / static_cast<float>(kRoomSteps);
    const float ease = 0.5f - 0.5f * std::cos(6.2831853f * t);
    const float lo = static_cast<float>(theme::kRoomPulseMinOpa);
    room_opa_[s] = static_cast<std::uint8_t>(lo + (255.0f - lo) * ease + 0.5f);
  }

  // Room outlines: one container per room, one tile per edge piece.
  constexpr std::int32_t pad = kRoomHaloWidth;
  rooms_.resize(room_outlines.size());
  for (std::size_t r = 0; r < room_outlines.size(); ++r) {
    const auto& pts = room_outlines[r];
    Room& room = rooms_[r];
    room.phase = static_cast<std::uint32_t>((r * 311U) % kRoomCycleMs);
    std::int32_t x1 = LV_COORD_MAX, y1 = LV_COORD_MAX, x2 = -LV_COORD_MAX, y2 = -LV_COORD_MAX;
    for (const auto& p : pts) {
      x1 = std::min(x1, static_cast<std::int32_t>(p.x));
      y1 = std::min(y1, static_cast<std::int32_t>(p.y));
      x2 = std::max(x2, static_cast<std::int32_t>(p.x));
      y2 = std::max(y2, static_cast<std::int32_t>(p.y));
    }
    if (pts.empty()) x1 = y1 = x2 = y2 = 0;
    lv_obj_t* box = lv_obj_create(map_);
    lv_obj_remove_style_all(box);
    lv_obj_set_clickable(box, false);
    lv_obj_set_scrollable(box, false);
    room.extent = {origin_.x + x1 - pad, origin_.y + y1 - pad, origin_.x + x2 + pad,
                   origin_.y + y2 + pad};
    // The container holds every edge tile whole (they are aligned out, so it is too): LVGL
    // looks for a covering object only inside parents that contain the whole repaint.
    const lv_area_t holder = align_out(room.extent);
    lv_obj_set_pos(box, holder.x1 - origin_.x, holder.y1 - origin_.y);
    lv_obj_set_size(box, lv_area_get_width(&holder), lv_area_get_height(&holder));
    lv_obj_set_hidden(box, true);
    room_box_.push_back(box);
    const lv_point_t box_origin{holder.x1, holder.y1};
    for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
      const auto sx = static_cast<std::int32_t>(pts[k].x);
      const auto sy = static_cast<std::int32_t>(pts[k].y);
      const auto tx = static_cast<std::int32_t>(pts[k + 1].x);
      const auto ty = static_cast<std::int32_t>(pts[k + 1].y);
      const std::size_t n = edge_pieces(tx - sx, ty - sy);
      for (std::size_t j = 0; j < n; ++j) {
        const auto jj = static_cast<std::int32_t>(j);
        const auto nn = static_cast<std::int32_t>(n);
        const std::int32_t ax = sx + (tx - sx) * jj / nn, ay = sy + (ty - sy) * jj / nn;
        const std::int32_t bx = sx + (tx - sx) * (jj + 1) / nn;
        const std::int32_t by = sy + (ty - sy) * (jj + 1) / nn;
        Edge e;
        e.t.area = {origin_.x + std::min(ax, bx) - pad, origin_.y + std::min(ay, by) - pad,
                    origin_.x + std::max(ax, bx) + pad, origin_.y + std::max(ay, by) + pad};
        e.t.area = align_out(e.t.area);
        if (!clip(e.t.area, tile_area_)) continue;
        e.ax = static_cast<float>(origin_.x + sx);
        e.ay = static_cast<float>(origin_.y + sy);
        e.bx = static_cast<float>(origin_.x + tx);
        e.by = static_cast<float>(origin_.y + ty);
        make_tile(e.t, box, box_origin);
        room.edges.push_back(std::move(e));
      }
    }
  }

  // Rings: one tile per pin, the ring's whole reach, clipped to the map.
  rings_.resize(pin_centres.size());
  for (std::size_t i = 0; i < pin_centres.size(); ++i) {
    Ring& ring = rings_[i];
    ring.cx = static_cast<float>(origin_.x + pin_centres[i].x);
    ring.cy = static_cast<float>(origin_.y + pin_centres[i].y);
    ring.phase = static_cast<std::uint32_t>((i * 173U) % kRingCycleMs);
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
  if (timer_ != nullptr) lv_timer_delete(timer_);
}

void PulseLayer::forget_objects() {
  alive_ = false;
  if (timer_ != nullptr) lv_timer_delete(timer_);
  timer_ = nullptr;
  for (Ring& r : rings_) r.t.img = nullptr;
  for (Room& room : rooms_) {
    for (Edge& e : room.edges) e.t.img = nullptr;
  }
  ring_img_.clear();
  room_box_.clear();
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
// lv_image_header_t is a set of bit-fields; a tile is at most the screen's size.
#pragma GCC diagnostic ignored "-Wconversion"
#endif
void PulseLayer::make_tile(Tile& t, lv_obj_t* parent, lv_point_t parent_origin) {
  t.w = lv_area_get_width(&t.area);
  t.h = lv_area_get_height(&t.area);
  t.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  t.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  t.dsc.header.w = static_cast<std::uint32_t>(t.w);
  t.dsc.header.h = static_cast<std::uint32_t>(t.h);
  t.dsc.header.stride = static_cast<std::uint32_t>(t.w) * 2U;
  t.img = lv_image_create(parent);
  lv_obj_remove_style_all(t.img);
  lv_obj_set_clickable(t.img, false);
  lv_obj_set_pos(t.img, t.area.x1 - parent_origin.x, t.area.y1 - parent_origin.y);
  lv_obj_set_size(t.img, t.w, t.h);
  lv_obj_add_event_cb(t.img, opaque_image_cover_cb, LV_EVENT_COVER_CHECK, nullptr);
  lv_obj_set_hidden(t.img, true);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void PulseLayer::allocate(Tile& t) {
  const std::size_t n = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  t.below.assign(n, 0);
  t.full.assign(n, 0);
  t.out.assign(n, 0);
  t.dsc.data = reinterpret_cast<const std::uint8_t*>(t.out.data());
  t.dsc.data_size = static_cast<std::uint32_t>(n * 2U);
  if (alive_ && t.img != nullptr) lv_image_set_src(t.img, &t.dsc);
  t.has_base = false;
  t.dirty = true;
}

std::size_t PulseLayer::tile_bytes(const Tile& t) {
  // Three captures/outputs of RGB565, plus a distance table or coverage mask per tile.
  return static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * (3U * 2U + 2U);
}

std::size_t PulseLayer::bytes_in_use() const {
  std::size_t n = 0;
  for (const Ring& r : rings_) {
    if (!r.allocated) continue;
    n += tile_bytes(r.t);
    for (const RingRef& ref : r.rings) n += ref.distance.capacity() * 2U;
    for (const RoomRef& ref : r.rooms) n += ref.coverage.capacity() + ref.halo.capacity();
  }
  for (const Room& room : rooms_) {
    if (!room.allocated) continue;
    for (const Edge& e : room.edges) {
      n += tile_bytes(e.t);
      for (const RoomRef& ref : e.rooms) n += ref.coverage.capacity() + ref.halo.capacity();
    }
  }
  return n;
}

void PulseLayer::release(Tile& t) {
  if (alive_ && t.img != nullptr) {
    lv_obj_set_hidden(t.img, true);
    lv_image_set_src(t.img, nullptr);
  }
  // Give the memory back (assigning {} would keep the capacity, and the budget no longer counts
  // a released tile).
  std::vector<std::uint16_t>().swap(t.below);
  std::vector<std::uint16_t>().swap(t.full);
  std::vector<std::uint16_t>().swap(t.out);
  t.dsc.data = nullptr;
  t.has_base = false;
  t.dirty = true;
}

void PulseLayer::set_ring(std::size_t pin, bool on) {
  if (pin >= rings_.size() || rings_[pin].on == on) return;
  Ring& r = rings_[pin];
  r.on = on;
  if (on) {
    if (fits(ring_admission_bytes(r))) {
      allocate(r.t);
      r.allocated = true;
      r.frame = -1;
    }
  } else if (r.allocated) {
    release(r.t);
    r.allocated = false;
    r.frame = -1;
    std::vector<RingRef>().swap(r.rings);
    std::vector<RoomRef>().swap(r.rooms);
    admit_waiting();
  }
  for (Ring& x : rings_) x.geometry_dirty = true;
  update_visibility();
  ensure_timer();
}

void PulseLayer::set_room(std::size_t room, bool on, std::uint32_t colour) {
  if (room >= rooms_.size() || !alive_) return;
  Room& r = rooms_[room];
  const std::uint16_t c565 = rgb565(colour);
  if (on) lv_obj_set_style_line_color(room_box_[room], lv_color_hex(colour), 0);
  if (r.on == on && (!on || r.colour == c565)) return;
  const bool was_on = r.on;
  r.on = on;
  r.colour = c565;
  std::size_t need = 0;
  for (const Edge& e : r.edges) need += tile_bytes(e.t);
  if (on && !r.allocated && fits(need)) {
    for (Edge& e : r.edges) allocate(e.t);
    r.allocated = true;
    r.step = -1;
  } else if (!on && r.allocated) {
    for (Edge& e : r.edges) {
      release(e.t);
      std::vector<RoomRef>().swap(e.rooms);
    }
    r.allocated = false;
    r.step = -1;
    admit_waiting();
  } else if (on && was_on) {
    r.step = -1;  // colour change: recompose at the next tick
  }
  update_animated_rooms();
  for (Ring& x : rings_) x.geometry_dirty = true;
  for (Room& x : rooms_) {
    for (Edge& e : x.edges) e.geometry_dirty = true;
  }
  update_visibility();
  ensure_timer();
}

void PulseLayer::admit_waiting() {
  // Memory came free: pulses waiting for it get it, rings first, then rooms, in config order.
  for (Ring& x : rings_) {
    if (x.on && !x.allocated && fits(ring_admission_bytes(x))) {
      allocate(x.t);
      x.allocated = true;
      x.frame = -1;
      for (Ring& y : rings_) y.geometry_dirty = true;
    }
  }
  for (Room& x : rooms_) {
    if (!x.on || x.allocated) continue;
    std::size_t need = 0;
    for (const Edge& e : x.edges) need += tile_bytes(e.t);
    if (!fits(need)) continue;
    for (Edge& e : x.edges) allocate(e.t);
    x.allocated = true;
    x.step = -1;
  }
  update_animated_rooms();
}

std::size_t PulseLayer::ring_admission_bytes(const Ring& r) const {
  // The tile, plus a distance table or coverage mask (2 bytes per pixel at most) for each pulse
  // it overlaps, as rebuild_ring_geometry() will allocate them.
  std::size_t overlaps = 1;  // its own ring
  for (const Ring& o : rings_) {
    if (&o != &r && o.allocated && std::abs(o.cx - r.cx) < 2.0f * kRingReach &&
        std::abs(o.cy - r.cy) < 2.0f * kRingReach)
      ++overlaps;
  }
  for (const Room& room : rooms_) {
    if (room.on && intersects(room.extent, r.t.area)) ++overlaps;
  }
  return tile_bytes(r.t) +
         static_cast<std::size_t>(r.t.w) * static_cast<std::size_t>(r.t.h) * 2U * overlaps;
}

void PulseLayer::update_animated_rooms() {
  std::size_t n = 0;
  for (Room& r : rooms_) {
    const bool animated = r.on && r.allocated && n < budget_.max_animated_rooms;
    if (animated) ++n;
    if (animated != r.animated) r.step = -1;
    r.animated = animated;
  }
}

void PulseLayer::mark_dirty(const lv_area_t& area) {
  for (Ring& r : rings_) {
    if (intersects(r.t.area, area)) r.t.dirty = true;
  }
  for (Room& room : rooms_) {
    for (Edge& e : room.edges) {
      if (intersects(e.t.area, area)) e.t.dirty = true;
    }
  }
}

void PulseLayer::mark_all_dirty(bool hide) {
  for (Ring& r : rings_) {
    r.t.dirty = true;
    r.t.has_base = r.t.has_base && !hide;
  }
  for (Room& room : rooms_) {
    for (Edge& e : room.edges) {
      e.t.dirty = true;
      e.t.has_base = e.t.has_base && !hide;
    }
  }
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
    const bool show = r.on && r.allocated && r.t.has_base && !veiled_;
    if (lv_obj_is_hidden(r.t.img) == show) lv_obj_set_hidden(r.t.img, !show);
  }
  for (std::size_t i = 0; i < rooms_.size(); ++i) {
    Room& room = rooms_[i];
    bool ready = room.on && room.allocated && !veiled_;
    for (const Edge& e : room.edges) ready = ready && e.t.has_base;
    for (Edge& e : room.edges) {
      if (lv_obj_is_hidden(e.t.img) == ready) lv_obj_set_hidden(e.t.img, !ready);
    }
    if (lv_obj_is_hidden(room_box_[i]) == room.on) lv_obj_set_hidden(room_box_[i], !room.on);
  }
}

bool PulseLayer::capture(Tile& t) {
  lv_display_t* disp = lv_obj_get_display(map_);
  lv_display_enable_invalidation(disp, false);  // a counter in LVGL 9.6: paired below
  // Pulses are never part of what lies under a pulse.
  std::vector<lv_obj_t*> hidden;
  for (Ring& r : rings_) {
    if (!lv_obj_is_hidden(r.t.img) && intersects(r.t.area, t.area)) hidden.push_back(r.t.img);
  }
  for (lv_obj_t* box : room_box_) {
    if (!lv_obj_is_hidden(box)) hidden.push_back(box);
  }
  for (lv_obj_t* o : hidden) lv_obj_set_hidden(o, true);
  // Where nothing in the map draws, the screen's background shows: capture that, not black.
  const std::uint16_t bg = rgb565(theme::kBackground);
  bool ok = render_area(map_, t.area, t.full.data(), bg);
  // Then without the pins that reach into the tile: whatever differs between the two captures
  // is drawn over the pulses.
  std::vector<lv_obj_t*> pins;
  for (lv_obj_t* p : pins_) {
    if (!lv_obj_is_hidden(p) && intersects(pin_extent(p), t.area)) pins.push_back(p);
  }
  for (lv_obj_t* p : pins) lv_obj_set_hidden(p, true);
  ok = render_area(map_, t.area, t.below.data(), bg) && ok;
  for (lv_obj_t* p : pins) lv_obj_set_hidden(p, false);
  for (lv_obj_t* o : hidden) lv_obj_set_hidden(o, false);
  lv_display_enable_invalidation(disp, true);
  ++stats_.captures;
  t.has_base = ok;
  t.dirty = false;
  return ok;
}

void PulseLayer::rebuild_ring_geometry(std::size_t i) {
  Ring& r = rings_[i];
  r.rings.clear();
  // Its own ring first, so the cap on layers in compose_ring() never drops it.
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
    distance_table(r.t.area.x1, r.t.area.y1, r.t.w, r.t.h, o.cx, o.cy, ref.distance);
    r.rings.push_back(std::move(ref));
  }
  r.rooms.clear();
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    const Room& room = rooms_[k];
    if (!room.on || !intersects(room.extent, r.t.area)) continue;
    RoomRef ref;
    ref.room = k;
    for (const Edge& e : room.edges) {
      lv_area_t seg{static_cast<std::int32_t>(std::min(e.ax, e.bx)) - kRoomHaloWidth,
                    static_cast<std::int32_t>(std::min(e.ay, e.by)) - kRoomHaloWidth,
                    static_cast<std::int32_t>(std::max(e.ax, e.bx)) + kRoomHaloWidth,
                    static_cast<std::int32_t>(std::max(e.ay, e.by)) + kRoomHaloWidth};
      if (!intersects(seg, r.t.area)) continue;
      add_segment_coverage(r.t.area.x1, r.t.area.y1, r.t.w, r.t.h, e.ax, e.ay, e.bx, e.by,
                           static_cast<float>(kRoomWidth), ref.coverage);
      add_segment_coverage(r.t.area.x1, r.t.area.y1, r.t.w, r.t.h, e.ax, e.ay, e.bx, e.by,
                           static_cast<float>(kRoomHaloWidth), ref.halo);
    }
    if (ref.coverage.empty()) continue;
    // Where the outline actually lies in this tile, so a room step repaints only that.
    std::int32_t x1 = r.t.w, y1 = r.t.h, x2 = -1, y2 = -1;
    for (std::int32_t y = 0; y < r.t.h; ++y) {
      for (std::int32_t x = 0; x < r.t.w; ++x) {
        if (ref.halo[static_cast<std::size_t>(y) * static_cast<std::size_t>(r.t.w) +
                     static_cast<std::size_t>(x)] == 0)
          continue;
        x1 = std::min(x1, x);
        y1 = std::min(y1, y);
        x2 = std::max(x2, x);
        y2 = std::max(y2, y);
      }
    }
    if (x2 < 0) continue;
    ref.box = {r.t.area.x1 + x1, r.t.area.y1 + y1, r.t.area.x1 + x2, r.t.area.y1 + y2};
    r.rooms.push_back(std::move(ref));
  }
  r.geometry_dirty = false;
}

std::size_t PulseLayer::room_overlays(const std::vector<RoomRef>& refs, Overlay* out,
                                      std::size_t cap) const {
  // Per outline a dark keyline, then the stroke in the room's colour (D-053).
  std::size_t n = 0;
  const std::uint16_t dark = rgb565(theme::kBackground);
  for (const RoomRef& ref : refs) {
    if (n + 2 > cap) break;
    const Room& room = rooms_[ref.room];
    const std::uint8_t opa = room.on ? room.opa : std::uint8_t{0};
    out[n++] = Overlay{ref.halo.data(), dark, halo_opa(opa)};
    out[n++] = Overlay{ref.coverage.data(), room.colour, opa};
  }
  return n;
}

void PulseLayer::compose_ring(std::size_t i, const lv_area_t& box_in) {
  Ring& r = rings_[i];
  if (!r.allocated || !r.t.has_base) return;
  lv_area_t box = align_out(box_in);
  if (!clip(box, r.t.area)) return;
  Overlay overlays[16];
  const std::size_t no = room_overlays(r.rooms, overlays, 16);
  RingLayer layers[16];
  std::size_t nr = 0;
  const std::uint16_t dark = rgb565(theme::kBackground);
  const std::uint16_t danger = rgb565(theme::kDanger);
  for (const RingRef& ref : r.rings) {
    if (nr + 2 > 16) break;
    const Ring& o = rings_[ref.ring];
    if (!o.on || o.frame < 0) continue;
    // A dark keyline, then the ring: it reads on a red room as well as on neutral heat.
    const auto f = static_cast<std::size_t>(o.frame);
    layers[nr++] = RingLayer{ref.distance.data(), ring_halo_[f].data(), ring_halo_[f].size(), dark};
    layers[nr++] =
        RingLayer{ref.distance.data(), ring_alpha_[f].data(), ring_alpha_[f].size(), danger};
  }
  ComposeJob job;
  job.below = r.t.below.data();
  job.full = r.t.full.data();
  job.out = r.t.out.data();
  job.stride = r.t.w;
  job.x0 = box.x1 - r.t.area.x1;
  job.y0 = box.y1 - r.t.area.y1;
  job.x1 = box.x2 - r.t.area.x1 + 1;
  job.y1 = box.y2 - r.t.area.y1 + 1;
  job.overlays = overlays;
  job.overlay_count = no;
  job.rings = layers;
  job.ring_count = nr;
  ops_->compose(job);
  if (alive_ && !lv_obj_is_hidden(r.t.img)) lv_obj_invalidate_area(r.t.img, &box);
}

void PulseLayer::rebuild_edge_geometry(Edge& e) {
  e.rooms.clear();
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    const Room& room = rooms_[k];
    if (!room.on || !intersects(room.extent, e.t.area)) continue;
    RoomRef ref;
    ref.room = k;
    for (const Edge& seg : room.edges) {
      const lv_area_t box{static_cast<std::int32_t>(std::min(seg.ax, seg.bx)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::min(seg.ay, seg.by)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(seg.ax, seg.bx)) + kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(seg.ay, seg.by)) + kRoomHaloWidth};
      if (!intersects(box, e.t.area)) continue;
      add_segment_coverage(e.t.area.x1, e.t.area.y1, e.t.w, e.t.h, seg.ax, seg.ay, seg.bx, seg.by,
                           static_cast<float>(kRoomWidth), ref.coverage);
      add_segment_coverage(e.t.area.x1, e.t.area.y1, e.t.w, e.t.h, seg.ax, seg.ay, seg.bx, seg.by,
                           static_cast<float>(kRoomHaloWidth), ref.halo);
    }
    if (!ref.coverage.empty()) e.rooms.push_back(std::move(ref));
  }
  e.geometry_dirty = false;
}

void PulseLayer::compose_edge(Edge& e) {
  if (!e.t.has_base) return;
  // Every outline through this tile, in room order, so overlapping tiles of adjacent rooms
  // agree (a neighbour's stroke may lag its own steps by one of this room's).
  Overlay overlays[16];
  const std::size_t no = room_overlays(e.rooms, overlays, 16);
  ComposeJob job;
  job.below = e.t.below.data();
  job.full = e.t.full.data();
  job.out = e.t.out.data();
  job.stride = e.t.w;
  job.x1 = e.t.w;
  job.y1 = e.t.h;
  job.overlays = overlays;
  job.overlay_count = no;
  ops_->compose(job);
  if (alive_ && !lv_obj_is_hidden(e.t.img)) lv_obj_invalidate(e.t.img);
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
  bool did = false;
  for (std::size_t i = 0; i < rings_.size() && !did; ++i) {
    Ring& r = rings_[i];
    if (!r.allocated || !r.t.dirty) continue;
    capture(r.t);
    rebuild_ring_geometry(i);
    update_visibility();
    compose_ring(i, r.t.area);
    did = true;
  }
  for (std::size_t k = 0; k < rooms_.size() && !did; ++k) {
    Room& room = rooms_[k];
    if (!room.allocated) continue;
    for (Edge& e : room.edges) {
      if (!e.t.dirty) continue;
      capture(e.t);
      rebuild_edge_geometry(e);
      update_visibility();
      compose_edge(e);
      did = true;
      break;
    }
  }
  for (std::size_t i = 0; i < rings_.size() && !did; ++i) {
    Ring& r = rings_[i];
    if (!r.allocated || !r.t.has_base || !r.geometry_dirty) continue;
    rebuild_ring_geometry(i);
    compose_ring(i, r.t.area);
    did = true;
  }
  for (std::size_t k = 0; k < rooms_.size() && !did; ++k) {
    if (!rooms_[k].allocated) continue;
    for (Edge& e : rooms_[k].edges) {
      if (!e.t.has_base || !e.geometry_dirty) continue;
      rebuild_edge_geometry(e);
      compose_edge(e);
      did = true;
      break;
    }
  }
  return stats().pending;
}

lv_area_t PulseLayer::ring_box(const Ring& r, int frame) const {
  if (frame < 0) return lv_area_t{0, 0, -1, -1};
  const float t = static_cast<float>(frame) / static_cast<float>(kRingFrames);
  const auto reach = static_cast<std::int32_t>(static_cast<float>(kRingFrom) +
                                               static_cast<float>(kRingTo - kRingFrom) * t +
                                               static_cast<float>(kHaloWidth) / 2.0f + 2.0f);
  const auto cx = static_cast<std::int32_t>(r.cx);
  const auto cy = static_cast<std::int32_t>(r.cy);
  return lv_area_t{cx - reach, cy - reach, cx + reach, cy + reach};
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
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    Room& room = rooms_[k];
    if (!room.on || !room.allocated) continue;
    bool ready = true;
    for (const Edge& e : room.edges) ready = ready && e.t.has_base;
    if (!ready) continue;
    const int step =
        room.animated
            ? static_cast<int>(((now_ms + room.phase) % kRoomCycleMs) * kRoomSteps / kRoomCycleMs)
            : -2;
    if (step == room.step) continue;
    room.step = step;
    room.opa = step >= 0 ? room_opa_[static_cast<std::size_t>(step)] : std::uint8_t{255};
    for (Edge& e : room.edges) compose_edge(e);
    ++stats_.frames;
    // Ring tiles over this outline catch up at their own next frame (at most one ring frame
    // later), so a room step never costs a separate repaint of a ring tile.
  }
  for (std::size_t i = 0; i < rings_.size(); ++i) {
    Ring& r = rings_[i];
    if (!r.on || !r.allocated || !r.t.has_base) continue;
    const int frame =
        static_cast<int>(((now_ms + r.phase) % kRingCycleMs) * kRingFrames / kRingCycleMs);
    constexpr std::uint32_t kFrameMs = kRingCycleMs / kRingFrames;
    if (frame == r.frame && now_ms - r.frame_ms < kFrameMs) continue;
    const int old = r.frame;
    if (old >= 0) {
      // Frame boundaries passed since the last frame shown (wrap-safe), less the one shown now.
      const std::uint32_t passed = (now_ms - r.frame_ms + kFrameMs / 2U) / kFrameMs;
      if (passed > 1U) stats_.dropped += passed - 1U;
    }
    r.frame_ms = now_ms;
    if (frame == old) continue;  // whole cycles later: the same picture
    r.frame = frame;
    ++stats_.frames;
    lv_area_t box = ring_box(r, frame);
    auto grow = [&box](const lv_area_t& b) {
      if (b.x1 > b.x2) return;
      box.x1 = std::min(box.x1, b.x1);
      box.y1 = std::min(box.y1, b.y1);
      box.x2 = std::max(box.x2, b.x2);
      box.y2 = std::max(box.y2, b.y2);
    };
    if (old >= 0) grow(ring_box(r, old));
    // Outlines under this tile that stepped since it was last composed.
    for (RoomRef& ref : r.rooms) {
      if (ref.drawn_step != rooms_[ref.room].step) {
        grow(ref.box);
        ref.drawn_step = rooms_[ref.room].step;
      }
    }
    // Every tile this ring reaches into, its own first.
    for (std::size_t k = 0; k < rings_.size(); ++k) {
      for (const RingRef& ref : rings_[k].rings) {
        if (ref.ring == i) {
          compose_ring(k, box);
          break;
        }
      }
    }
  }
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
  s.tile_bytes = 0;
  s.rings_shown = 0;
  s.rooms_animated = 0;
  s.pending = false;
  auto bytes = [](const Tile& t) {
    return (t.below.capacity() + t.full.capacity() + t.out.capacity()) * sizeof(std::uint16_t);
  };
  for (const Ring& r : rings_) {
    s.tile_bytes += bytes(r.t);
    for (const RingRef& ref : r.rings) s.tile_bytes += ref.distance.capacity() * 2U;
    for (const RoomRef& ref : r.rooms)
      s.tile_bytes += ref.coverage.capacity() + ref.halo.capacity();
    if (r.allocated) {
      s.rings_shown += r.on ? 1U : 0U;
      // Only work prepare_step() can do: geometry needs a captured base (a failed capture waits
      // for the next change under the tile instead of stalling the embedder's slices).
      s.pending = s.pending || r.t.dirty || (r.geometry_dirty && r.t.has_base);
    }
  }
  for (const Room& room : rooms_) {
    for (const Edge& e : room.edges) {
      s.tile_bytes += bytes(e.t);
      for (const RoomRef& ref : e.rooms)
        s.tile_bytes += ref.coverage.capacity() + ref.halo.capacity();
      if (room.allocated) s.pending = s.pending || e.t.dirty || (e.geometry_dirty && e.t.has_base);
    }
    s.rooms_animated += room.animated ? 1U : 0U;
  }
  return s;
}

}  // namespace hearthmap::ui::detail
