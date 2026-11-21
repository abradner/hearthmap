// pulse_layer.h — danger rings and room outlines as pre-rendered opaque sprites (D-053).
//
// Every pulse is an opaque RGB565 lv_image ("tile") over the map, above the pins. A ring's tile
// is its whole reach; outlines are drawn by the cells of a kCellPx grid that some room's stroke
// passes through, one tile per cell whichever rooms share it, so outline tiles never overlap
// one another and each can be presented (D-058). Its frames are
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
//
// Memory (D-064): the layer itself, every frame set, capture, table and the containers holding
// them come from the embedder's memory port (hearthmap/ui/memory.h), never the default heap. A
// buffer the port refuses fails that tile's preparation (counted in PulseStats::alloc_failures);
// the tile waits for the next change under it, as after a failed capture.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/ui/memory.h"
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
  std::uint32_t min_divisor = 1;        ///< keep at most 1/N of the frames (1, 2, 4 or 8)
};

struct PulseStats {
  std::uint32_t frames = 0;          ///< Frames shown since construction.
  std::uint32_t dropped = 0;         ///< Ring frames skipped because the clock moved on.
  std::uint32_t captures = 0;        ///< Tiles captured since construction.
  std::size_t tile_bytes = 0;        ///< Memory held by tiles, frames and tables now.
  std::size_t rings_shown = 0;       ///< Rings with frames now.
  std::size_t rooms_animated = 0;    ///< Room outlines with frames now.
  std::uint32_t frame_divisor = 1;   ///< 1: full frame rate; 2, 4, 8: fewer frames to fit memory
  std::uint32_t not_ready = 0;       ///< frames not shown: their set incomplete, or out of range.
  bool pending = false;              ///< prepare_step() has work.
  std::uint64_t frame_us = 0;        ///< Time in tick() (choosing and showing frames).
  std::uint64_t prepare_us = 0;      ///< Time in prepare_step() (captures and composing frames).
  std::uint32_t alloc_failures = 0;  ///< Buffers the memory port refused (D-064).
  bool paused = false;               ///< set_paused(): no new sets are being built.
};

class PulseLayer {
 public:
  static constexpr std::uint32_t kCycleMs = theme::kPulseRingCycleMs;  ///< rings and rooms
  static constexpr std::uint32_t kRingFrameMs = 33;  ///< 48 frames per cycle: 30 frames/s
  static constexpr std::uint32_t kRoomStepMs = 66;   ///< 24 steps per cycle
  /// Frames per cycle may be divided by 2, 4 or 8 to fit memory. Every divided frame time must
  /// still divide the cycle, or the clock would ask for a frame past the end of a set.
  static constexpr std::uint32_t kMaxDivisor = 8;
  static constexpr std::uint32_t kTickMs = 16;  ///< ESPHome's refresh period
  static constexpr std::uint32_t kMaxInvalidationsPerTick = 20;
  static constexpr int kRingFrom = theme::kPulseRingFrom;
  static constexpr int kRingTo = theme::kPulseRingTo;
  static constexpr int kRingWidth = theme::kPulseRingWidth;
  static constexpr int kRoomWidth = theme::kRoomPulseWidth;
  /// The outline grid's cell, px (a multiple of theme::kTileAlign): every outline tile lies
  /// inside one cell, so within the 128 x 128 a presenter accepts.
  static constexpr std::int32_t kCellPx = 64;

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

  /// Per room: an empty marker object, hidden when the room is not pulsing, whose line colour is
  /// its outline's. Per pin: its ring tile. And the container of every outline tile.
  const std::vector<lv_obj_t*>& room_objects() const { return room_marker_; }
  const std::vector<lv_obj_t*>& ring_objects() const { return ring_img_; }
  lv_obj_t* outline_object() const { return outline_box_; }

  void set_ring(std::size_t pin, bool on);
  /// colour: 0xRRGGBB; `on` false hides the outline.
  void set_room(std::size_t room, bool on, std::uint32_t colour);
  /// Something under `area` (screen px) changed: recompose the tiles it touches.
  void mark_dirty(const lv_area_t& area);
  /// The whole map changed: every tile is recomposed. With `hide` (another layer's colours) the
  /// tiles step aside until then; without (a refresh of the same layer's image) the old frames
  /// keep playing until the new ones are ready.
  void mark_all_dirty(bool hide);
  /// `o` (drawn in the map, above the pulses) is hidden whenever a tile's background is captured.
  void hide_in_captures(lv_obj_t* o) { never_captured_.push_back(o); }
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
  /// Frozen: tick() chooses every tile's frame for time 0, so each shows one fixed frame
  /// (Screen::set_pulse_mode, PulseMode::Still).
  void set_frozen(bool frozen) { frozen_ = frozen; }
  /// Paused: prepare_step() starts nothing new (a set being composed is dropped), and the sets
  /// already built keep playing. The embedder pauses it when memory runs short (D-064).
  void set_paused(bool paused);
  struct Due {
    lv_obj_t* image;
    lv_image_dsc_t* dsc;
    const std::uint16_t* frame;
  };
  /// The frames chosen since the last call, for the embedder to show (present or invalidate).
  const BulkVector<Due>& take_due();
  /// Back on the list, for the next take_due() (the embedder ran out of invalidations).
  void requeue(const Due& d) { due_.push_back(d); }

