// screen.h — the LVGL object tree for one hearthmap screen, driven by core's ViewModel.
//
// Construction builds everything that depends only on the config (rooms, walls, labels, one
// widget per pin, the HUD card pool, the overlay). update() then changes only what differs from
// the previous update: it compares what each widget would show and touches LVGL only where that
// changed. Nothing is rebuilt per update except the layer tabs, and those only when the set of
// enabled layers changes.
//
// The Screen draws; it does not decide (AGENTS.md principle 2). Its only inputs are the config,
// the view model, the header text, `now` (for "seen N min ago") and the embedder's SourceStatus.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/ui/fonts.h"
#include "hearthmap/ui/ports.h"
#include "hearthmap/ui/present.h"
#include "hearthmap/ui/sprite_composer.h"
#include "hearthmap/view_model.h"
#include "lvgl.h"

namespace hearthmap::ui {

/// Text for the top bar, formatted by the embedder (it owns the clock and the time zone).
struct HeaderText {
  std::string clock;    ///< e.g. "09:00"
  std::string summary;  ///< e.g. "House 21.4°   52% RH   PM2.5 6"
};

/// What the embedder knows about its sources and its heat image that the view model does not
/// carry (docs/decisions.md D-046). None of it is a judgement core could make: it describes the
/// plumbing around core, so it stays out of the ViewModel. The defaults are the host's: a fixture
/// whose timestamps are the source's own, nothing to be disconnected from, and a heat image that
/// is always drawn for the layer shown.
struct SourceStatus {
  /// The readings' source is disconnected (on the panel: Home Assistant's API connection with
  /// its state subscription is down). The top bar shows "Offline" in the warn colour instead of
  /// the house summary, until a later update clears it.
  bool offline = false;
  /// The readings' last_updated were set by the embedder, not the source: the device re-stamps
  /// every value while connected (readings.h, D-012), so its "last seen" would always read "just
  /// now". The overlay then shows the room only, with no "seen" time.
  bool restamped = false;
  /// The heat image does not show the view model's active layer yet (a rebuild for the newly
  /// selected layer is in progress, or no image has been drawn). The heat image is hidden (the map
  /// shows the background under walls, labels and pins) until an update clears it, then revealed in
  /// bands over about half a second, so a layer's colours are never shown under another
  /// layer's tab (D-052).
  bool heat_behind = false;
  /// The heat image shown carries the walls and room labels (Screen::bake_rows() ran over all
  /// of it). The wall and label objects are then hidden, so LVGL no longer draws them (D-055).
  bool heat_baked = false;
};

struct ScreenDeps {
  Fonts fonts;
  EventSink* sink = nullptr;              ///< Required.
  const ImageProvider* photos = nullptr;  ///< Optional: nullptr draws every plant as a leaf.
  /// Optional: how sprite frames are composed (D-054). nullptr: plain C++
  /// (cpu_sprite_composer()).
  const SpriteComposer* sprite_composer = nullptr;
  /// Memory the pulse sprites may hold, bytes (D-053, D-054). 0: 2 MiB. Pulsing pins and rooms
  /// past it show their badge or a steady state instead of an animation.
  std::size_t pulse_memory_bytes = 0;
  /// Optional: a microsecond clock, so pulse_counters() can say how long frames and captures
  /// take (the panel's perf log). ui_lvgl reads no clock of its own.
  std::uint32_t (*micros)() = nullptr;
  /// Memory the map bake may hold, bytes (D-055; 3 per map pixel). 0: 4 MiB. Without enough,
  /// walls and labels stay LVGL objects.
  std::size_t bake_memory_bytes = 0;
  /// Keep at most 1/N of the pulse frames per cycle (1, 2, 4 or 8), whatever memory allows. For
  /// tests, and for a target that wants the memory back.
  std::uint32_t pulse_min_divisor = 1;
};

/// The pulse sprites' counters (D-053), for an embedder's performance log.
struct PulseCounters {
  std::uint32_t frames = 0;    ///< ring frames and room steps shown since construction
  std::uint32_t dropped = 0;   ///< ring frames skipped because the clock moved on
  std::uint32_t captures = 0;  ///< sprite backgrounds rendered since construction
  std::size_t tile_bytes = 0;  ///< memory the sprites hold now
  std::size_t rings_shown = 0;
  std::size_t rooms_animated = 0;
  std::uint64_t frame_us = 0;       ///< composing frames, in LVGL's timer (with ScreenDeps::micros)
  std::uint64_t prepare_us = 0;     ///< in prepare_step() (with ScreenDeps::micros)
  std::uint32_t frame_divisor = 1;  ///< 1: full frame rate; 2, 4, 8: fewer frames to fit memory
  std::uint32_t not_ready = 0;      ///< frames held back: set incomplete, or index out of range
};

/// Raw pointers to the widgets, for tests and for embedders that want to restyle. Owned by the
/// Screen; valid until it is destroyed (tab pointers until the tab set changes).
struct ScreenWidgets {
  lv_obj_t* root = nullptr;
  lv_obj_t* clock = nullptr;
  lv_obj_t* summary = nullptr;
  std::vector<lv_obj_t*> tabs;        ///< One per ViewModel::tabs entry, same order.
  lv_obj_t* map = nullptr;            ///< Click target for TapMap; at canvas.map_area.
  lv_obj_t* heat_image = nullptr;     ///< Container of the heat image's strip images (D-052).
  std::vector<lv_obj_t*> room_pulse;  ///< Per room, config order. Hidden when not pulsing.
  lv_obj_t* outline_pulse = nullptr;  ///< Every room outline's tiles (cells of a grid).
  std::vector<lv_obj_t*> room_label;  ///< Per room, config order.
  std::vector<lv_obj_t*> pin;         ///< Per pin, config order: the pin's container.
  std::vector<lv_obj_t*> pin_value;   ///< Per pin: value label (plain pins), else nullptr.
  std::vector<lv_obj_t*> pin_badge;   ///< Per pin: warn/danger badge.
  std::vector<lv_obj_t*> pin_pulse;   ///< Per pin: danger pulse ring.
  std::vector<lv_obj_t*> pin_ring;    ///< Per pin: moisture arc (plant/soil pins), else nullptr.
  lv_obj_t* hud_count = nullptr;
  lv_obj_t* hud_empty = nullptr;
  std::vector<lv_obj_t*> hud_card;  ///< Pool of config.hud.max_items cards.
  std::vector<lv_obj_t*> hud_name;
  std::vector<lv_obj_t*> hud_room;
  std::vector<lv_obj_t*> hud_percent;
  lv_obj_t* hud_more = nullptr;
  /// The overlay: hidden when no pin is open. Card-sized and drawing nothing itself; its click
  /// area reaches past every screen edge, so it takes every tap outside the card (D-052).
  lv_obj_t* scrim = nullptr;
  lv_obj_t* overlay_card = nullptr;
  lv_obj_t* overlay_title = nullptr;
  lv_obj_t* overlay_subtitle = nullptr;
  lv_obj_t* overlay_thirst = nullptr;  ///< Thirst row container (plant and soil pins only).
  lv_obj_t* overlay_verdict = nullptr;
  lv_obj_t* overlay_happy = nullptr;
  std::vector<lv_obj_t*> overlay_stat_label;
  std::vector<lv_obj_t*> overlay_stat_value;
  lv_obj_t* overlay_history_title = nullptr;
};

class Screen {
 public:
  /// Builds the tree under `parent` (normally lv_screen_active()), at the parent's origin, sized
  /// to config.canvas. `config` must outlive the Screen. `heat_image` describes the caller's
  /// RGB565 buffer (HeatRasterizer's size); the Screen shows it but never writes it.
  Screen(lv_obj_t* parent, const Config& config, const lv_image_dsc_t* heat_image, ScreenDeps deps);
  ~Screen();
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;

