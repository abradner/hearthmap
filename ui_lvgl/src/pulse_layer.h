// pulse_layer.h — danger rings and room outlines as pre-rendered opaque sprites (D-053).
//
// Every pulse is an opaque RGB565 lv_image ("tile") over the map, above the pins. Its content is
// composed by us, never drawn by LVGL: the scene under the tile is captured twice through LVGL's
// own renderer (render_area(): once without the pins, once with them), and each frame blends the
// pulse shapes over the first capture from look-up tables, keeping the pins' pixels from the
// second. LVGL then only copies the tile, which on the panel costs a tenth of drawing a ring.
//
// Captures are expensive (a software render of the tile) and happen in prepare_step(), one tile
// per call, from the embedder's sliced work; frames are cheap and run on an LVGL timer from
// LVGL's tick, so a slow loop drops frames instead of queueing them.
//
// Budget by construction (PulseBudget): tiles are allocated only while their memory fits the
// budget, in config order (a pulsing pin or room past it shows its badge or nothing), at most
// max_animated_rooms room outlines animate (others show a steady outline), and each ring frame
// invalidates at most its own tile.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/ui/sprite_composer.h"
#include "hearthmap/ui/theme.h"
#include "lvgl.h"
#include "sprites.h"

namespace hearthmap::ui::detail {

/// What the pulse layer may use. Derived by the embedder from its memory (D-054); the defaults
/// suit the 32 MB panel and a 1280 x 800 screen.
struct PulseBudget {
  std::size_t memory_bytes = 2U * 1024U * 1024U;  ///< tiles, captures and tables, all pulses
  std::size_t max_animated_rooms = 6;             ///< more pulsing rooms show a steady outline
};

struct PulseStats {
  std::uint32_t frames = 0;        ///< Ring frames and room steps shown since construction.
  std::uint32_t dropped = 0;       ///< Ring frames skipped because the clock moved on.
  std::uint32_t captures = 0;      ///< Tiles captured since construction.
  std::size_t tile_bytes = 0;      ///< Memory held by tiles and their tables now.
  std::size_t rings_shown = 0;     ///< Rings with a tile now.
  std::size_t rooms_animated = 0;  ///< Room outlines animating now.
  bool pending = false;            ///< prepare_step() has work.
};

class PulseLayer {
 public:
  static constexpr std::uint32_t kRingCycleMs = theme::kPulseRingCycleMs;
  static constexpr std::size_t kRingFrames = 20;  ///< 12.5 frames/s per ring
  static constexpr std::uint32_t kRoomCycleMs = theme::kRoomPulseCycleMs;
  static constexpr std::size_t kRoomSteps = 14;  ///< 5 steps/s per room
  static constexpr std::uint32_t kTickMs = 16;   ///< ESPHome's refresh period
  static constexpr int kRingFrom = theme::kPulseRingFrom;
  static constexpr int kRingTo = theme::kPulseRingTo;
  static constexpr int kRingWidth = theme::kPulseRingWidth;
  static constexpr int kRoomWidth = theme::kRoomPulseWidth;
  static constexpr std::int32_t kEdgePieceMax = 48;  ///< longest diagonal piece, px

  /// `map` is the map container at `map_origin` (screen px) of `map_size`. `pins` are the pin
  /// containers, hidden while the scene under a tile is captured. Tiles are created above
  /// everything already in `map`, so construct this after the pins.
  PulseLayer(lv_obj_t* map, lv_point_t map_origin, lv_point_t map_size,
             const std::vector<std::vector<lv_point_precise_t>>& room_outlines,
             const std::vector<lv_point_t>& pin_centres, std::vector<lv_obj_t*> pins,
             PulseBudget budget, const SpriteComposer& ops);
  ~PulseLayer();
  PulseLayer(const PulseLayer&) = delete;
  PulseLayer& operator=(const PulseLayer&) = delete;

  /// Per room: its outline container (hidden when not pulsing), and per pin its ring tile.
  const std::vector<lv_obj_t*>& room_objects() const { return room_box_; }
  const std::vector<lv_obj_t*>& ring_objects() const { return ring_img_; }