  PulseStats stats() const;
  /// Every tile that LVGL or a presenter may show points at a whole frame of a complete set of
  /// its own size. False, with the first problem in `why`, otherwise. For tests: walks all tiles.
  bool verify_shown(const char** why);
  /// Frame sets whose canaries have changed (something wrote outside its frames), now.
  std::uint32_t check_canaries() const;
  /// What a frame not composed yet holds, so a test can tell one shown by mistake.
  static constexpr std::uint16_t kUncomposed = 0xF81F;
  /// The LVGL objects must not be touched any more (their parent was deleted).
  void forget_objects();

 private:
  /// Canary words either side of every frame set's frames, checked by check_canaries().
  static constexpr std::size_t kGuardWords = 16;
  static constexpr std::uint16_t kCanary = 0xA5C3;
  struct FrameSet {
    /// kGuardWords canaries, `count` frames of w x h back to back, kGuardWords canaries.
    BulkBuffer<std::uint16_t> px;
    std::uint16_t* frames() { return px.data() + kGuardWords; }
    const std::uint16_t* frames() const { return px.data() + kGuardWords; }
    std::size_t frame_words() const {
      return px.size() < 2 * kGuardWords ? 0 : px.size() - 2 * kGuardWords;
    }
    std::uint32_t count = 0;
    std::uint32_t frame_ms = 0;  ///< count * frame_ms == kCycleMs
    bool complete = false;       ///< every frame composed: only then is it shown
  };
  struct Tile {
    lv_obj_t* img = nullptr;
    lv_image_dsc_t dsc{};
    lv_area_t area{};  // screen px
    std::int32_t w = 0, h = 0;
    BulkBuffer<std::uint16_t> below, full;  ///< the captures
    FrameSet shown;                         ///< what plays
    std::uint32_t phase_ms = 0;             ///< a whole number of frames
    int shown_index = -1;
    std::uint32_t shown_ms = 0;
    bool dirty = true;    ///< needs a capture and new frames
    bool hidden = false;  ///< step aside until new frames exist
  };
  struct RingRef {
    std::size_t ring = 0;
    BulkBuffer<std::uint16_t> distance;
    TileBox box;  ///< where that ring can reach in this tile
  };
  struct RoomRef {
    std::size_t room = 0;
    BulkBuffer<std::uint8_t> coverage;
    BulkBuffer<std::uint8_t> halo;  ///< the dark keyline under the stroke
    TileBox box;
  };
  struct Ring {
    Tile t;
    float cx = 0, cy = 0;
    bool on = false;
    bool allocated = false;
    BulkVector<RingRef> rings;
    BulkVector<RoomRef> rooms;
  };
  /// One outline tile: a cell of the grid, and every room whose stroke reaches it.
  struct Cell {
    Tile t;
    lv_area_t rect{};               ///< the grid cell, within the tile area
    BulkVector<std::size_t> rooms;  ///< in config order
    BulkVector<RoomRef> refs;       ///< the pulsing ones, from the last capture
    bool gone = false;  ///< wholly under a ring's tile, which draws its strokes: no tile
  };
  struct Segment {
    float ax = 0, ay = 0, bx = 0, by = 0;  // screen px
  };
  struct Room {
    BulkVector<Segment> segments;
    BulkVector<std::size_t> cells;
    std::uint32_t phase_ms = 0;  ///< a whole number of room steps at the coarsest rate
    int shown_step = -1;
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
  void size_tile(Tile& t, lv_point_t parent_origin);
  /// The aligned box, within the cell, of the strokes of the rooms through it that pulse (and
  /// of `with`, if it is one of them); the current area when none.
  lv_area_t cell_area(const Cell& c, std::size_t with) const;
  /// `a` less what the rings' tiles cover, where that leaves a rectangle (a ring's tile draws the
  /// strokes under it, so a cell need not); false when a ring covers all of it.
  bool trim_by_rings(lv_area_t& a) const;
  /// The cell resized to its strokes less the rings (frames of another size are dropped).
  void fit_cell(Cell& c);
  void fit_cells(const Room& r);
  void fit_cells_under(const lv_area_t& area);
  /// Admits `room` if everything fits at the coarsest rate with it; otherwise leaves it waiting.
  bool try_admit_room(std::size_t room);
  std::size_t need_at(std::uint32_t div, bool with_rooms) const;
  bool fits_at_coarsest(bool with_rooms) const;
  /// Gives up a room's memory (it stays on, waiting); its cells no other room holds are freed.
  void drop_room(std::size_t room);
  void release(Tile& t);
  /// The memory port refused one of `t`'s buffers (D-064).
  void starve(Tile& t);
  bool capture(Tile& t, bool with_pins);
  /// False when the memory port refused a table (the tile's tables are then incomplete).
  bool rebuild_ring_geometry(std::size_t i);
  bool rebuild_cell_geometry(Cell& c);
  void compose_frame(Tile& t, std::uint32_t k, std::uint32_t frame_ms, std::uint16_t* out);
  void finish_job();
  void cancel_stale_job();
  void update_visibility();
  void ensure_timer();
  void admit_waiting();
  void choose_divisor();
  std::size_t tile_frame_bytes(const Tile& t, bool ring) const;
  /// Without rooms: what rings (and the set being composed) hold alone.
  std::size_t bytes_in_use(bool with_rooms = true) const;
  int ring_frame_at(std::size_t ring, std::uint32_t t_ms) const;
  std::uint8_t room_opa_at(std::size_t room, std::uint32_t t_ms) const;
  const std::uint8_t* opa_lut(std::uint8_t opa);
  bool is_ring_tile(const Tile* t) const;
  Ring* ring_of(const Tile* t);
  Cell* cell_of(const Tile* t);
  /// Some room through it pulses and holds memory: the cell needs frames.
  bool cell_active(const Cell& c) const;
  /// Active, and not wholly under a ring: it needs frames.
  bool cell_live(const Cell& c) const { return !c.gone && cell_active(c); }
  /// Every cell of the room has frames.
  bool room_ready(const Room& r) const;
  void show_frame(Tile& t, int index);
  /// The frame of the tile's set for `now_ms`; -1 when the set is not complete or the index
  /// would fall outside it (never shown: counted as not ready).
  int frame_index(const Tile& t, std::uint32_t now_ms) const;
  bool verify_tile(Tile& t, const char** why);

