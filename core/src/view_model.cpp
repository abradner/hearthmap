// view_model.cpp — hit-testing, UI state transitions and the view model (view_model.h).
#include "hearthmap/view_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "hearthmap/calibration.h"

namespace hearthmap {
namespace {

constexpr Role kAllRoles[] = {Role::Temperature, Role::Humidity,      Role::Moisture,
                              Role::Co2,         Role::Pm25,          Role::Voc,
                              Role::Battery,     Role::PredictedDryAt};
constexpr Layer kAllLayers[] = {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air};

std::optional<std::size_t> find_pin(const Config& config, const std::string& id) {
  for (std::size_t i = 0; i < config.pins.size(); ++i) {
    if (config.pins[i].id == id) return i;
  }
  return std::nullopt;
}

std::optional<std::size_t> find_room(const Config& config, const std::string& id) {
  for (std::size_t i = 0; i < config.rooms.size(); ++i) {
    if (config.rooms[i].id == id) return i;
  }
  return std::nullopt;
}

// The role through which `pin` feeds `layer`, if any. A layer is fed by exactly one role, and
// a pin binds each role at most once, so there is at most one such binding.
std::optional<Role> feeding_role(const Pin& pin, Layer layer) {
  for (const auto& b : pin.entities) {
    if (layer_of(b.role) == layer && feeds_layer(pin.kind, b.role)) return b.role;
  }
  return std::nullopt;
}

// An Assessment for a binding the pin should have but does not (only in an invalid config).
Assessment missing_assessment(Role role) {
  Assessment a;
  a.role = role;
  a.freshness = Freshness::Missing;
  a.severity = Severity::Stale;
  return a;
}

Layer shown_layer(const Config& config, Layer requested) {
  return config.layer(requested).enabled ? requested : config.default_layer;
}

// HUD candidacy (docs/decisions.md D-009 with ruling R2): a fresh plant whose thirst is past
// the dry edge of its comfort band and whose severity is Warn or Danger. The severity is
// classified here from the effective band rather than taken from the assessment, so the HUD
// does not depend on the active layer or on whether the soil layer is enabled.
std::optional<HudItem> hud_candidate(const Config& config, std::size_t index,
                                     const Assessment& primary) {
  const Pin& pin = config.pins[index];
  if (pin.kind != PinKind::Plant) return std::nullopt;
  if (primary.freshness != Freshness::Fresh || !primary.value || !primary.band) {
    return std::nullopt;
  }
  const double thirst_value = *primary.value;
  if (!(thirst_value > primary.band->comfort_max)) return std::nullopt;
  const Severity s = classify(*primary.band, thirst_value);
  if (s != Severity::Warn && s != Severity::Danger) return std::nullopt;
  return HudItem{index, thirst_value, s};
}

// Fills `hud` in place. The candidates are collected straight into hud.items, sorted, then cut
// to the cap; the storage is reserved for every Plant pin, so a warmed-up rebuild does not
// allocate whichever plants are thirsty.
void build_hud(const Config& config, const std::vector<PinView>& pins, Hud& hud) {
  hud.items.clear();
  // Every Plant pin is a possible candidate, and all candidates are collected before the cut
  // to max_items, so reserve for all of them: a no-op once warm, whatever the readings were
  // then, so plants turning thirsty later never allocates.
  std::size_t plants = 0;
  for (const auto& pin : config.pins) {
    if (pin.kind == PinKind::Plant) ++plants;
  }
  hud.items.reserve(plants);
  for (const auto& pv : pins) {
    if (auto item = hud_candidate(config, pv.pin_index, pv.primary)) hud.items.push_back(*item);
  }
  // Danger before Warn, then thirst descending, ties in config order (the order collected).
  // A stable insertion sort: no allocation (std::stable_sort may allocate a buffer, D-022), and
  // n is at most the 256 pins validate() allows.
  auto before = [](const HudItem& a, const HudItem& b) {
    if (rank(a.severity) != rank(b.severity)) return rank(a.severity) > rank(b.severity);
    return a.thirst > b.thirst;
  };
  for (std::size_t i = 1; i < hud.items.size(); ++i) {
    const HudItem item = hud.items[i];
    std::size_t j = i;
    for (; j > 0 && before(item, hud.items[j - 1]); --j) hud.items[j] = hud.items[j - 1];
    hud.items[j] = item;
  }
  const std::size_t cap =
      config.hud.max_items > 0 ? static_cast<std::size_t>(config.hud.max_items) : 0;
  const std::size_t shown = std::min(cap, hud.items.size());
  hud.overflow = hud.items.size() - shown;
  hud.items.resize(shown);  // shrinking keeps the capacity
}

// Fills `view` in place, reusing its rows' storage (and each row's unit string, reserved to
// kOverlayUnitCapacity when the row is created).
void build_overlay(const Config& config, std::size_t index, const Readings& readings, Timestamp now,
                   OverlayView& view) {
  const Pin& pin = config.pins[index];
  view.pin_index = index;
  std::size_t n = 0;
  // Rows in Role enum order, never binding order (D-019).
  for (Role role : kAllRoles) {
    if (role == Role::PredictedDryAt) continue;  // reserved, not shown in v1
    const EntityBinding* binding = find_binding(pin, role);
    if (binding == nullptr) continue;
    if (view.rows.size() <= n) {
      view.rows.emplace_back();
      view.rows.back().unit.reserve(kOverlayUnitCapacity);  // a unit arriving later fits
    }
    OverlayRow& row = view.rows[n++];
    row.assessment = assess(config, pin, role, readings, now).value_or(missing_assessment(role));
    if (const Reading* r = readings.find(binding->entity_id)) {
      row.unit.assign(r->unit);
      row.last_updated = r->last_updated;
    } else {
      row.unit.clear();
      row.last_updated.reset();
    }
  }
  view.rows.resize(n);
}

}  // namespace

HitResult hit_test(const Config& config, CanvasPoint point, double radius_px) {
  HitResult result;
  if (!contains(config.canvas.map_area, point)) return result;

  double best = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < config.pins.size(); ++i) {
    const CanvasPoint c = to_canvas(config.canvas.map_area, config.pins[i].position);
    const double d = std::hypot(c.x - point.x, c.y - point.y);
    if (d <= radius_px && d < best) {  // strict: the earlier pin keeps a tie
      best = d;
      result = HitResult{HitResult::Kind::Pin, i};
    }
  }
  if (result.kind == HitResult::Kind::Pin) return result;

