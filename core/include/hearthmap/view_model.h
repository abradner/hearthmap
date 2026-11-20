// view_model.h — everything the screen shows, as a pure function of its inputs.
//
//   build_view_model(config, readings, now, ui_state) -> ViewModel
//   hit_test(config, canvas point)                    -> what a tap on the map landed on
//   apply(config, ui_state, event)                    -> the next ui_state
//
// ui_lvgl renders a ViewModel and diffs consecutive ones; it decides nothing itself. The same
// inputs always produce the same ViewModel, so the host renderer's golden images are a test of
// this file as much as of the drawing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hearthmap/config.h"
#include "hearthmap/geometry.h"
#include "hearthmap/heat_field.h"
#include "hearthmap/readings.h"
#include "hearthmap/severity.h"

namespace hearthmap {

// ---------------------------------------------------------------------------------------------
// UI state and events
// ---------------------------------------------------------------------------------------------

/// The only state the UI holds. Everything else is derived.
struct UiState {
  Layer active_layer = Layer::Temperature;
  /// The pin whose detail overlay is open, by pin id, or nullopt when none is.
  std::optional<std::string> overlay_pin;
};

/// The initial UiState for a config: default_layer, no overlay.
inline UiState initial_ui_state(const Config& config) {
  return UiState{config.default_layer, std::nullopt};
}

/// Something the user did. Construct with the named factories.
///
/// Tap routing (docs/decisions.md D-013): while an overlay is open it is modal. A tap on the
/// scrim (anywhere outside the overlay card) sends only CloseOverlay; a tap on the card sends
/// nothing (the card's own close control sends CloseOverlay). TapMap is sent only when no
/// overlay is open, so a tap through the scrim never also hits a pin underneath.
struct UiEvent {
  enum class Type : std::uint8_t {
    TapMap,        ///< A tap inside the map image, no overlay open; `point` in canvas pixels.
    SelectLayer,   ///< A layer tab was tapped; `layer`.
    TapHudItem,    ///< A HUD row was tapped; `pin_id`.
    CloseOverlay,  ///< The overlay's close control, or a tap on the scrim around it.
  };

  Type type = Type::CloseOverlay;
  CanvasPoint point;
  Layer layer = Layer::Temperature;
  std::string pin_id;

