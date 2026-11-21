// pulse_layer.h — danger rings and room outlines as pre-rendered opaque sprites (D-053).
//
// Every pulse is an opaque RGB565 lv_image ("tile") over the map, above the pins. Its frames are
// composed by us, never drawn by LVGL: the scene under the tile is captured through LVGL's own
// renderer (render_area(): once with the pins, once without), and every frame of the tile's
// cycle is composed from those captures once, in prepare_step(), and kept (RAM for CPU, D-053).
// Animating is then only pointing the tile's descriptor at the next frame: no arithmetic per
// frame. After a change under a tile its frames are recomposed in the background while the old
// set keeps playing; the new set replaces it whole.
//
// Frames come from LVGL's tick: frame = (tick + phase) mod cycle / frame time, so a slow loop
// skips frames instead of queueing them. Every phase is a whole number of frames, so a tile's
// frame k shows each neighbouring pulse exactly at the state that pulse shows at the same time.
//
// Budget by construction (PulseBudget): frames per cycle are divided by 1, 2 or 4 until every
// pulsing object fits the memory budget; past that, pulses are admitted in config order (rings
// first) while they fit, and the rest show their badge or nothing.
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
/// suit a small target.
struct PulseBudget {
  std::size_t memory_bytes = 2U * 1024U * 1024U;  ///< frames, captures and tables, all pulses
  std::uint32_t (*micros)() = nullptr;  ///< a microsecond clock to time captures and composing
};

struct PulseStats {
  std::uint32_t frames = 0;         ///< Frames shown since construction.
  std::uint32_t dropped = 0;        ///< Ring frames skipped because the clock moved on.
  std::uint32_t captures = 0;       ///< Tiles captured since construction.
  std::size_t tile_bytes = 0;       ///< Memory held by tiles, frames and tables now.
  std::size_t rings_shown = 0;      ///< Rings with frames now.
  std::size_t rooms_animated = 0;   ///< Room outlines with frames now.
  std::uint32_t frame_divisor = 1;  ///< 1: full frame rate; 2, 4: fewer frames to fit memory.
  bool pending = false;             ///< prepare_step() has work.
  std::uint64_t frame_us = 0;       ///< Time in tick() (choosing and showing frames).
  std::uint64_t prepare_us = 0;     ///< Time in prepare_step() (captures and composing frames).
};

class PulseLayer {
 public:
  static constexpr std::uint32_t kCycleMs = theme::kPulseRingCycleMs;  ///< rings and rooms
  static constexpr std::uint32_t kRingFrameMs = 32;  ///< 50 frames per cycle: 31 frames/s
  static constexpr std::uint32_t kRoomStepMs = 64;   ///< 25 steps per cycle
  static constexpr std::uint32_t kTickMs = 16;       ///< ESPHome's refresh period
  static constexpr std::uint32_t kMaxInvalidationsPerTick = 20;
  static constexpr int kRingFrom = theme::kPulseRingFrom;
  static constexpr int kRingTo = theme::kPulseRingTo;
  static constexpr int kRingWidth = theme::kPulseRingWidth;
  static constexpr int kRoomWidth = theme::kRoomPulseWidth;
  /// Longest piece an outline edge is cut into, px, so every tile stays within the 128 x 128 a
  /// presenter accepts.
  static constexpr std::int32_t kEdgePieceMax = 96;

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
  /// Something under `area` (screen px) changed: recompose the tiles it touches.
  void mark_dirty(const lv_area_t& area);
  /// The whole map changed: every tile is recomposed. With `hide` (another layer's colours) the
  /// tiles step aside until then; without (a refresh of the same layer's image) the old frames
  /// keep playing until the new ones are ready.
  void mark_all_dirty(bool hide);
  /// While veiled every tile is hidden (the map under them is not the one they captured).
  void set_veiled(bool veiled);

  /// One unit of preparation: one capture, or one frame composed. Returns true while work
  /// remains.
  bool prepare_step();

  /// Choose every tile's frame for `now_ms` (run by the layer's own timer). With `defer`, a tile
  /// whose frame changed is only listed (take_due()); without, its descriptor is pointed at the
  /// frame and the tile invalidated at once.
  void tick(std::uint32_t now_ms);
  void set_defer(bool defer) { defer_ = defer; }
  struct Due {
    lv_obj_t* image;
    lv_image_dsc_t* dsc;
    const std::uint16_t* frame;
  };
  /// The frames chosen since the last call, for the embedder to show (present or invalidate).
  const std::vector<Due>& take_due();

  PulseStats stats() const;
  /// The LVGL objects must not be touched any more (their parent was deleted).
  void forget_objects();