  void set_ring(std::size_t pin, bool on);
  /// colour: 0xRRGGBB; `on` false hides the outline.
  void set_room(std::size_t room, bool on, std::uint32_t colour);
  /// Something under `area` (screen px) changed: recapture the tiles it touches.
  void mark_dirty(const lv_area_t& area);
  /// The whole map changed: every tile is recaptured. With `hide` (another layer's colours) the
  /// tiles step aside until then; without (a refresh of the same layer's image, close to the old
  /// one) they keep their old background for the few slices it takes.
  void mark_all_dirty(bool hide);
  /// While veiled every tile is hidden (the map under them is not the one they captured).
  void set_veiled(bool veiled);

  /// One unit of preparation (one tile captured and composed). Returns true while work remains.
  bool prepare_step();
  /// Show every pulse's frame for `now_ms` (also run by the layer's own timer).
  void tick(std::uint32_t now_ms);
  PulseStats stats() const;
  /// The LVGL objects must not be touched any more (their parent was deleted).
  void forget_objects();

 private:
  struct Tile {
    lv_obj_t* img = nullptr;
    lv_image_dsc_t dsc{};
    lv_area_t area{};  // screen px
    std::int32_t w = 0, h = 0;
    std::vector<std::uint16_t> below, full, out;
    bool has_base = false;
    bool dirty = true;
  };
  struct RingRef {
    std::size_t ring = 0;
    std::vector<std::uint16_t> distance;
  };
  struct RoomRef {
    std::size_t room = 0;
    std::vector<std::uint8_t> coverage;
    lv_area_t box{};      ///< screen px: where the coverage is non-zero
    int drawn_step = -3;  ///< the room step this tile last composed
  };
  struct Ring {
    Tile t;
    float cx = 0, cy = 0;
    bool on = false;
    bool allocated = false;
    bool geometry_dirty = true;
    int frame = -1;
    std::uint32_t phase = 0;
    std::vector<RingRef> rings;
    std::vector<RoomRef> rooms;
  };
  struct Edge {
    Tile t;
    std::vector<std::uint8_t> coverage;
    float ax = 0, ay = 0, bx = 0, by = 0;
  };
  struct Room {
    std::vector<Edge> edges;
    bool on = false;
    bool allocated = false;
    bool animated = false;
    std::uint16_t colour = 0;
    std::uint32_t phase = 0;
    int step = -1;
    std::uint8_t opa = 255;
    lv_area_t extent{};  // screen px, the outline with its width
  };

  static void on_timer(lv_timer_t* t);
  void make_tile(Tile& t, lv_obj_t* parent, lv_point_t parent_origin);
  void allocate(Tile& t);
  static std::size_t tile_bytes(const Tile& t);
  std::size_t bytes_in_use() const;
  bool fits(std::size_t more) const { return bytes_in_use() + more <= budget_.memory_bytes; }
  void release(Tile& t);
  bool capture(Tile& t);
  void compose_ring(std::size_t i, const lv_area_t& box);
  void compose_edge(const Room& room, Edge& e);
  void rebuild_ring_geometry(std::size_t i);
  void update_visibility();
  void update_animated_rooms();
  void ensure_timer();
  lv_area_t ring_box(const Ring& r, int frame) const;

  lv_obj_t* map_;
  PulseBudget budget_;
  const SpriteComposer* ops_;
  lv_point_t origin_;
  lv_area_t map_area_{};
  std::vector<lv_obj_t*> pins_;
  std::vector<Ring> rings_;
  std::vector<Room> rooms_;
  std::vector<lv_obj_t*> ring_img_;
  std::vector<lv_obj_t*> room_box_;
  std::array<std::vector<std::uint8_t>, kRingFrames> ring_alpha_;
  std::array<std::uint8_t, kRoomSteps> room_opa_{};
  lv_timer_t* timer_ = nullptr;
  bool veiled_ = false;
  bool alive_ = true;
  PulseStats stats_;
};

}  // namespace hearthmap::ui::detail