  const MapPoint m = to_map(config.canvas.map_area, point);
  for (std::size_t i = 0; i < config.rooms.size(); ++i) {
    if (point_in_polygon(config.rooms[i].polygon, m)) return HitResult{HitResult::Kind::Room, i};
  }
  return result;
}

UiState apply(const Config& config, const UiState& state, const UiEvent& event) {
  UiState next = state;
  switch (event.type) {
    case UiEvent::Type::TapMap: {
      const HitResult hit = hit_test(config, event.point);
      if (hit.kind == HitResult::Kind::Pin) {
        const std::string& id = config.pins[hit.index].id;
        if (state.overlay_pin == id) {
          next.overlay_pin.reset();
        } else {
          next.overlay_pin = id;
        }
      } else {
        next.overlay_pin.reset();
      }
      break;
    }
    case UiEvent::Type::SelectLayer:
      if (config.layer(event.layer).enabled) next.active_layer = event.layer;
      break;
    case UiEvent::Type::TapHudItem:
      if (find_pin(config, event.pin_id)) next.overlay_pin = event.pin_id;
      break;
    case UiEvent::Type::CloseOverlay: next.overlay_pin.reset(); break;
  }
  return next;
}

HouseSummary house_summary(const Config& config, const Readings& readings, Timestamp now) {
  struct Mean {
    double sum = 0.0;
    int count = 0;
    std::optional<double> value() const {
      return count > 0 ? std::optional<double>(sum / count) : std::nullopt;
    }
  };
  Mean temp;
  Mean hum;
  Mean pm;
  for (const Pin& pin : config.pins) {
    bool indoor = false;
    for (const Room& r : config.rooms) {
      if (r.id == pin.room_id) {
        indoor = !r.outdoor;
        break;
      }
    }
    if (!indoor) continue;
    for (Role role : {Role::Temperature, Role::Humidity, Role::Pm25}) {
      if (role != Role::Pm25 && !feeds_layer(pin.kind, role)) continue;
      const std::optional<Assessment> a = assess(config, pin, role, readings, now);
      if (!a || a->freshness != Freshness::Fresh || !a->value) continue;
      Mean& m = role == Role::Temperature ? temp : role == Role::Humidity ? hum : pm;
      m.sum += *a->value;
      ++m.count;
    }
  }
  return HouseSummary{temp.value(), hum.value(), pm.value()};
}

