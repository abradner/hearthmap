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
constexpr std::uint32_t kMaxDivisor = PulseLayer::kMaxDivisor;
static_assert(PulseLayer::kCycleMs % (PulseLayer::kRingFrameMs * kMaxDivisor) == 0,
              "every ring frame rate must divide the cycle");
static_assert(PulseLayer::kCycleMs % (PulseLayer::kRoomStepMs * kMaxDivisor) == 0,
              "every outline step rate must divide the cycle");
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
  worker_ = budget.worker != nullptr ? budget.worker : &inline_worker_;
  job_.layer = this;
  // Every opacity table now: the worker reads them, so none is filled lazily (D-069).
  for (std::uint32_t opa = 0; opa < 256; ++opa) {
    for (std::uint32_t c = 0; c < 256; ++c) {
      opa_luts_[opa][c] = static_cast<std::uint8_t>((c * opa + 127U) / 255U);
    }
  }
  // Ring frames: radius 26 -> 52 px, opacity 1 -> 0, linearly over the cycle (the UI spec's
  // ripple); room steps: opacity 0.35 -> 1 -> 0.35 on a cosine ease.
  ring_alpha_.resize(kRingFrames);
  ring_halo_.resize(kRingFrames);
  for (std::uint32_t f = 0; f < kRingFrames; ++f) {
    const float t = static_cast<float>(f) / static_cast<float>(kRingFrames);
    const float radius =
        static_cast<float>(kRingFrom) + static_cast<float>(kRingTo - kRingFrom) * t;
    const auto opa = static_cast<std::uint8_t>(255.0f * (1.0f - t) + 0.5f);
    // A table the port refuses stays empty, and compose() skips a layer without one.
    if (!ring_lut(radius, static_cast<float>(kRingWidth), opa, kRingLutSize, ring_alpha_[f]))
      ++stats_.alloc_failures;
    if (!ring_lut(radius, static_cast<float>(kHaloWidth), halo_opa(opa), kRingLutSize,
                  ring_halo_[f]))
      ++stats_.alloc_failures;
  }
  static_assert(std::tuple_size<decltype(room_opa_)>::value == kRoomSteps, "a step per entry");
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
  using CellKey = std::pair<std::int32_t, std::int32_t>;
  std::map<CellKey, std::size_t, std::less<CellKey>,
           BulkAllocator<std::pair<const CellKey, std::size_t>>>
      cell_at;
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
    ring_img_.push_back(ring.t.pc[0].img);
  }
  // Extra pieces (D-070) for the tiles a later tile can overlap: a ring under a later ring's
  // tile, a cell under any ring's. Made now, so every object keeps its place in the tree.
  for (std::size_t i = 0; i < rings_.size(); ++i) {
    for (std::size_t j = i + 1; j < rings_.size(); ++j) {
      if (intersects(rings_[i].t.area, rings_[j].t.area)) {
        make_piece_slots(rings_[i].t, map_);
        break;
      }
    }
  }
  for (Cell& c : cells_) {
    for (const Ring& r : rings_) {
      if (intersects(c.rect, r.t.area)) {
        make_piece_slots(c.t, outline_box_);
        break;
      }
    }
  }
  // Each tile's lists of what reaches it, sized now to everything that ever could (pins, rooms
  // and ring tiles never move; a cell's rooms are fixed above): a vector's growth cannot fail
  // softly (memory.h), so preparing a tile must never grow one (D-064).
  for (Ring& r : rings_) {
    std::size_t n_rings = 0;
    for (const Ring& o : rings_) {
      const lv_area_t reach{static_cast<std::int32_t>(o.cx) - kRingReach,
                            static_cast<std::int32_t>(o.cy) - kRingReach,
                            static_cast<std::int32_t>(o.cx) + kRingReach,
                            static_cast<std::int32_t>(o.cy) + kRingReach};
      if (intersects(reach, r.t.area)) ++n_rings;
    }
    std::size_t n_rooms = 0;
    for (const Room& room : rooms_) n_rooms += intersects(room.extent, r.t.area) ? 1U : 0U;
    r.rings.reserve(n_rings);
    r.rooms.reserve(n_rooms);
  }
  for (Cell& c : cells_) c.refs.reserve(c.rooms.size());
  // Sized once, so the frame clock and captures never grow them: a due frame per tile at most
  // (requeue() can add one more), and everything capture() may hide.
  due_.reserve(2 * kMaxPieces * (rings_.size() + cells_.size()));
  due_out_.reserve(due_.capacity());
  capture_hidden_.reserve(rings_.size() * kMaxPieces + pins_.size() + 8);
  // Staging for captures, the largest tile's size (a cell only shrinks from its first area).
  std::size_t largest = 0;
  for (const Ring& r : rings_) {
    largest = std::max(largest, static_cast<std::size_t>(r.t.w) * static_cast<std::size_t>(r.t.h));
  }
  for (const Cell& c : cells_) {
    largest = std::max(largest, static_cast<std::size_t>(c.t.w) * static_cast<std::size_t>(c.t.h));
  }
  staging_px_ = largest;
  if (!ensure_staging()) ++stats_.alloc_failures;  // asked again at the first capture
}

bool PulseLayer::ensure_staging() {
  if (staged_full_.size() == staging_px_ && staged_below_.size() == staging_px_) return true;
  if (staged_full_.resize(staging_px_) && staged_below_.resize(staging_px_)) return true;
  staged_full_.release();  // all or nothing: a capture needs both
  staged_below_.release();
  return false;
}

PulseLayer::~PulseLayer() {
  if (timer_ != nullptr && alive_) lv_timer_delete(timer_);
  // A job still out holds this layer's buffers and reads its tables: it comes back (at most one
  // step later) before anything is freed. Only at destruction, never in the running loop.
  if (job_.out) {
    worker_->cancel(WorkLane::Sprites);
    while (worker_->poll(WorkLane::Sprites) == Worker::Poll::Busy) {
    }
  }
}

