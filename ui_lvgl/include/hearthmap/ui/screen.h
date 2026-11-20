// screen.h — the LVGL object tree for one hearthmap screen, driven by core's ViewModel.
//
// Construction builds everything that depends only on the config (rooms, walls, labels, one
// widget per pin, the HUD card pool, the overlay). update() then changes only what differs from
// the previous update: it compares what each widget would show and touches LVGL only where that
// changed. Nothing is rebuilt per update except the layer tabs, and those only when the set of
// enabled layers changes.
//
// The Screen draws; it does not decide (AGENTS.md principle 2). Its only inputs are the config,
// the view model, the header text and `now` (for "seen N min ago").
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
#include "hearthmap/view_model.h"
#include "lvgl.h"

namespace hearthmap::ui {

/// Text for the top bar, formatted by the embedder (it owns the clock and the time zone).
struct HeaderText {
  std::string clock;    ///< e.g. "09:00"
  std::string summary;  ///< e.g. "House 21.4°   52% RH   PM2.5 6"
};

struct ScreenDeps {
  Fonts fonts;
  EventSink* sink = nullptr;              ///< Required.
  const ImageProvider* photos = nullptr;  ///< Optional: nullptr draws every plant as a leaf.
};

/// Raw pointers to the widgets, for tests and for embedders that want to restyle. Owned by the
/// Screen; valid until it is destroyed (tab pointers until the tab set changes).
struct ScreenWidgets {
  lv_obj_t* root = nullptr;
  lv_obj_t* clock = nullptr;
  lv_obj_t* summary = nullptr;
  std::vector<lv_obj_t*> tabs;  ///< One per ViewModel::tabs entry, same order.
  lv_obj_t* map = nullptr;      ///< Click target for TapMap; at canvas.map_area.
  lv_obj_t* heat_image = nullptr;
  std::vector<lv_obj_t*> room_pulse;  ///< Per room, config order. Hidden when not pulsing.
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
  lv_obj_t* scrim = nullptr;  ///< The overlay: hidden when no pin is open.
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
  void update(const ViewModel& vm, const HeaderText& header, Timestamp now);

  /// Tell LVGL the caller rewrote the heat image buffer.
  void invalidate_heat();

  /// Counts LVGL property writes made by update(), for tests of the diffing (and a cheap
  /// "did anything change" signal for embedders).
  std::size_t writes() const;

  const ScreenWidgets& widgets() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hearthmap::ui