void build_view_model(const Config& config, const Readings& readings, Timestamp now,
                      const UiState& ui_state, ViewModel& out) {
  out.summary = house_summary(config, readings, now);
  out.active_layer = shown_layer(config, ui_state.active_layer);
  const bool layer_enabled = config.layer(out.active_layer).enabled;
  out.tabs.clear();
  out.tabs.reserve(kLayerCount);  // a no-op once warm, whichever layers are enabled
  for (Layer l : kAllLayers) {
    if (config.layer(l).enabled) out.tabs.push_back(l);
  }

  out.pins.resize(config.pins.size());
  for (std::size_t i = 0; i < config.pins.size(); ++i) {
    const Pin& pin = config.pins[i];
    PinView& pv = out.pins[i];
    pv = PinView{};
    pv.pin_index = i;
    pv.position = to_canvas(config.canvas.map_area, pin.position);
    const Role primary = primary_role(pin.kind);
    pv.primary = assess(config, pin, primary, readings, now).value_or(missing_assessment(primary));
    pv.active_severity = pv.primary.severity;
    const std::optional<Role> role =
        layer_enabled ? feeding_role(pin, out.active_layer) : std::nullopt;
    if (role) {
      pv.on_active_layer = true;
      const Assessment a =
          (*role == primary) ? pv.primary : *assess(config, pin, *role, readings, now);
      pv.active_severity = a.severity;
      if (a.freshness == Freshness::Fresh) pv.active_value = a.value;
    }
  }

  out.rooms.resize(config.rooms.size());
  for (std::size_t r = 0; r < config.rooms.size(); ++r) {
    RoomView& rv = out.rooms[r];
    rv = RoomView{};
    rv.room_index = r;
    if (!layer_enabled) continue;
    for (const auto& pv : out.pins) {
      if (!pv.on_active_layer || config.pins[pv.pin_index].room_id != config.rooms[r].id) {
        continue;
      }
      // Stale counts as Ok for pulsing (D-008); only Warn and Danger raise the pulse.
      if (pv.active_severity == Severity::Warn || pv.active_severity == Severity::Danger) {
        rv.pulse = worse(rv.pulse, pv.active_severity);
      }
    }
  }

  build_hud(config, out.pins, out.hud);

  std::optional<std::size_t> overlay_index;
  if (ui_state.overlay_pin) overlay_index = find_pin(config, *ui_state.overlay_pin);
  if (overlay_index) {
    if (!out.overlay) out.overlay.emplace();
    build_overlay(config, *overlay_index, readings, now, *out.overlay);
  } else {
    out.overlay.reset();
  }
}

ViewModel build_view_model(const Config& config, const Readings& readings, Timestamp now,
                           const UiState& ui_state) {
  ViewModel vm;
  build_view_model(config, readings, now, ui_state, vm);
  return vm;
}

void heat_samples(const Config& config, const Readings& readings, Timestamp now, Layer layer,
                  std::vector<HeatSample>& out) {
  out.clear();
  if (!config.layer(layer).enabled) return;
  for (const auto& pin : config.pins) {
    const std::optional<Role> role = feeding_role(pin, layer);
    if (!role) continue;
    const std::optional<std::size_t> room = find_room(config, pin.room_id);
    if (!room || *room > static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max())) {
      continue;
    }
    const std::optional<Assessment> a = assess(config, pin, *role, readings, now);
    if (!a || a->freshness != Freshness::Fresh || !a->value) continue;
    HeatSample s;
    s.position = to_float(pin.position);
    s.room_index = static_cast<std::int16_t>(*room);
    s.value = static_cast<float>(*a->value);
    out.push_back(s);
  }
}

std::vector<HeatSample> heat_samples(const Config& config, const Readings& readings, Timestamp now,
                                     Layer layer) {
  std::vector<HeatSample> samples;
  heat_samples(config, readings, now, layer, samples);
  return samples;
}

}  // namespace hearthmap
