// ports.h — the two interfaces an embedder implements: where taps go, where photos come from.
#pragma once

#include <string_view>

#include "hearthmap/view_model.h"
#include "lvgl.h"

namespace hearthmap::ui {

/// Receives what the user did, already translated into core's vocabulary. The embedder feeds
/// each event to core's apply(), rebuilds the view model and calls Screen::update(). ui_lvgl
/// keeps no UI state of its own (docs/decisions.md D-013).
///
/// Called from inside an LVGL event callback. Implementations should queue the event and act on
/// it after LVGL returns (and must never destroy the Screen from inside the call).
///
/// What is sent (docs/decisions.md D-013, amended):
/// - overlay closed: a tap in the map area -> TapMap with the tap point in canvas pixels; a
///   layer tab -> SelectLayer; a HUD card -> TapHudItem with the plant's pin id;
/// - overlay open: a tap on the scrim -> CloseOverlay; a tap on the card -> nothing. The scrim
///   covers the whole screen, so no other event can be sent while the overlay is open.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void on_ui_event(const UiEvent& event) = 0;
};

/// Supplies plant photos by photo id (Pin::photo). Return nullptr when there is none; the UI
/// then draws the placeholder leaf. The returned descriptor must stay valid until the next call
/// for the same id or until the Screen is destroyed. Any LVGL colour format LVGL can draw works;
/// the UI scales it to cover the plant's disc.
class ImageProvider {
 public:
  virtual ~ImageProvider() = default;
  virtual const lv_image_dsc_t* plant_photo(std::string_view photo_id) const = 0;
};

}  // namespace hearthmap::ui