 private:
  struct FrameSet {
    std::vector<std::uint16_t> px;  ///< `count` frames of w x h, back to back
    std::uint32_t count = 0;
    std::uint32_t frame_ms = 0;  ///< count * frame_ms == kCycleMs
  };
  struct Tile {
    lv_obj_t* img = nullptr;
    lv_image_dsc_t dsc{};
    lv_area_t area{};  // screen px
    std::int32_t w = 0, h = 0;
    std::vector<std::uint16_t> below, full;  ///< the captures
    FrameSet shown;                          ///< what plays
    std::uint32_t phase_ms = 0;              ///< a whole number of frames
    int shown_index = -1;
    std::uint32_t shown_ms = 0;
    bool dirty = true;    ///< needs a capture and new frames
    bool hidden = false;  ///< step aside until new frames exist
  };
  struct RingRef {
    std::size_t ring = 0;
    std::vector<std::uint16_t> distance;
    TileBox box;  ///< where that ring can reach in this tile
  };
  struct RoomRef {
    std::size_t room = 0;
    std::vector<std::uint8_t> coverage;
    std::vector<std::uint8_t> halo;  ///< the dark keyline under the stroke
    TileBox box;
  };
  struct Ring {
    Tile t;
    float cx = 0, cy = 0;
    bool on = false;
    bool allocated = false;
    std::vector<RingRef> rings;
    std::vector<RoomRef> rooms;
  };
  struct Edge {
    Tile t;
    std::vector<RoomRef> rooms;
    float ax = 0, ay = 0, bx = 0, by = 0;
  };
  struct Room {
    std::vector<Edge> edges;
    bool on = false;
    bool allocated = false;
    std::uint16_t colour = 0;
    lv_area_t extent{};  // screen px, the outline with its keyline
  };
  /// The tile being prepared: which, and how far.
  struct Job {
    Tile* tile = nullptr;
    enum class Stage : std::uint8_t {
      CaptureFull,
      CaptureBelow,
      Compose
    } stage = Stage::CaptureFull;
    FrameSet next;
    std::uint32_t frame = 0;
  };

  static void on_timer(lv_timer_t* t);
  void make_tile(Tile& t, lv_obj_t* parent, lv_point_t parent_origin);
  void release(Tile& t);
  bool capture(Tile& t, bool with_pins);
  void rebuild_ring_geometry(std::size_t i);
  void rebuild_edge_geometry(Edge& e);
  void compose_frame(Tile& t, std::uint32_t k, std::uint16_t* out);
  void finish_job();
  void cancel_stale_job();
  void update_visibility();
  void ensure_timer();
  void admit_waiting();
  void choose_divisor();
  std::size_t tile_frame_bytes(const Tile& t, bool ring) const;
  std::size_t bytes_in_use() const;
  bool fits(std::size_t more) const { return bytes_in_use() + more <= budget_.memory_bytes; }
  int ring_frame_at(std::size_t ring, std::uint32_t t_ms) const;
  std::uint8_t room_opa_at(std::size_t room, std::uint32_t t_ms) const;
  const std::uint8_t* opa_lut(std::uint8_t opa);
  bool is_ring_tile(const Tile* t) const;
  Ring* ring_of(const Tile* t);
  Edge* edge_of(const Tile* t, std::size_t* room_index);
  void show_frame(Tile& t, int index);

  lv_obj_t* map_;
  PulseBudget budget_;
  const SpriteComposer* ops_;
  lv_point_t origin_;
  lv_area_t map_area_{};
  lv_area_t tile_area_{};  ///< map_area_ shrunk to whole kTileAlign blocks
  std::vector<lv_obj_t*> pins_;
  std::vector<Ring> rings_;
  std::vector<Room> rooms_;
  std::vector<lv_obj_t*> ring_img_;
  std::vector<lv_obj_t*> room_box_;
  /// Ring alpha by distance for each of the cycle's ring frames, and its keyline's.
  std::vector<std::vector<std::uint8_t>> ring_alpha_;
  std::vector<std::vector<std::uint8_t>> ring_halo_;
  std::vector<std::uint8_t> room_opa_;                         ///< per room step
  std::array<std::array<std::uint8_t, 256>, 256> opa_luts_{};  ///< coverage -> alpha, per opa
  std::array<bool, 256> opa_lut_ready_{};
  std::uint32_t divisor_ = 1;
  Job job_;
  std::vector<Due> due_;
  std::vector<Due> due_out_;
  bool defer_ = false;
  lv_timer_t* timer_ = nullptr;
  bool veiled_ = false;
  bool alive_ = true;
  PulseStats stats_;
};

}  // namespace hearthmap::ui::detail