  static UiEvent tap_map(CanvasPoint p) { return UiEvent{Type::TapMap, p, {}, {}}; }
  static UiEvent select_layer(Layer l) { return UiEvent{Type::SelectLayer, {}, l, {}}; }
  static UiEvent tap_hud_item(std::string id) {
    return UiEvent{Type::TapHudItem, {}, {}, std::move(id)};
  }
  static UiEvent close_overlay() { return UiEvent{}; }
};

// ---------------------------------------------------------------------------------------------
// Hit-testing
// ---------------------------------------------------------------------------------------------

/// Pin touch target radius in canvas pixels: about 5 mm on a 10.1" 1280x800 panel.
inline constexpr double kDefaultPinHitRadiusPx = 32.0;

struct HitResult {
  enum class Kind : std::uint8_t { None, Pin, Room };
  Kind kind = Kind::None;
  std::size_t index = 0;  ///< Index into Config::pins or Config::rooms; meaningless for None.
};

/// What a tap at `point` (canvas pixels) lands on.
/// - Outside canvas.map_area: None.
/// - Else the pin with the smallest canvas-pixel distance <= radius_px; ties go to the pin
///   earlier in config order. Pins of every kind are hittable whichever layer is active.
/// - Else the first room in config order whose polygon contains the point (in map space).
/// - Else None.
/// Implemented in core (PR 4).
HitResult hit_test(const Config& config, CanvasPoint point,
                   double radius_px = kDefaultPinHitRadiusPx);

/// The next UiState. apply() does not rely on the routing rule above: it stays total.
/// - TapMap: hit a pin -> overlay_pin = that pin, unless it is already the open one, in which
///   case the overlay closes (a second tap toggles). Hit a room or nothing -> overlay closes.
/// - SelectLayer: active_layer = layer when that layer is enabled, else unchanged. The overlay
///   stays as it is.
/// - TapHudItem: overlay_pin = pin_id when it names a pin in config, else unchanged.
/// - CloseOverlay: overlay_pin = nullopt.
/// Implemented in core (PR 4).
UiState apply(const Config& config, const UiState& state, const UiEvent& event);

// ---------------------------------------------------------------------------------------------
// The view model
// ---------------------------------------------------------------------------------------------

struct PinView {
  std::size_t pin_index = 0;  ///< Into Config::pins; ViewModel::pins is in config order.
  CanvasPoint position;       ///< Pin centre in canvas pixels.
  /// The assessment of the pin's primary role (primary_role(kind)). Its severity is the pin's
  /// badge; for plant and soil pins its value is thirst.
  Assessment primary;
  /// True when the pin feeds the active layer through any binding (feeds_layer()). Pins that
  /// do not are drawn dimmed but stay tappable.
  bool on_active_layer = false;
  /// The value to print on the pin for the active layer: the assessed value of the binding
  /// that feeds the active layer, when on_active_layer and that binding is Fresh.
  std::optional<double> active_value;
  /// Severity for the active layer's binding when on_active_layer, else primary.severity.
  Severity active_severity = Severity::Stale;
};

struct RoomView {
  std::size_t room_index = 0;  ///< Into Config::rooms; ViewModel::rooms is in config order.
  /// Ok = no pulse; Warn or Danger = pulse at that level. The worst severity, by rank(), over
  /// the active-layer assessments of pins in this room that feed the active layer, with Stale
  /// counted as Ok (a silent sensor shows on its pin, it does not pulse a room). Never Stale.
  /// Disabled active layer: always Ok. (docs/decisions.md D-008.)
  Severity pulse = Severity::Ok;
};

struct HudItem {
  std::size_t pin_index = 0;           ///< Into Config::pins; always a Plant pin.
  double thirst = 0.0;                 ///< 0..1, the fresh primary value.
  Severity severity = Severity::Warn;  ///< Warn or Danger.
};

/// Plants that need water, worst first (docs/decisions.md D-009).
/// Candidates: Plant pins whose primary assessment is Fresh and on the dry side, where "dry
/// side" means s = classify(*primary.band, thirst) is Warn or Danger AND thirst >
/// primary.band->comfort_max (it does not refer to warn_above, which may be absent; a too-wet
/// plant is not listed). HudItem::severity is that s. Order: Danger before Warn, then thirst
/// descending, then config order. `items` holds at most config.hud.max_items; `overflow`
/// counts the candidates left out ("+N more"). The HUD is independent of the active layer and
/// of whether the soil layer is enabled: it classifies the band itself rather than reading
/// primary.severity, which is Ok while the soil layer is disabled (D-020).
struct Hud {
  std::vector<HudItem> items;
  std::size_t overflow = 0;
};

/// One row of the detail overlay: a bound role and what is known about it.
struct OverlayRow {
  Assessment assessment;                  ///< role, freshness, value, raw, severity.
  std::string unit;                       ///< From the reading when present, else empty.
  std::optional<Timestamp> last_updated;  ///< From the reading when present.
};

struct OverlayView {
  std::size_t pin_index = 0;
  /// One row per binding, ordered by the Role enum (Temperature, Humidity, Moisture, Co2, Pm25,
  /// Voc, Battery), never by binding or JSON key order, which a source need not preserve
  /// (D-019). PredictedDryAt is omitted (reserved, not shown in v1). Rows for roles whose
  /// layer is disabled still appear, with their values, unjudged (D-020).
  std::vector<OverlayRow> rows;
};

struct ViewModel {
  /// The layer actually shown: ui_state.active_layer when enabled, else config.default_layer
  /// (D-020). Never the requested layer when that one is disabled.
  Layer active_layer = Layer::Temperature;
  std::vector<Layer> tabs;  ///< Enabled layers, in Layer enum order.
  std::vector<PinView> pins;
  std::vector<RoomView> rooms;
  Hud hud;
  /// Present when ui_state.overlay_pin names a pin in config; an unknown id yields nullopt.
  std::optional<OverlayView> overlay;
};

/// Everything the screen shows for (config, readings, now, ui_state), written into `out`,
/// reusing its vectors' and strings' storage: once warmed up with the same config, a rebuild
/// does not allocate (D-022). If ui_state.active_layer is disabled, the view model uses
/// config.default_layer instead (ViewModel::active_layer says which).
/// Implemented in core (PR 4).
void build_view_model(const Config& config, const Readings& readings, Timestamp now,
                      const UiState& ui_state, ViewModel& out);

/// Returning form of build_view_model(); allocates.
/// Implemented in core (PR 4).
ViewModel build_view_model(const Config& config, const Readings& readings, Timestamp now,
                           const UiState& ui_state);

/// Heat samples for `layer`, written into `out` (cleared first, storage reused): one per
/// binding that feeds `layer` (feeds_layer()) and is Fresh, at the pin's position and room,
/// with the assessed value as float (thirst for the soil layer). Empty when `layer` is
/// disabled. Pins in outdoor rooms are included; compute_heat_field() ignores them, so the
/// outdoor rule lives in one place. Pins whose room id is unknown are skipped (validate()
/// makes that impossible in a usable config).
/// Implemented in core (PR 4).
void heat_samples(const Config& config, const Readings& readings, Timestamp now, Layer layer,
                  std::vector<HeatSample>& out);

/// Returning form of heat_samples(); allocates.
/// Implemented in core (PR 4).
std::vector<HeatSample> heat_samples(const Config& config, const Readings& readings, Timestamp now,
                                     Layer layer);

}  // namespace hearthmap