  lv_obj_t* map_;
  PulseBudget budget_;
  const SpriteComposer* ops_;
  lv_point_t origin_;
  lv_area_t map_area_{};
  lv_area_t tile_area_{};  ///< map_area_ shrunk to whole kTileAlign blocks
  // Object lists for the embedder and for hiding (a pointer per pin or room, sized here once)
  // stay plain vectors: ScreenWidgets hands them out as such.
  std::vector<lv_obj_t*> pins_;
  BulkVector<lv_obj_t*> never_captured_;
  BulkVector<Ring> rings_;
  BulkVector<Room> rooms_;
  BulkVector<Cell> cells_;
  std::vector<lv_obj_t*> ring_img_;
  std::vector<lv_obj_t*> room_marker_;
  BulkVector<lv_obj_t*> capture_hidden_;  ///< capture()'s scratch, reserved once
  lv_obj_t* outline_box_ = nullptr;
  /// Ring alpha by distance for each of the cycle's ring frames, and its keyline's.
  BulkVector<BulkBuffer<std::uint8_t>> ring_alpha_;
  BulkVector<BulkBuffer<std::uint8_t>> ring_halo_;
  std::array<std::uint8_t, kCycleMs / kRoomStepMs> room_opa_{};  ///< per room step
  std::array<std::array<std::uint8_t, 256>, 256> opa_luts_{};    ///< coverage -> alpha, per opa
  std::array<bool, 256> opa_lut_ready_{};
  std::uint32_t divisor_ = 1;
  Job job_;
  BulkVector<Due> due_;  ///< reserved once: at most two per tile
  BulkVector<Due> due_out_;
  BulkBuffer<std::uint16_t> reference_;  ///< verify_shown()'s scratch frame
  bool defer_ = false;
  bool frozen_ = false;
  bool paused_ = false;
  lv_timer_t* timer_ = nullptr;
  bool veiled_ = false;
  bool alive_ = true;
  PulseStats stats_;
};

}  // namespace hearthmap::ui::detail