  /// Show `vm`. Only widgets whose shown state differs from the previous call are touched.
  void update(const ViewModel& vm, const HeaderText& header, Timestamp now,
              const SourceStatus& status);
  /// update() with the host's SourceStatus (all false).
  void update(const ViewModel& vm, const HeaderText& header, Timestamp now) {
    update(vm, header, now, SourceStatus{});
  }

  /// Tell LVGL the caller rewrote the heat image buffer.
  void invalidate_heat();

  /// One bounded unit of sprite preparation (capturing what lies under one pulse, a software
  /// render of about 112 x 112 px): call it from sliced work until it returns false. Pulses are
  /// not shown until their sprite is prepared; after a change under a sprite it shows its old
  /// background until prepared again. Never called from inside an LVGL event.
  bool prepare_step();
  PulseCounters pulse_counters() const;
  /// Every pulse tile LVGL or a presenter may show points at a whole, composed frame of its own
  /// size, equal to that frame composed again. False, with the first problem in `why`. For
  /// tests: walks and recomposes every tile.
  bool verify_pulse_frames(const char** why = nullptr);
  /// Pulse frame sets whose guard words have changed (a write outside a set), now. Cheap: a few
  /// words per set; the panel checks it once a second.
  std::uint32_t pulse_canary_failures() const;

  /// How the pulse frames chosen since the last call were shown.
  struct ShownFrames {
    std::uint32_t presented = 0;
    std::uint32_t invalidated = 0;
    /// Per PresentBlock value, how often it stopped a present.
    std::array<std::uint32_t, 16> blocked{};
  };
  /// With `deferred`, pulse frames are only chosen on LVGL's timer, and show_due_frames() shows
  /// them (D-058): call it from the main loop, outside LVGL callbacks. Without (the default),
  /// each frame is shown by invalidating its tile at once.
  void set_frames_deferred(bool deferred);
  /// Shows every pulse frame chosen since the last call through `presenter` (present_check() and
  /// show_tile_frame(); an invalidation where it cannot present). Adds to `tally` when given.
  void show_due_frames(Presenter& presenter, ShownFrames* tally = nullptr);

  /// The walls and labels have been rendered for baking (by prepare_step()).
  bool bake_ready() const;
  /// Lay the walls and labels over rows [row_begin, row_end) of a freshly rasterised heat image
  /// (the size of the map area, `stride` pixels per row). Returns false, writing nothing, until
  /// bake_ready(). Pure pixel work: safe from the embedder's sliced heat steps.
  bool bake_rows(std::uint16_t* image, std::int32_t stride, std::int32_t row_begin,
                 std::int32_t row_end) const;

  /// Counts LVGL property writes made by update(), for tests of the diffing (and a cheap
  /// "did anything change" signal for embedders).
  std::size_t writes() const;

  const ScreenWidgets& widgets() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hearthmap::ui