void PulseLayer::forget_objects() {
  alive_ = false;
  if (timer_ != nullptr) lv_timer_delete(timer_);
  timer_ = nullptr;
  for (Ring& r : rings_) {
    for (Piece& p : r.t.pc) p.img = nullptr;
  }
  for (Cell& c : cells_) {
    for (Piece& p : c.t.pc) p.img = nullptr;
  }
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
namespace {
lv_obj_t* make_tile_image(lv_obj_t* parent) {
  lv_obj_t* img = lv_image_create(parent);
  lv_obj_remove_style_all(img);
  lv_obj_set_clickable(img, false);
  lv_obj_add_event_cb(img, opaque_image_cover_cb, LV_EVENT_COVER_CHECK, nullptr);
  lv_obj_set_hidden(img, true);
  return img;
}
}  // namespace

void PulseLayer::make_tile(Tile& t, lv_obj_t* parent, lv_point_t parent_origin) {
  t.pc[0].img = make_tile_image(parent);
  t.piece_slots = 1;
  size_tile(t, parent_origin);
}

void PulseLayer::make_piece_slots(Tile& t, lv_obj_t* parent) {
  for (std::size_t k = t.piece_slots; k < kMaxPieces; ++k) t.pc[k].img = make_tile_image(parent);
  t.piece_slots = kMaxPieces;
}

void PulseLayer::size_tile(Tile& t, lv_point_t parent_origin) {
  t.w = lv_area_get_width(&t.area);
  t.h = lv_area_get_height(&t.area);
  t.origin = parent_origin;
  t.frame = nullptr;
  for (Piece& p : t.pc) {
    p.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    p.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  }
  layout_pieces(t, &t.area, 1);  // one piece, the whole tile, until cut
}

void PulseLayer::layout_pieces(Tile& t, const lv_area_t* rects, std::size_t n) {
  if (n > t.piece_slots) {
    n = 1;  // cannot cut that finely: the whole tile (it may then be covered)
    rects = &t.area;
  }
  t.pieces = n;
  const std::uint32_t stride = static_cast<std::uint32_t>(t.w) * 2U;
  for (std::size_t k = 0; k < kMaxPieces; ++k) {
    Piece& p = t.pc[k];
    p.on = k < n;
    if (!p.on) {
      if (alive_ && p.img != nullptr && !lv_obj_is_hidden(p.img)) lv_obj_set_hidden(p.img, true);
      continue;
    }
    const lv_area_t& r = rects[k];
    const std::int32_t w = lv_area_get_width(&r), h = lv_area_get_height(&r);
    p.ox = r.x1 - t.area.x1;
    p.oy = r.y1 - t.area.y1;
    p.dsc.header.w = static_cast<std::uint32_t>(w);
    p.dsc.header.h = static_cast<std::uint32_t>(h);
    p.dsc.header.stride = stride;
    // LVGL wants data_size >= stride * h (lv_draw_buf_init): the last row's stride runs past the
    // piece's right edge, a length LVGL only checks and never reads (a draw reads w pixels a row).
    p.dsc.data_size = stride * static_cast<std::uint32_t>(h);
    p.dsc.data =
        t.frame == nullptr
            ? nullptr
            : reinterpret_cast<const std::uint8_t*>(
                  t.frame + static_cast<std::size_t>(p.oy) * static_cast<std::size_t>(t.w) +
                  static_cast<std::size_t>(p.ox));
    if (!alive_ || p.img == nullptr) continue;
    lv_obj_set_pos(p.img, r.x1 - t.origin.x, r.y1 - t.origin.y);
    lv_obj_set_size(p.img, w, h);
    // An lv_image reads its source's size when the source is set, not when it draws: given the
    // same descriptor with a new size it would draw the old size, centred. Set it again.
    if (lv_image_get_src(p.img) == &p.dsc) {
      lv_image_set_src(p.img, nullptr);
      lv_image_set_src(p.img, &p.dsc);
    }
  }
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void PulseLayer::point(Tile& t, const std::uint16_t* frame) {
  t.frame = frame;
  for (std::size_t k = 0; k < t.pieces; ++k) {
    Piece& p = t.pc[k];
    p.dsc.data = frame == nullptr
                     ? nullptr
                     : reinterpret_cast<const std::uint8_t*>(
                           frame + static_cast<std::size_t>(p.oy) * static_cast<std::size_t>(t.w) +
                           static_cast<std::size_t>(p.ox));
  }
}

void PulseLayer::set_src(Tile& t) {
  if (!alive_) return;
  for (std::size_t k = 0; k < t.pieces; ++k) {
    Piece& p = t.pc[k];
    if (p.img != nullptr && lv_image_get_src(p.img) != &p.dsc) lv_image_set_src(p.img, &p.dsc);
  }
}

void PulseLayer::invalidate_tile(Tile& t) {
  if (!alive_) return;
  for (std::size_t k = 0; k < t.pieces; ++k) {
    if (t.pc[k].img != nullptr && !lv_obj_is_hidden(t.pc[k].img)) lv_obj_invalidate(t.pc[k].img);
  }
}

bool PulseLayer::drop_due(Tile& t) {
  bool any = false;
  for (std::size_t i = 0; i < due_.size();) {
    bool mine = false;
    for (const Piece& p : t.pc) mine = mine || due_[i].dsc == &p.dsc;
    if (mine) {
      due_.erase(due_.begin() + static_cast<std::ptrdiff_t>(i));
      any = true;
    } else {
      ++i;
    }
  }
  return any;
}

void PulseLayer::release(Tile& t) {
  drop_job(&t);
  if (alive_) {
    for (Piece& p : t.pc) {
      if (p.img == nullptr) continue;
      lv_obj_set_hidden(p.img, true);
      lv_image_set_src(p.img, nullptr);
    }
  }
  t.below.release();
  t.full.release();
  t.shown.px.release();
  t.shown.count = 0;
  t.shown.complete = false;
  point(t, nullptr);
  t.captures_match = false;
  t.vis = false;
  t.shown_index = -1;
  t.dirty = true;
  t.hidden = false;
  (void)drop_due(t);
}

std::size_t PulseLayer::tile_frame_bytes(const Tile& t, bool ring) const {
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  const std::uint32_t frames = (ring ? kRingFrames : kRoomSteps) / divisor_;
  return px * 2U * frames;
}

std::size_t PulseLayer::bytes_in_use(bool with_rooms) const {
  // What the sprites hold now: captures, tables, frame sets, and the set being composed.
  auto held = [](const Tile& t) { return t.below.bytes() + t.full.bytes() + t.shown.px.bytes(); };
  std::size_t n = job_.next.px.bytes() + job_.below.bytes() + job_.full.bytes() +
                  staged_full_.bytes() + staged_below_.bytes();
  for (const RingRef& ref : job_.rings) n += ref.distance.bytes();
  for (const RoomRef& ref : job_.rooms) n += ref.coverage.bytes() + ref.halo.bytes();
  for (const Ring& r : rings_) {
    if (!r.allocated) continue;
    n += held(r.t);
    for (const RingRef& ref : r.rings) n += ref.distance.bytes();
    for (const RoomRef& ref : r.rooms) n += ref.coverage.bytes() + ref.halo.bytes();
  }
  for (const Cell& c : cells_) {
    if (!with_rooms || (!cell_live(c) && c.t.shown.px.empty())) continue;
    n += held(c.t);
    for (const RoomRef& ref : c.refs) n += ref.coverage.bytes() + ref.halo.bytes();
  }
  return n;
}

// What every allocated pulse needs once all its frames exist at `div`, at the peak: captures,
// a table per pulse reaching the tile (2 bytes a pixel each: a ring's distance table, or a room's
// coverage and keyline), the frames, and room for one replacement set (a set is recomposed while
// the old one plays). An upper bound of bytes_in_use() whenever every set is at `div` or coarser.
std::size_t PulseLayer::need_at(std::uint32_t div, bool with_rooms) const {
  // The two capture staging buffers, held whatever pulses (D-071).
  std::size_t n = staging_px_ * 2U * sizeof(std::uint16_t), largest = 0;
  auto add = [&](const Tile& t, bool ring, std::size_t tables) {
    const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
    // The frames, their guard words and the row of padding after them (FrameSet::pad).
    const std::size_t frames = px * 2U * ((ring ? kRingFrames : kRoomSteps) / div) +
                               (2U * kGuardWords + static_cast<std::size_t>(t.w)) * 2U;
    n += px * 4U + px * 2U * tables + frames;
    largest = std::max(largest, frames);
  };
  for (const Ring& r : rings_) {
    if (!r.allocated) continue;
    std::size_t tables = 0;
    for (const Ring& o : rings_) {
      const lv_area_t reach{static_cast<std::int32_t>(o.cx) - kRingReach,
                            static_cast<std::int32_t>(o.cy) - kRingReach,
                            static_cast<std::int32_t>(o.cx) + kRingReach,
                            static_cast<std::int32_t>(o.cy) + kRingReach};
      if (o.on && o.allocated && intersects(reach, r.t.area)) ++tables;
    }
    for (const Room& room : rooms_) {
      if (room.on && room.allocated && intersects(room.extent, r.t.area)) ++tables;
    }
    add(r.t, true, tables);
  }
  if (with_rooms) {
    for (const Cell& c : cells_) {
      if (!cell_live(c)) continue;
      std::size_t tables = 0;
      for (std::size_t k : c.rooms) tables += (rooms_[k].on && rooms_[k].allocated) ? 1U : 0U;
      add(c.t, false, tables);
    }
  }
  return n + largest;
}

bool PulseLayer::fits_at_coarsest(bool with_rooms) const {
  return need_at(kMaxDivisor, with_rooms) <= budget_.memory_bytes;
}

// The smallest divisor of frames per cycle at which every allocated pulse fits the budget at its
// peak (need_at). Finer than before: the old sets (smaller) play while each tile is recomposed.
// Coarser: the old sets are larger than the new, so they are given up first and the tiles step
// aside until recomposed; keeping them would pass the budget.
void PulseLayer::choose_divisor() {
  std::uint32_t div = budget_.min_divisor;
  if (div < 1U || div > kMaxDivisor || (div & (div - 1U)) != 0U) div = 1;
  while (div < kMaxDivisor && need_at(div, true) > budget_.memory_bytes) div *= 2;
  if (div == divisor_) return;
  const bool coarser = div > divisor_;
  // Paused, nothing can be rebuilt: coarsening now would only take every playing set away. The
  // pulse that asked for it gets nothing meanwhile (nothing is built), so memory does not grow;
  // set_paused(false) chooses again.
  if (coarser && paused_) return;
  divisor_ = div;
  if (coarser) {
    for (Ring& r : rings_) {
      if (r.allocated) release(r.t);
    }
    for (Cell& c : cells_) {
      if (cell_live(c)) release(c.t);
    }
  }
  mark_all_dirty(false);
}

void PulseLayer::set_ring(std::size_t pin, bool on) {
  if (pin >= rings_.size() || rings_[pin].on == on) return;
  Ring& r = rings_[pin];
  r.on = on;
  if (on) {
    // Admitted if everything still fits at the coarsest rate with it. Rings come first (D-053):
    // outlines give their memory up, the last room first, and wait; only when giving them all
    // up would make room, never for nothing.
    r.allocated = true;
    if (!fits_at_coarsest(true) && fits_at_coarsest(false)) {
      for (std::size_t k = rooms_.size(); k-- > 0 && !fits_at_coarsest(true);) {
        if (rooms_[k].allocated) drop_room(k);
      }
    }
    if (fits_at_coarsest(true)) {
      r.t.dirty = true;
      fit_cells_under(r.t.area);  // its tile draws the strokes under it now
    } else {
      r.allocated = false;
    }
  } else if (r.allocated) {
    release(r.t);
    r.allocated = false;
    // Their tables go back; the lists keep the capacity they were given at construction.
    r.rings.clear();
    r.rooms.clear();
    fit_cells_under(r.t.area);  // the cells it covered draw their strokes again
    admit_waiting();
  }
  mark_ring_neighbours(r);
  choose_divisor();
  update_visibility();
  ensure_timer();
}

bool PulseLayer::try_admit_room(std::size_t room) {
  Room& r = rooms_[room];
  r.allocated = true;
  fit_cells(r);
  if (fits_at_coarsest(true)) return true;
  drop_room(room);  // frees its cells and refits the shared ones
  return false;
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
    (void)try_admit_room(room);
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

void PulseLayer::mark_ring_neighbours(const Ring& r) {
  // Neighbours' tiles show this ring (or stop showing it): recompose those it reaches.
  const lv_area_t reach{
      static_cast<std::int32_t>(r.cx) - kRingReach, static_cast<std::int32_t>(r.cy) - kRingReach,
      static_cast<std::int32_t>(r.cx) + kRingReach, static_cast<std::int32_t>(r.cy) + kRingReach};
  for (Ring& x : rings_) {
    if (&x != &r && x.allocated && intersects(x.t.area, reach)) x.t.dirty = true;
  }
  cancel_stale_job();
}

void PulseLayer::admit_waiting() {
  // Memory came free: pulses waiting for it get it, rings first, then rooms, in config order.
  for (Ring& x : rings_) {
    if (!x.on || x.allocated) continue;
    x.allocated = true;
    if (fits_at_coarsest(true)) {
      x.t.dirty = true;
      fit_cells_under(x.t.area);
      mark_ring_neighbours(x);  // as set_ring() does: their opaque frames must show it
    } else {
      x.allocated = false;
    }
  }
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    Room& x = rooms_[k];
    if (!x.on || x.allocated || !try_admit_room(k)) continue;
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
  if (job_.tile != nullptr && job_.tile->dirty) drop_job(job_.tile);
}

void PulseLayer::reset_job() {
  job_.tile = nullptr;
  job_.stage = Job::Stage::CaptureFull;
  job_.out = false;
  job_.next.px.release();
  job_.next.pad = 0;
  job_.next.count = 0;
  job_.next.complete = false;
  job_.table = 0;
  job_.filled = false;
  job_.frame = 0;
}

void PulseLayer::drop_job(Tile* t) {
  if (job_.tile == nullptr || (t != nullptr && job_.tile != t)) return;
  if (job_.out) {
    // The worker holds it: it is stopped between two steps and freed when polled back, and the
    // bumped generation stops it being adopted if it finished first (D-069).
    ++job_.tile->gen;
    worker_->cancel(WorkLane::Sprites);
    // Inline, the cancellation runs in this poll, so the set goes now, as it always did; a
    // threaded worker gives it back within a step, to a later prepare_step().
    bool completed = false;
    if (worker_->poll(WorkLane::Sprites, &completed) == Worker::Poll::Finished) {
      collect_job(completed);
    }
    return;
  }
  reset_job();  // still at the main loop's captures: nothing is lent
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

namespace {

// `r` less `b`: up to four rectangles (a band above, one below, and the parts left and right of
// `b` between them). Aligned inputs give aligned pieces.
std::size_t subtract(const lv_area_t& r, const lv_area_t& b, lv_area_t* out) {
  if (!intersects(r, b)) {
    out[0] = r;
    return 1;
  }
  const lv_area_t i{std::max(r.x1, b.x1), std::max(r.y1, b.y1), std::min(r.x2, b.x2),
                    std::min(r.y2, b.y2)};
  std::size_t n = 0;
  if (r.y1 < i.y1) out[n++] = lv_area_t{r.x1, r.y1, r.x2, i.y1 - 1};
  if (i.y2 < r.y2) out[n++] = lv_area_t{r.x1, i.y2 + 1, r.x2, r.y2};
  if (r.x1 < i.x1) out[n++] = lv_area_t{r.x1, i.y1, i.x1 - 1, i.y2};
  if (i.x2 < r.x2) out[n++] = lv_area_t{i.x2 + 1, i.y1, r.x2, i.y2};
  return n;
}

}  // namespace

void PulseLayer::cut_tiles() {
  constexpr std::size_t kMaxRects = 16;
  auto cut = [this](Tile& t, auto&& each_later) {
    if (!t.vis) return;
    lv_area_t cur[kMaxRects], next[kMaxRects], part[4];
    std::size_t n = 1;
    cur[0] = t.area;
    bool overflow = false;
    each_later([&](const lv_area_t& b) {
      if (overflow || !intersects(t.area, b)) return;
      std::size_t m = 0;
      for (std::size_t k = 0; k < n && !overflow; ++k) {
        const std::size_t parts = subtract(cur[k], b, part);
        if (m + parts > kMaxRects) {
          overflow = true;
          break;
        }
        for (std::size_t q = 0; q < parts; ++q) next[m++] = part[q];
      }
      if (overflow) return;
      std::copy(next, next + m, cur);
      n = m;
    });
    if (overflow) n = kMaxPieces + 1;  // layout_pieces() keeps it whole
    // Unchanged: nothing to move.
    bool same = n == t.pieces;
    for (std::size_t k = 0; same && k < n; ++k) {
      const Piece& p = t.pc[k];
      same = p.ox == cur[k].x1 - t.area.x1 && p.oy == cur[k].y1 - t.area.y1 &&
             static_cast<std::int32_t>(p.dsc.header.w) == lv_area_get_width(&cur[k]) &&
             static_cast<std::int32_t>(p.dsc.header.h) == lv_area_get_height(&cur[k]);
    }
    if (same) return;
    layout_pieces(t, cur, n);
    if (t.frame != nullptr) set_src(t);
    // Listed frames pointed at the old pieces; moving the pieces repaints them anyway.
    (void)drop_due(t);
  };
  for (std::size_t i = 0; i < rings_.size(); ++i) {
    cut(rings_[i].t, [this, i](auto&& f) {
      for (std::size_t j = i + 1; j < rings_.size(); ++j) {
        if (rings_[j].t.vis) f(rings_[j].t.area);
      }
    });
  }
  for (Cell& c : cells_) {
    cut(c.t, [this](auto&& f) {
      for (const Ring& r : rings_) {
        if (r.t.vis) f(r.t.area);
      }
    });
  }
}

void PulseLayer::update_visibility() {
  if (!alive_) return;
  for (Ring& r : rings_) {
    r.t.vis = r.on && r.allocated && r.t.shown.count > 0 && !r.t.hidden && !veiled_;
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
    c.t.vis = show && any_ready;
  }
  // Each shown tile through the pieces of it no later shown tile overlaps (D-070), then every
  // piece shown or hidden with its tile.
  cut_tiles();
  auto apply = [](Tile& t) {
    for (std::size_t k = 0; k < t.pieces; ++k) {
      lv_obj_t* img = t.pc[k].img;
      if (img != nullptr && lv_obj_is_hidden(img) == t.vis) lv_obj_set_hidden(img, !t.vis);
    }
  };
  for (Ring& r : rings_) apply(r.t);
  for (Cell& c : cells_) apply(c.t);
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
  // Refused at construction (or since): asked again here, on the main loop; refused again, the
  // tile waits for the next change under it (D-064).
  if (!ensure_staging() || staging_px_ < px) {
    starve(t);
    return false;
  }
  BulkBuffer<std::uint16_t>& out = with_pins ? staged_full_ : staged_below_;
  lv_display_t* disp = lv_obj_get_display(map_);
  lv_display_enable_invalidation(disp, false);  // a counter in LVGL 9.6: paired below
  // Pulses are never part of what lies under a pulse.
  BulkVector<lv_obj_t*>& hidden = capture_hidden_;
  hidden.clear();
  for (Ring& r : rings_) {
    if (!intersects(r.t.area, t.area)) continue;
    for (Piece& p : r.t.pc) {
      if (p.img != nullptr && !lv_obj_is_hidden(p.img)) hidden.push_back(p.img);
    }
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

bool PulseLayer::plan_ring(std::size_t i) {
  const Ring& r = rings_[i];
  const std::size_t px = static_cast<std::size_t>(r.t.w) * static_cast<std::size_t>(r.t.h);
  job_.rings.clear();
  // Its own ring first, so a cap on layers can only drop another ring's overlap.
  for (std::size_t n = 0; n < rings_.size(); ++n) {
    const std::size_t j = n == 0 ? i : (n <= i ? n - 1 : n);
    const Ring& o = rings_[j];
    if (!o.on || !o.allocated) continue;
    const lv_area_t reach{
        static_cast<std::int32_t>(o.cx) - kRingReach, static_cast<std::int32_t>(o.cy) - kRingReach,
        static_cast<std::int32_t>(o.cx) + kRingReach, static_cast<std::int32_t>(o.cy) + kRingReach};
    if (!intersects(reach, r.t.area)) continue;
    RingRef ref;
    ref.ring = j;
    ref.box = local_box(reach, r.t.area);
    if (!ref.distance.resize(px)) return false;
    job_.rings.push_back(std::move(ref));
  }
  job_.rooms.clear();
  for (std::size_t k = 0; k < rooms_.size(); ++k) {
    const Room& room = rooms_[k];
    if (!room.on || !room.allocated || !intersects(room.extent, r.t.area)) continue;
    RoomRef ref;
    ref.room = k;
    ref.colour = room.colour;
    bool any = false;
    for (const Segment& e : room.segments) {
      lv_area_t b{static_cast<std::int32_t>(std::min(e.ax, e.bx)) - kRoomHaloWidth,
                  static_cast<std::int32_t>(std::min(e.ay, e.by)) - kRoomHaloWidth,
                  static_cast<std::int32_t>(std::max(e.ax, e.bx)) + kRoomHaloWidth,
                  static_cast<std::int32_t>(std::max(e.ay, e.by)) + kRoomHaloWidth};
      if (!intersects(b, r.t.area)) continue;
      any = true;
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
    if (!any) continue;
    if (!ref.coverage.resize(px) || !ref.halo.resize(px)) return false;
    job_.rooms.push_back(std::move(ref));
  }
  return true;
}

bool PulseLayer::plan_cell(const Cell& c) {
  const std::size_t px = static_cast<std::size_t>(c.t.w) * static_cast<std::size_t>(c.t.h);
  job_.rings.clear();
  job_.rooms.clear();
  // Only rooms listed for the cell can reach it: their strokes are what made it a cell.
  for (std::size_t k : c.rooms) {
    const Room& room = rooms_[k];
    if (!room.on || !room.allocated) continue;
    bool any = false;
    for (const Segment& seg : room.segments) {
      const lv_area_t box{static_cast<std::int32_t>(std::min(seg.ax, seg.bx)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::min(seg.ay, seg.by)) - kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(seg.ax, seg.bx)) + kRoomHaloWidth,
                          static_cast<std::int32_t>(std::max(seg.ay, seg.by)) + kRoomHaloWidth};
      any = any || intersects(box, c.t.area);
    }
    if (!any) continue;
    RoomRef ref;
    ref.room = k;
    ref.colour = room.colour;
    ref.box = TileBox{0, 0, c.t.w, c.t.h};
    if (!ref.coverage.resize(px) || !ref.halo.resize(px)) return false;
    job_.rooms.push_back(std::move(ref));
  }
  return true;
}

// One unit of the tile's tables and frames. Runs on the worker (or inline): it touches only what
// the job owns, plus tables fixed at construction (see Job in pulse_layer.h).
bool PulseLayer::Job::step() {
  const std::size_t px = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  if (!filled) {
    // A pattern no frame holds, so a frame shown before it is composed shows (tests look).
    std::fill_n(next.frames(), next.frame_words(), kUncomposed);
    filled = true;
    return true;
  }
  if (table < rings.size()) {
    RingRef& ref = rings[table++];
    const Ring& o = layer->rings_[ref.ring];
    fill_distance(x0, y0, w, h, o.cx, o.cy, ref.distance.data());
    return true;
  }
  if (table < rings.size() + rooms.size()) {
    RoomRef& ref = rooms[table++ - rings.size()];
    std::fill_n(ref.coverage.data(), px, std::uint8_t{0});
    std::fill_n(ref.halo.data(), px, std::uint8_t{0});
    const lv_area_t area{x0, y0, x0 + w - 1, y0 + h - 1};
    for (const Segment& e : layer->rooms_[ref.room].segments) {
      const lv_area_t b{static_cast<std::int32_t>(std::min(e.ax, e.bx)) - kRoomHaloWidth,
                        static_cast<std::int32_t>(std::min(e.ay, e.by)) - kRoomHaloWidth,
                        static_cast<std::int32_t>(std::max(e.ax, e.bx)) + kRoomHaloWidth,
                        static_cast<std::int32_t>(std::max(e.ay, e.by)) + kRoomHaloWidth};
      if (!intersects(b, area)) continue;
      accumulate_segment(x0, y0, w, h, e.ax, e.ay, e.bx, e.by, static_cast<float>(kRoomWidth),
                         ref.coverage.data());
      accumulate_segment(x0, y0, w, h, e.ax, e.ay, e.bx, e.by, static_cast<float>(kRoomHaloWidth),
                         ref.halo.data());
    }
    return true;
  }
  if (frame >= next.count) return false;
  const ComposeIn in{below.data(), full.data(), w, h, phase_ms, &rings, &rooms};
  layer->compose_frame(in, frame, next.frame_ms,
                       next.frames() + static_cast<std::size_t>(frame) * px);
  return ++frame < next.count;
}

bool PulseLayer::submit_job() {
  Tile& t = *job_.tile;
  Ring* ring = ring_of(&t);
  Cell* cell = ring == nullptr ? cell_of(&t) : nullptr;
  // Lend the captures and the lists (their reserved capacity travels with them, D-064).
  job_.below.swap(t.below);
  job_.full.swap(t.full);
  if (ring != nullptr) {
    job_.rings.swap(ring->rings);
    job_.rooms.swap(ring->rooms);
  } else if (cell != nullptr) {
    job_.rooms.swap(cell->refs);
  }
  bool ok = true;
  if (ring != nullptr) {
    ok = plan_ring(static_cast<std::size_t>(ring - rings_.data()));
  } else if (cell != nullptr) {
    ok = plan_cell(*cell);
  }
  const std::uint32_t base = ring != nullptr ? kRingFrameMs : kRoomStepMs;
  job_.next.frame_ms = base * divisor_;
  job_.next.count = kCycleMs / job_.next.frame_ms;
  job_.next.complete = false;
  const std::size_t words =
      static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h) * job_.next.count;
  job_.next.pad = static_cast<std::size_t>(t.w);  // see FrameSet::pad
  ok = ok && job_.next.px.resize(words + 2 * kGuardWords + job_.next.pad);
  if (!ok) {
    collect_job(false);  // gives everything back; the caller starves the tile
    return false;
  }
  std::fill_n(job_.next.px.begin(), kGuardWords, kCanary);
  std::fill_n(job_.next.frames() + words, kGuardWords, kCanary);
  std::fill_n(job_.next.frames() + words + kGuardWords, job_.next.pad, std::uint16_t{0});
  job_.x0 = t.area.x1;
  job_.y0 = t.area.y1;
  job_.w = t.w;
  job_.h = t.h;
  job_.phase_ms = t.phase_ms;
  job_.gen = t.gen;
  job_.table = 0;
  job_.filled = false;
  job_.frame = 0;
  job_.stage = Job::Stage::Work;
  job_.out = true;
  (void)worker_->submit(WorkLane::Sprites, job_);  // the lane is ours, and idle here
  return true;
}

void PulseLayer::collect_job(bool completed) {
  Tile& t = *job_.tile;
  job_.out = false;
  Ring* ring = ring_of(&t);
  Cell* cell = ring == nullptr ? cell_of(&t) : nullptr;
  t.below.swap(job_.below);
  t.full.swap(job_.full);
  if (ring != nullptr) {
    ring->rings.swap(job_.rings);
    ring->rooms.swap(job_.rooms);
  } else if (cell != nullptr) {
    cell->refs.swap(job_.rooms);
  }
  if (completed && job_.gen == t.gen) {
    ++stats_.sets_built;
    finish_job();
    return;
  }
  if (job_.stage == Job::Stage::Work) ++stats_.sets_discarded;
  t.captures_match = false;  // the captures it took back are newer than the playing set
  // Not wanted any more (released while out): its captures and tables go back too.
  const bool needed =
      ring != nullptr ? (ring->on && ring->allocated) : (cell != nullptr && cell_live(*cell));
  if (!needed) {
    t.below.release();
    t.full.release();
    if (ring != nullptr) {
      ring->rings.clear();
      ring->rooms.clear();
    } else if (cell != nullptr) {
      cell->refs.clear();
    }
  }
  reset_job();
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
  release(c.t);    // frames of the old size are no use
  c.refs.clear();  // tables back, capacity kept
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

void PulseLayer::drop_room(std::size_t room) {
  Room& r = rooms_[room];
  r.allocated = false;
  r.shown_step = -1;
  for (std::size_t i : r.cells) {
    Cell& c = cells_[i];
    if (cell_active(c)) continue;  // another room still pulses through it
    release(c.t);
    c.refs.clear();  // tables back, capacity kept
  }
  fit_cells(r);  // the cells it shared shrink to the rooms left
  mark_dirty(r.extent);
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
void PulseLayer::compose_frame(const ComposeIn& in, std::uint32_t k, std::uint32_t frame_ms,
                               std::uint16_t* out) const {
  const std::uint32_t t_ms = (k * frame_ms + kCycleMs - in.phase_ms % kCycleMs) % kCycleMs;
  const std::uint16_t dark = rgb565(theme::kBackground);
  Overlay overlays[32];
  RingLayer layers[32];
  std::size_t no = 0, nr = 0;
  for (const RoomRef& ref : *in.rooms) {
    if (no + 2 > 32) break;
    const std::uint8_t opa = room_opa_at(ref.room, t_ms);
    overlays[no++] = Overlay{ref.halo.data(), opa_lut(halo_opa(opa)), dark, ref.box};
    overlays[no++] = Overlay{ref.coverage.data(), opa_lut(opa), ref.colour, ref.box};
  }
  const std::uint16_t danger = rgb565(theme::kDanger);
  for (const RingRef& ref : *in.rings) {
    if (nr + 2 > 32) break;
    const auto f = static_cast<std::size_t>(ring_frame_at(ref.ring, t_ms));
    // A dark keyline, then the ring: it reads on a red room as well as on neutral heat.
    layers[nr++] =
        RingLayer{ref.distance.data(), ring_halo_[f].data(), ring_halo_[f].size(), dark, ref.box};
    layers[nr++] = RingLayer{ref.distance.data(), ring_alpha_[f].data(), ring_alpha_[f].size(),
                             danger, ref.box};
  }
  ComposeJob job;
  job.below = in.below;
  job.full = in.full;
  job.out = out;
  job.stride = in.w;
  job.w = in.w;
  job.h = in.h;
  job.overlays = overlays;
  job.overlay_count = no;
  job.rings = layers;
  job.ring_count = nr;
  ops_->compose(job);
}

PulseLayer::ComposeIn PulseLayer::tile_inputs(Tile& t) {
  if (Ring* r = ring_of(&t)) {
    return ComposeIn{t.below.data(), t.full.data(), t.w, t.h, t.phase_ms, &r->rings, &r->rooms};
  }
  Cell* c = cell_of(&t);
  static const BulkVector<RoomRef>* none = nullptr;
  return ComposeIn{t.below.data(),
                   t.full.data(),
                   t.w,
                   t.h,
                   t.phase_ms,
                   &no_rings_,
                   c != nullptr ? &c->refs : none};
}

void PulseLayer::show_frame(Tile& t, int index) {
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  const std::uint16_t* frame = t.shown.frames() + static_cast<std::size_t>(index) * px;
  t.shown_index = index;
  if (!alive_ || t.pc[0].img == nullptr) return;
  if (t.frame == nullptr || lv_image_get_src(t.pc[0].img) != &t.pc[0].dsc) {
    point(t, frame);
    set_src(t);
  }
  if (defer_ && tile_shown(t)) {
    // The embedder shows it (presents it, or invalidates): until then the old frame stays.
    // One entry per piece, each at its own place in the frame.
    for (std::size_t k = 0; k < t.pieces; ++k) {
      Piece& pc = t.pc[k];
      const std::uint16_t* at = frame +
                                static_cast<std::size_t>(pc.oy) * static_cast<std::size_t>(t.w) +
                                static_cast<std::size_t>(pc.ox);
      bool listed = false;
      for (Due& d : due_) {
        if (d.dsc == &pc.dsc) {
          d.frame = at;
          listed = true;
        }
      }
      if (!listed) due_.push_back(Due{pc.img, &pc.dsc, at});
    }
    t.frame = frame;  // what a re-cut points the pieces at
    return;
  }
  point(t, frame);
  invalidate_tile(t);
}

void PulseLayer::finish_job() {
  Tile& t = *job_.tile;
  // The new set replaces the old whole; the descriptor moves to it before the old is freed.
  job_.next.complete = true;
  std::swap(t.shown, job_.next);
  t.captures_match = true;
  reset_job();
  t.hidden = false;
  int index = frame_index(t, frozen_ ? 0U : lv_tick_get());
  if (index < 0) {
    ++stats_.not_ready;
    index = 0;  // the set is complete, so frame 0 exists
  }
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  point(t, t.shown.frames() + static_cast<std::size_t>(index) * px);
  (void)drop_due(t);  // listed frames of the old set: the invalidation below draws the new one
  if (alive_ && t.pc[0].img != nullptr) {
    set_src(t);
    t.shown_index = index;
    t.shown_ms = lv_tick_get();
    update_visibility();
    invalidate_tile(t);
  }
}

void PulseLayer::starve(Tile& t) {
  // The port refused a buffer for this tile: it gives back what it holds and stops showing (its
  // captures may already be newer than its frames), and waits for the next change under it
  // rather than asking again at once.
  ++stats_.alloc_failures;
  release(t);
  t.dirty = false;
  update_visibility();
}

void PulseLayer::set_paused(bool paused) {
  if (paused == paused_) return;
  paused_ = paused;
  if (paused && job_.tile != nullptr) {
    // The set being composed is given back; its tile is built again once resumed.
    job_.tile->dirty = true;
    drop_job(job_.tile);
  }
  if (!paused) {
    choose_divisor();  // a coarser rate held back while paused
    update_visibility();
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

  if (job_.out) {
    // The worker has the tables and frames (or, inline, one more step runs here now).
    bool completed = false;
    if (worker_->poll(WorkLane::Sprites, &completed) == Worker::Poll::Busy) return true;
    collect_job(completed);
    return stats().pending;
  }
  if (paused_) return false;
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
      reset_job();  // waits for the next change under the tile
      return stats().pending;
    }
    job_.stage = Job::Stage::CaptureBelow;
    return true;
  }
  if (!capture(t, false)) {
    reset_job();
    return stats().pending;
  }
  if (unchanged(t)) {
    // Nothing under the tile moved (a heat image that is the same here, a pin elsewhere): the
    // playing set is exactly what would be built, so it stays and nothing is composed (D-071).
    ++stats_.sets_kept;
    reset_job();
    t.hidden = false;
    update_visibility();
    return stats().pending;
  }
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  if (!t.full.resize(px) || !t.below.resize(px)) {
    reset_job();
    starve(t);
    return stats().pending;
  }
  t.captures_match = false;
  std::copy_n(staged_full_.data(), px, t.full.data());
  std::copy_n(staged_below_.data(), px, t.below.data());
  if (!submit_job()) starve(t);
  return stats().pending;
}

bool PulseLayer::unchanged(Tile& t) const {
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  const Ring* ring = nullptr;
  const Cell* cell = nullptr;
  for (const Ring& r : rings_) ring = &r.t == &t ? &r : ring;
  if (ring == nullptr) {
    for (const Cell& c : cells_) cell = &c.t == &t ? &c : cell;
  }
  const std::uint32_t frame_ms = (ring != nullptr ? kRingFrameMs : kRoomStepMs) * divisor_;
  if (!t.captures_match || !t.shown.complete || t.shown.count == 0 ||
      t.shown.frame_ms != frame_ms || t.full.size() != px || t.below.size() != px) {
    return false;
  }
  if (!std::equal(t.full.begin(), t.full.end(), staged_full_.data()) ||
      !std::equal(t.below.begin(), t.below.end(), staged_below_.data())) {
    return false;
  }
  // The same pulses, in the same order and colours, as plan_ring() / plan_cell() would list.
  std::size_t at = 0;
  if (ring != nullptr) {
    const std::size_t i = static_cast<std::size_t>(ring - rings_.data());
    for (std::size_t n = 0; n < rings_.size(); ++n) {
      const std::size_t j = n == 0 ? i : (n <= i ? n - 1 : n);
      const Ring& o = rings_[j];
      if (!o.on || !o.allocated) continue;
      const lv_area_t reach{static_cast<std::int32_t>(o.cx) - kRingReach,
                            static_cast<std::int32_t>(o.cy) - kRingReach,
                            static_cast<std::int32_t>(o.cx) + kRingReach,
                            static_cast<std::int32_t>(o.cy) + kRingReach};
      if (!intersects(reach, t.area)) continue;
      if (at >= ring->rings.size() || ring->rings[at].ring != j) return false;
      ++at;
    }
    if (at != ring->rings.size()) return false;
  }
  const BulkVector<RoomRef>& refs = ring != nullptr ? ring->rooms : cell->refs;
  at = 0;
  auto room_reaches = [&t](const Room& room) {
    for (const Segment& e : room.segments) {
      const lv_area_t b{static_cast<std::int32_t>(std::min(e.ax, e.bx)) - kRoomHaloWidth,
                        static_cast<std::int32_t>(std::min(e.ay, e.by)) - kRoomHaloWidth,
                        static_cast<std::int32_t>(std::max(e.ax, e.bx)) + kRoomHaloWidth,
                        static_cast<std::int32_t>(std::max(e.ay, e.by)) + kRoomHaloWidth};
      if (intersects(b, t.area)) return true;
    }
    return false;
  };
  auto check_room = [&](std::size_t k) {
    const Room& room = rooms_[k];
    if (!room.on || !room.allocated || !room_reaches(room)) return true;
    if (at >= refs.size() || refs[at].room != k || refs[at].colour != room.colour) return false;
    ++at;
    return true;
  };
  if (ring != nullptr) {
    for (std::size_t k = 0; k < rooms_.size(); ++k) {
      if (!intersects(rooms_[k].extent, t.area)) continue;
      if (!check_room(k)) return false;
    }
  } else {
    for (std::size_t k : cell->rooms) {
      if (!check_room(k)) return false;
    }
  }
  return at == refs.size();
}

void PulseLayer::tick(std::uint32_t now_ms) {
  if (!alive_ || veiled_) return;
  if (frozen_) now_ms = 0;
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
    if (t.shown.count == 0 || t.hidden || !tile_shown(t)) return;
    // An area per shown piece (D-070): a tile whose pieces do not all fit this tick waits, with
    // its frame unchanged, for the next.
    if (!defer_ && invalidations + t.pieces > kMaxInvalidationsPerTick) return;
    const std::uint32_t fm = t.shown.frame_ms;
    const int index = frame_index(t, now_ms);
    if (index < 0) {
      ++stats_.not_ready;
      return;
    }
    if (index == t.shown_index && now_ms - t.shown_ms < fm) return;
    if (t.shown_index >= 0) {
      // Frame boundaries passed since the last frame shown (wrap-safe), less the one shown now.
      const std::uint32_t passed = (now_ms - t.shown_ms + fm / 2U) / fm;
      if (passed > 1U) stats_.dropped += passed - 1U;
    }
    t.shown_ms = now_ms;
    if (index == t.shown_index) return;  // whole cycles later: the same picture
    show_frame(t, index);
    invalidations += defer_ ? 1U : static_cast<std::uint32_t>(t.pieces);
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
    if (first.shown.count == 0 || !first.vis) continue;
    const auto s = static_cast<int>(((now_ms + room.phase_ms) % kCycleMs) / first.shown.frame_ms);
    if (s != room.shown_step) ++stats_.frames;
    room.shown_step = s;
  }
}

int PulseLayer::frame_index(const Tile& t, std::uint32_t now_ms) const {
  if (!t.shown.complete || t.shown.count == 0 || t.shown.frame_ms == 0) return -1;
  const std::uint32_t index = ((now_ms + t.phase_ms) % kCycleMs) / t.shown.frame_ms;
  return index < t.shown.count ? static_cast<int>(index) : -1;
}

bool PulseLayer::verify_tile(Tile& t, const char** why) {
  auto fail = [why](const char* w) {
    if (why != nullptr) *why = w;
    return false;
  };
  if (!alive_) return true;
  const std::size_t px = static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h);
  const std::uint16_t* b = t.shown.frames();
  // A pointer into a whole frame of the set, given where in the frame it should be.
  auto whole = [&](const std::uint16_t* q, std::size_t offset) {
    if (!t.shown.complete || t.shown.count == 0) return false;
    if (q < b + offset) return false;
    const std::uint16_t* f = q - offset;
    return f < b + t.shown.frame_words() && static_cast<std::size_t>(f - b) % px == 0;
  };
  // Every distinct frame a shown piece points at: pieces of one tile may point at different
  // frames for a moment (a deferred piece waiting on LVGL's area limit while another was shown).
  std::array<const std::uint16_t*, kMaxPieces> shown_frames{};
  std::size_t distinct = 0;
  for (std::size_t k = 0; k < kMaxPieces; ++k) {
    Piece& pc = t.pc[k];
    if (pc.img == nullptr) continue;
    const bool hidden = lv_obj_is_hidden(pc.img);
    if (lv_image_get_src(pc.img) != &pc.dsc) {
      if (!hidden && lv_image_get_src(pc.img) != nullptr) {
        return fail("a tile shows a source that is not its descriptor");
      }
      continue;
    }
    if (k >= t.pieces) {
      if (!hidden) return fail("a piece outside the tile's cut is shown");
      continue;
    }
    if (hidden && pc.dsc.data == nullptr) continue;
    const auto w = static_cast<std::int32_t>(pc.dsc.header.w);
    const auto h = static_cast<std::int32_t>(pc.dsc.header.h);
    lv_obj_update_layout(pc.img);  // a re-cut piece's new size is applied by the layout
    if (w <= 0 || h <= 0 || pc.ox < 0 || pc.oy < 0 || pc.ox + w > t.w || pc.oy + h > t.h ||
        pc.dsc.header.stride != static_cast<std::uint32_t>(t.w) * 2U ||
        lv_obj_get_width(pc.img) != w || lv_obj_get_height(pc.img) != h ||
        lv_image_get_src_width(pc.img) != w || lv_image_get_src_height(pc.img) != h) {
      return fail("a tile's descriptor or object is not the tile's size");
    }
    if (pc.dsc.data == nullptr) {
      if (hidden) continue;
      return fail("a visible tile has no frame");
    }
    if (!t.shown.complete || t.shown.count == 0)
      return fail("a tile points into an incomplete set");
    if (t.shown.frame_words() != px * t.shown.count)
      return fail("a set is not count frames of w x h");
    const std::size_t offset = static_cast<std::size_t>(pc.oy) * static_cast<std::size_t>(t.w) +
                               static_cast<std::size_t>(pc.ox);
    const auto* q = reinterpret_cast<const std::uint16_t*>(pc.dsc.data);
    if (!whole(q, offset)) return fail("a tile points outside its set, or between frames");
    // What the descriptor claims (data_size bytes from its start) lies inside the set's
    // allocation for the set's last frame too, the worst case for a piece low in the tile.
    const std::uint16_t* last_start =
        b + static_cast<std::size_t>(t.shown.count - 1U) * px + offset;
    if (last_start + pc.dsc.data_size / 2U > t.shown.px.data() + t.shown.px.size()) {
      return fail("a piece's descriptor claims memory past its set's allocation");
    }
    const std::uint16_t* f = q - offset;
    if (std::find(shown_frames.begin(),
                  shown_frames.begin() + static_cast<std::ptrdiff_t>(distinct),
                  f) == shown_frames.begin() + static_cast<std::ptrdiff_t>(distinct)) {
      shown_frames[distinct++] = f;
    }
    for (const Due& d : due_) {
      if (d.dsc == &pc.dsc && !whole(d.frame, offset)) {
        return fail("a listed frame lies outside its set");
      }
    }
  }
  // The frame shown equals that frame composed afresh from the tile's captures, unless the tile is
  // being recaptured (its old set plays meanwhile, from captures that are being replaced).
  if (distinct > 0 && !t.dirty && t.captures_match && job_.tile != &t && !t.below.empty() &&
      !t.full.empty()) {
    if (!reference_.resize(px)) return fail("no memory to compose a reference frame");
    for (std::size_t n = 0; n < distinct; ++n) {
      const std::uint16_t* f = shown_frames[n];
      const auto k = static_cast<std::uint32_t>(static_cast<std::size_t>(f - b) / px);
      compose_frame(tile_inputs(t), k, t.shown.frame_ms, reference_.data());
      if (!std::equal(reference_.begin(), reference_.end(), f)) {
        return fail("a shown frame differs from the same frame composed again");
      }
    }
  }
  return true;
}

std::uint32_t PulseLayer::check_canaries() const {
  std::uint32_t bad = 0;
  auto check = [&bad](const FrameSet& f) {
    if (f.px.size() < 2 * kGuardWords + f.pad) return;
    const std::size_t tail = f.px.size() - f.pad;  // the trailing guard ends here
    for (std::size_t i = 0; i < kGuardWords; ++i) {
      if (f.px[i] != kCanary || f.px[tail - 1 - i] != kCanary) {
        ++bad;
        return;
      }
    }
  };
  for (const Ring& r : rings_) check(r.t.shown);
  for (const Cell& c : cells_) check(c.t.shown);
  check(job_.next);
  return bad;
}

bool PulseLayer::verify_shown(const char** why) {
  for (Ring& r : rings_) {
    if (!verify_tile(r.t, why)) return false;
  }
  for (Cell& c : cells_) {
    if (!verify_tile(c.t, why)) return false;
  }
  return true;
}

const BulkVector<PulseLayer::Due>& PulseLayer::take_due() {
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
  s.paused = paused_;
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
