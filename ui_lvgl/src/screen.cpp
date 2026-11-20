#include "hearthmap/ui/screen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hearthmap/geometry.h"
#include "hearthmap/ui/format.h"
#include "hearthmap/ui/theme.h"
#include "image_util.h"

namespace hearthmap::ui {

namespace {

using detail::LeafImage;

// ---- geometry of the spec ------------------------------------------------------------------

constexpr int kPlainDotRadius = 7;
constexpr int kPlainDotBorder = 3;
constexpr int kPlantRadius = 22;
constexpr int kPinRingWidth = 6;
constexpr int kPulseFrom = 26;
constexpr int kPulseTo = 52;
constexpr int kPulseWidth = 4;
constexpr std::uint32_t kPulsePeriodMs = 1600;
constexpr std::uint32_t kRoomPulseHalfPeriodMs = 1400;  // ~2.8 s full cycle
constexpr int kRoomPulseWidth = 5;
constexpr int kBadgeW = 28;
constexpr int kBadgeH = 25;
constexpr int kHudDiscRadius = 26;
constexpr int kHudTitleHeight = 40;
constexpr int kOverlayW = 660;
constexpr int kOverlayH = 600;
constexpr int kOverlayPad = 32;
constexpr int kOverlayDiscRadius = 64;
constexpr int kOverlayInnerW = kOverlayW - 2 * kOverlayPad;
constexpr int kBarH = 18;
constexpr int kStatColumns = 3;
constexpr std::size_t kMaxStats = 6;

lv_color_t hex(std::uint32_t c) {
  return lv_color_hex(c);
}

lv_coord_t px(double v) {
  return static_cast<lv_coord_t>(std::lround(v));
}

bool in_pot(PinKind kind) {
  return kind == PinKind::Plant || kind == PinKind::Soil;
}

Role role_for_layer(Layer layer) {
  switch (layer) {
    case Layer::Temperature: return Role::Temperature;
    case Layer::Humidity: return Role::Humidity;
    case Layer::Soil: return Role::Moisture;
    case Layer::Air: return Role::Co2;
  }
  return Role::Temperature;
}

// A plain object with no theme styling, not clickable, not scrollable.
lv_obj_t* make_box(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_clickable(o, false);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_click_focusable(o, false);
  return o;
}

// Let `obj`'s children draw up to `px` beyond its edges. In LVGL 9.6 overflow_visible alone
// clips children to the parent's extended draw area, which is 0 unless someone sets it.
void ext_draw_cb(lv_event_t* e) {
  lv_event_set_ext_draw_size(
      e, static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
}

void allow_overflow(lv_obj_t* obj, std::int32_t px) {
  lv_obj_set_overflow_visible(obj, true);
  lv_obj_add_event_cb(obj, ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE,
                      reinterpret_cast<void*>(static_cast<std::intptr_t>(px)));
  lv_obj_refresh_ext_draw_size(obj);
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, std::uint32_t colour) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_clickable(l, false);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, hex(colour), 0);
  lv_label_set_text(l, "");
  return l;
}

lv_obj_t* make_circle(lv_obj_t* parent, int radius, std::uint32_t fill) {
  lv_obj_t* o = make_box(parent);
  lv_obj_set_size(o, 2 * radius, 2 * radius);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, hex(fill), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

// The warn/danger badge: a filled triangle with "!" in it, drawn on demand, with a dark rim so
// it stays visible on heat of its own colour.
void badge_draw_cb(lv_event_t* e) {
  lv_obj_t* obj = lv_event_get_current_target_obj(e);
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  const std::int32_t mid = (a.x1 + a.x2 + 1) / 2;
  d.p[0].x = mid;
  d.p[0].y = a.y1;
  d.p[1].x = a.x2;
  d.p[1].y = a.y2;
  d.p[2].x = a.x1;
  d.p[2].y = a.y2;
  d.color = lv_color_hex(theme::kBackground);
  d.opa = LV_OPA_COVER;
  lv_draw_triangle(layer, &d);
  d.p[0].y = a.y1 + 4;
  d.p[1].x = a.x2 - 3;
  d.p[1].y = a.y2 - 2;
  d.p[2].x = a.x1 + 3;
  d.p[2].y = a.y2 - 2;
  d.color = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
  lv_draw_triangle(layer, &d);
}

void room_pulse_exec(void* var, std::int32_t v) {
  // Opacity 0.35..1 on an eased (sine-like) ramp.
  const std::int32_t opa = 89 + ((255 - 89) * v) / 1000;
  lv_obj_set_style_line_opa(static_cast<lv_obj_t*>(var), static_cast<lv_opa_t>(opa), 0);
}

void pin_pulse_exec(void* var, std::int32_t v) {
  auto* ring = static_cast<lv_obj_t*>(var);
  const std::int32_t r = kPulseFrom + ((kPulseTo - kPulseFrom) * v) / 1000;
  lv_obj_set_size(ring, 2 * r, 2 * r);
  lv_obj_set_style_border_opa(ring, static_cast<lv_opa_t>((255 * (1000 - v)) / 1000), 0);
}

// ---- what each widget shows, for diffing ----------------------------------------------------

struct PinLook {
  bool dim = false;
  bool stale = false;
  std::string value_text;
  std::uint32_t value_colour = 0;
  Severity badge = Severity::Ok;  // Ok: no badge
  bool pulse = false;
  int ring_permille = -1;  // -1: no arc
  std::uint32_t ring_colour = 0;
  bool operator==(const PinLook& o) const {
    return dim == o.dim && stale == o.stale && value_text == o.value_text &&
           value_colour == o.value_colour && badge == o.badge && pulse == o.pulse &&
           ring_permille == o.ring_permille && ring_colour == o.ring_colour;
  }
  bool operator!=(const PinLook& o) const { return !(*this == o); }
};

struct HudLook {
  struct Item {
    std::size_t pin = 0;
    int percent = 0;
    int permille = 0;
    std::uint32_t colour = 0;
    bool operator==(const Item& o) const {
      return pin == o.pin && percent == o.percent && permille == o.permille && colour == o.colour;
    }
  };
  std::vector<Item> items;
  std::size_t overflow = 0;
  bool operator==(const HudLook& o) const { return items == o.items && overflow == o.overflow; }
};

struct StatLook {
  std::string label;
  std::string value;
  std::uint32_t colour = 0;
  bool operator==(const StatLook& o) const {
    return label == o.label && value == o.value && colour == o.colour;
  }
};

struct OverlayLook {
  bool open = false;
  std::size_t pin = 0;
  std::string title;
  std::string subtitle;
  bool pot = false;
  bool stale = false;
  int ring_permille = -1;
  std::uint32_t status_colour = 0;
  std::string verdict;
  int bar_permille = -1;  // -1: no fill
  int happy_lo = -1;      // moisture percent; -1: no happy range
  int happy_hi = -1;
  std::vector<StatLook> stats;
  std::string history_title;
  bool operator==(const OverlayLook& o) const {
    return open == o.open && pin == o.pin && title == o.title && subtitle == o.subtitle &&
           pot == o.pot && stale == o.stale && ring_permille == o.ring_permille &&
           status_colour == o.status_colour && verdict == o.verdict &&
           bar_permille == o.bar_permille && happy_lo == o.happy_lo && happy_hi == o.happy_hi &&
           stats == o.stats && history_title == o.history_title;
  }
};

// A plant disc with its moisture ring: used on the map, in HUD cards and in the overlay.
struct Disc {
  lv_obj_t* holder = nullptr;  // 2r x 2r
  lv_obj_t* face = nullptr;    // the clipped circle
  lv_obj_t* image = nullptr;
  lv_obj_t* ring = nullptr;
};

Disc make_disc(lv_obj_t* parent, int radius, int ring_width) {
  Disc d;
  d.holder = make_box(parent);
  lv_obj_set_size(d.holder, 2 * radius, 2 * radius);
  d.face = make_circle(d.holder, radius, theme::kPlantDisc);
  lv_obj_set_style_clip_corner(d.face, true, 0);
  d.image = lv_image_create(d.face);
  lv_obj_remove_style_all(d.image);
  lv_obj_set_clickable(d.image, false);
  lv_obj_set_size(d.image, 2 * radius, 2 * radius);
  lv_obj_center(d.image);
  d.ring = lv_arc_create(d.holder);
  lv_obj_remove_style_all(d.ring);
  lv_obj_set_clickable(d.ring, false);
  lv_obj_set_size(d.ring, 2 * radius, 2 * radius);
  lv_arc_set_rotation(d.ring, 270);
  lv_arc_set_bg_angles(d.ring, 0, 360);
  lv_arc_set_range(d.ring, 0, 1000);
  lv_arc_set_value(d.ring, 0);
  lv_obj_set_style_arc_width(d.ring, ring_width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(d.ring, hex(theme::kTrack), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(d.ring, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_arc_width(d.ring, ring_width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(d.ring, true, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(d.ring, LV_OPA_COVER, LV_PART_INDICATOR);
  return d;
}

}  // namespace

// ------------------------------------------------------------------------------------------------

struct Screen::Impl {
  const Config& config;
  ScreenDeps deps;
  ScreenWidgets w;
  std::size_t writes = 0;

  LeafImage pin_leaf{30, theme::kLeaf};
  LeafImage pin_leaf_stale{30, theme::kLeafStale};
  LeafImage hud_leaf{36, theme::kLeaf};
  LeafImage hud_leaf_stale{36, theme::kLeafStale};
  LeafImage overlay_leaf{88, theme::kLeaf};
  LeafImage overlay_leaf_stale{88, theme::kLeafStale};

  std::vector<std::vector<lv_point_precise_t>> room_points;
  std::vector<Disc> pin_disc;  // per pin; holder == nullptr for plain pins
  std::vector<lv_obj_t*> pin_dot;
  std::vector<Disc> hud_disc;
  Disc overlay_disc;
  lv_obj_t* overlay_dot_face = nullptr;
  lv_obj_t* overlay_dot = nullptr;
  lv_obj_t* overlay_bar_fill = nullptr;
  lv_obj_t* overlay_bar_happy = nullptr;
  lv_obj_t* overlay_stats = nullptr;
  lv_obj_t* overlay_history = nullptr;
  lv_obj_t* tabs_box = nullptr;
  lv_obj_t* hud_title = nullptr;

  // Previous shown state. Empty optionals mean "never shown": the first update writes all.
  std::optional<std::string> clock_text;
  std::optional<std::string> summary_text;
  std::optional<std::vector<Layer>> tab_layers;
  std::optional<Layer> tab_active;
  std::vector<std::optional<Severity>> room_pulse;
  std::vector<std::optional<PinLook>> pin_look;
  std::optional<HudLook> hud_look;
  std::optional<OverlayLook> overlay_look;
  std::vector<std::string> hud_pin_ids;  // per card, for TapHudItem

  Impl(lv_obj_t* parent, const Config& cfg, const lv_image_dsc_t* heat, ScreenDeps d)
      : config(cfg), deps(std::move(d)) {
    build_root(parent);
    build_top_bar();
    build_map(heat);
    build_hud();
    build_overlay();
  }

  // ---- event plumbing -------------------------------------------------------------------------

  void send(const UiEvent& e) const {
    if (deps.sink != nullptr) deps.sink->on_ui_event(e);
  }

  static Impl* self(lv_event_t* e) { return static_cast<Impl*>(lv_event_get_user_data(e)); }

  static void on_map_click(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    self(e)->send(
        UiEvent::tap_map(CanvasPoint{static_cast<double>(p.x), static_cast<double>(p.y)}));
  }

  static void on_tab_click(lv_event_t* e) {
    auto* obj = lv_event_get_current_target_obj(e);
    const auto layer =
        static_cast<Layer>(reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(obj)));
    self(e)->send(UiEvent::select_layer(layer));
  }

  static void on_hud_click(lv_event_t* e) {
    Impl* s = self(e);
    auto* obj = lv_event_get_current_target_obj(e);
    const auto i = reinterpret_cast<std::uintptr_t>(lv_obj_get_user_data(obj));
    if (i < s->hud_pin_ids.size() && !s->hud_pin_ids[i].empty()) {
      s->send(UiEvent::tap_hud_item(s->hud_pin_ids[i]));
    }
  }

  static void on_scrim_click(lv_event_t* e) { self(e)->send(UiEvent::close_overlay()); }

  // ---- construction ---------------------------------------------------------------------------

  void build_root(lv_obj_t* parent) {
    w.root = make_box(parent);
    lv_obj_set_pos(w.root, 0, 0);
    lv_obj_set_size(w.root, config.canvas.width, config.canvas.height);
    lv_obj_set_style_bg_color(w.root, hex(theme::kBackground), 0);
    lv_obj_set_style_bg_opa(w.root, LV_OPA_COVER, 0);
  }

  void build_top_bar() {
    w.clock = make_label(w.root, deps.fonts.px30, theme::kTextPrimary);
    lv_obj_set_pos(w.clock, 24, 14);
    w.summary = make_label(w.root, deps.fonts.px22, theme::kTextSecondary);
    lv_obj_set_pos(w.summary, 24, 20);
    tabs_box = make_box(w.root);
    lv_obj_set_size(tabs_box, LV_SIZE_CONTENT, 44);
    lv_obj_set_flex_flow(tabs_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs_box, 12, 0);
    lv_obj_align(tabs_box, LV_ALIGN_TOP_RIGHT, -24, 10);
  }

  lv_point_precise_t map_px(MapPoint p) const {
    const CanvasRect& a = config.canvas.map_area;
    const CanvasPoint c = to_canvas(a, p);
    lv_point_precise_t out;
    out.x = px(c.x - a.x);
    out.y = px(c.y - a.y);
    return out;
  }

  void build_map(const lv_image_dsc_t* heat) {
    const CanvasRect& a = config.canvas.map_area;
    w.map = make_box(w.root);
    lv_obj_set_pos(w.map, a.x, a.y);
    lv_obj_set_size(w.map, a.width, a.height);
    allow_overflow(w.map, 160);
    lv_obj_set_clickable(w.map, true);
    lv_obj_add_event_cb(w.map, on_map_click, LV_EVENT_CLICKED, this);

    w.heat_image = lv_image_create(w.map);
    lv_obj_remove_style_all(w.heat_image);
    lv_obj_set_clickable(w.heat_image, false);
    lv_obj_set_pos(w.heat_image, 0, 0);
    if (heat != nullptr) lv_image_set_src(w.heat_image, heat);

    // Walls (indoor rooms; the outdoor fill comes from the heat image), then the pulse outlines
    // on top, so a pulsing room's outline covers its walls.
    room_points.resize(config.rooms.size());
    for (std::size_t r = 0; r < config.rooms.size(); ++r) {
      auto& pts = room_points[r];
      for (const auto& v : config.rooms[r].polygon) pts.push_back(map_px(v));
      if (!pts.empty()) pts.push_back(pts.front());
    }
    for (std::size_t r = 0; r < config.rooms.size(); ++r) {
      if (config.rooms[r].outdoor) continue;
      lv_obj_t* wall = lv_line_create(w.map);
      lv_obj_remove_style_all(wall);
      lv_obj_set_clickable(wall, false);
      lv_line_set_points(wall, room_points[r].data(),
                         static_cast<std::uint32_t>(room_points[r].size()));
      lv_obj_set_style_line_width(wall, 2, 0);
      lv_obj_set_style_line_color(wall, hex(theme::kWall), 0);
      lv_obj_set_style_line_opa(wall, LV_OPA_COVER, 0);
    }
    for (std::size_t r = 0; r < config.rooms.size(); ++r) {
      lv_obj_t* pulse = lv_line_create(w.map);
      lv_obj_remove_style_all(pulse);
      lv_obj_set_clickable(pulse, false);
      lv_line_set_points(pulse, room_points[r].data(),
                         static_cast<std::uint32_t>(room_points[r].size()));
      lv_obj_set_style_line_width(pulse, kRoomPulseWidth, 0);
      lv_obj_set_style_line_rounded(pulse, true, 0);
      lv_obj_set_style_line_opa(pulse, LV_OPA_COVER, 0);
      lv_obj_set_hidden(pulse, true);
      w.room_pulse.push_back(pulse);
    }
    for (std::size_t r = 0; r < config.rooms.size(); ++r) {
      lv_obj_t* label = make_label(w.map, deps.fonts.px20, theme::kTextSecondary);
      lv_label_set_text(label, config.rooms[r].name.c_str());
      // Top-left inside the room: the vertex nearest the map's top-left corner.
      lv_point_precise_t best{0, 0};
      bool first = true;
      for (const auto& v : config.rooms[r].polygon) {
        const lv_point_precise_t p = map_px(v);
        if (first || p.x + p.y < best.x + best.y) best = p;
        first = false;
      }
      lv_obj_set_pos(label, best.x + 12, best.y + 8);
      w.room_label.push_back(label);
    }
    room_pulse.assign(config.rooms.size(), std::nullopt);

    // Danger pulses below every pin, then the pins.
    w.pin_pulse.assign(config.pins.size(), nullptr);
    for (std::size_t i = 0; i < config.pins.size(); ++i) {
      const lv_point_precise_t c = map_px(config.pins[i].position);
      lv_obj_t* holder = make_box(w.map);
      allow_overflow(holder, 8);
      lv_obj_set_size(holder, 2 * kPulseTo, 2 * kPulseTo);
      lv_obj_set_pos(holder, c.x - kPulseTo, c.y - kPulseTo);
      lv_obj_t* ring = make_box(holder);
      lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(ring, kPulseWidth, 0);
      lv_obj_set_style_border_color(ring, hex(theme::kDanger), 0);
      lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
      lv_obj_set_size(ring, 2 * kPulseFrom, 2 * kPulseFrom);
      lv_obj_set_align(ring, LV_ALIGN_CENTER);
      lv_obj_set_hidden(ring, true);
      w.pin_pulse[i] = ring;
    }
    pin_disc.assign(config.pins.size(), Disc{});
    pin_dot.assign(config.pins.size(), nullptr);
    w.pin.assign(config.pins.size(), nullptr);
    w.pin_value.assign(config.pins.size(), nullptr);
    w.pin_badge.assign(config.pins.size(), nullptr);
    w.pin_ring.assign(config.pins.size(), nullptr);
    pin_look.assign(config.pins.size(), std::nullopt);
    for (std::size_t i = 0; i < config.pins.size(); ++i) build_pin(i);
  }

  lv_obj_t* make_badge(lv_obj_t* parent) {
    lv_obj_t* b = make_box(parent);
    lv_obj_set_size(b, kBadgeW, kBadgeH);
    lv_obj_add_event_cb(b, badge_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_t* mark = make_label(b, deps.fonts.px20, theme::kBackground);
    lv_label_set_text(mark, "!");
    lv_obj_align(mark, LV_ALIGN_CENTER, 0, 3);
    lv_obj_set_hidden(b, true);
    return b;
  }

  void build_pin(std::size_t i) {
    const Pin& pin = config.pins[i];
    const lv_point_precise_t c = map_px(pin.position);
    if (in_pot(pin.kind)) {
      lv_obj_t* box = make_box(w.map);
      allow_overflow(box, 140);
      lv_obj_set_size(box, 2 * kPlantRadius, 2 * kPlantRadius);
      lv_obj_set_pos(box, c.x - kPlantRadius, c.y - kPlantRadius);
      Disc d = make_disc(box, kPlantRadius, kPinRingWidth);
      if (pin.kind == PinKind::Plant) {
        set_disc_image(d, photo_for(pin), &pin_leaf);
      }
      lv_obj_t* badge = make_badge(box);
      lv_obj_set_pos(badge, 2 * kPlantRadius - kBadgeW / 2, -kBadgeH / 2);
      w.pin[i] = box;
      w.pin_badge[i] = badge;
      w.pin_ring[i] = d.ring;
      pin_disc[i] = d;
    } else {
      constexpr int r = kPlainDotRadius + kPlainDotBorder;
      lv_obj_t* box = make_box(w.map);
      allow_overflow(box, 140);
      lv_obj_set_size(box, 2 * r, 2 * r);
      lv_obj_set_pos(box, c.x - r, c.y - r);
      lv_obj_t* dot = make_circle(box, r, theme::kTextPrimary);
      lv_obj_set_style_border_width(dot, kPlainDotBorder, 0);
      lv_obj_set_style_border_color(dot, hex(theme::kBackground), 0);
      lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
      lv_obj_t* value = make_label(box, deps.fonts.px22, theme::kTextPrimary);
      // A dark backing so the value reads on any heat colour, including its own status colour.
      lv_obj_set_style_bg_color(value, hex(theme::kBackground), 0);
      lv_obj_set_style_bg_opa(value, LV_OPA_60, 0);
      lv_obj_set_style_radius(value, 6, 0);
      lv_obj_set_style_pad_hor(value, 6, 0);
      lv_obj_set_style_pad_ver(value, 1, 0);
      lv_obj_align(value, LV_ALIGN_LEFT_MID, 2 * r + 2, 0);
      lv_obj_t* badge = make_badge(box);
      lv_obj_set_pos(badge, 2, -kBadgeH - 8);
      w.pin[i] = box;
      w.pin_value[i] = value;
      w.pin_badge[i] = badge;
      pin_dot[i] = dot;
    }
  }

  const lv_image_dsc_t* photo_for(const Pin& pin) const {
    if (deps.photos == nullptr || !pin.photo.has_value()) return nullptr;
    return deps.photos->plant_photo(*pin.photo);
  }

  static void set_disc_image(Disc& d, const lv_image_dsc_t* photo, const LeafImage* leaf) {
    if (photo != nullptr) {
      lv_image_set_src(d.image, photo);
      lv_image_set_inner_align(d.image, LV_IMAGE_ALIGN_COVER);
    } else if (leaf != nullptr) {
      lv_image_set_src(d.image, leaf->dsc());
      lv_image_set_inner_align(d.image, LV_IMAGE_ALIGN_CENTER);
    } else {
      lv_image_set_src(d.image, nullptr);
    }
  }

  void build_hud() {
    const Fonts& f = deps.fonts;
    hud_title = make_label(w.root, f.px24, theme::kTextPrimary);
    lv_label_set_text(hud_title, "Thirsty plants");
    lv_obj_set_pos(hud_title, theme::kHudX, theme::kHudY);
    w.hud_count = make_label(w.root, f.px24, theme::kDanger);
    lv_obj_align_to(w.hud_count, hud_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    w.hud_empty = make_label(w.root, f.px20, theme::kTextMuted);
    lv_label_set_text(w.hud_empty, "Nothing needs water");
    lv_obj_set_pos(w.hud_empty, theme::kHudX, theme::kHudY + kHudTitleHeight);
    lv_obj_set_hidden(w.hud_empty, true);

    const auto cap = static_cast<std::size_t>(std::max<std::int32_t>(0, config.hud.max_items));
    hud_pin_ids.assign(cap, std::string());
    for (std::size_t i = 0; i < cap; ++i) {
      lv_obj_t* card = make_box(w.root);
      lv_obj_set_size(card, theme::kHudWidth, theme::kHudCardHeight);
      lv_obj_set_pos(card, theme::kHudX,
                     theme::kHudY + kHudTitleHeight +
                         static_cast<int>(i) * (theme::kHudCardHeight + theme::kHudCardGap));
      lv_obj_set_style_radius(card, 12, 0);
      lv_obj_set_style_bg_color(card, hex(theme::kCard), 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      lv_obj_set_clickable(card, true);
      lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<std::uintptr_t>(i)));
      lv_obj_add_event_cb(card, on_hud_click, LV_EVENT_CLICKED, this);
      Disc d = make_disc(card, kHudDiscRadius, kPinRingWidth);
      lv_obj_set_pos(d.holder, 16, (theme::kHudCardHeight - 2 * kHudDiscRadius) / 2);
      lv_obj_t* name = make_label(card, f.px22, theme::kTextPrimary);
      lv_obj_set_pos(name, 84, 13);
      lv_obj_set_width(name, 160);
      lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_t* room = make_label(card, f.px20, theme::kTextSecondary);
      lv_obj_set_pos(room, 84, 45);
      lv_obj_set_width(room, 160);
      lv_label_set_long_mode(room, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_t* pct = make_label(card, f.px28, theme::kTextPrimary);
      lv_obj_align(pct, LV_ALIGN_RIGHT_MID, -18, 0);
      lv_obj_set_hidden(card, true);
      w.hud_card.push_back(card);
      w.hud_name.push_back(name);
      w.hud_room.push_back(room);
      w.hud_percent.push_back(pct);
      hud_disc.push_back(d);
    }
    w.hud_more = make_label(w.root, f.px20, theme::kTextSecondary);
    lv_obj_set_hidden(w.hud_more, true);
    lv_obj_set_pos(w.hud_more, theme::kHudX + 4,
                   theme::kHudY + kHudTitleHeight +
                       static_cast<int>(cap) * (theme::kHudCardHeight + theme::kHudCardGap) + 2);
  }

  void build_overlay() {
    const Fonts& f = deps.fonts;
    w.scrim = make_box(w.root);
    lv_obj_set_pos(w.scrim, 0, 0);
    lv_obj_set_size(w.scrim, config.canvas.width, config.canvas.height);
    lv_obj_set_style_bg_color(w.scrim, hex(theme::kScrim), 0);
    lv_obj_set_style_bg_opa(w.scrim, theme::kScrimOpa, 0);
    lv_obj_set_clickable(w.scrim, true);
    lv_obj_add_event_cb(w.scrim, on_scrim_click, LV_EVENT_CLICKED, this);
    lv_obj_set_hidden(w.scrim, true);

    lv_obj_t* card = make_box(w.scrim);
    w.overlay_card = card;
    lv_obj_set_size(card, kOverlayW, kOverlayH);
    lv_obj_set_pos(card, (config.canvas.width - kOverlayW) / 2,
                   (config.canvas.height - kOverlayH) / 2);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_bg_color(card, hex(theme::kCard), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, hex(theme::kCardStroke), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    // Clickable with no handler: a tap on the card is absorbed (sends nothing).
    lv_obj_set_clickable(card, true);

    lv_obj_t* hint = make_label(w.scrim, f.px20, theme::kTextMuted);
    lv_label_set_text(hint, "Tap outside to close");
    lv_obj_align_to(hint, card, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);

    // Header
    overlay_disc = make_disc(card, kOverlayDiscRadius, 8);
    lv_obj_set_pos(overlay_disc.holder, kOverlayPad, kOverlayPad);
    overlay_dot_face = make_circle(card, kOverlayDiscRadius, theme::kTabInactive);
    lv_obj_set_pos(overlay_dot_face, kOverlayPad, kOverlayPad);
    overlay_dot = make_circle(overlay_dot_face, 16, theme::kTextPrimary);
    lv_obj_center(overlay_dot);
    const int text_x = kOverlayPad + 2 * kOverlayDiscRadius + 28;
    w.overlay_title = make_label(card, f.px34, theme::kTextPrimary);
    lv_obj_set_pos(w.overlay_title, text_x, kOverlayPad + 22);
    lv_obj_set_width(w.overlay_title, kOverlayW - text_x - kOverlayPad);
    lv_label_set_long_mode(w.overlay_title, LV_LABEL_LONG_MODE_DOTS);
    w.overlay_subtitle = make_label(card, f.px22, theme::kTextSecondary);
    lv_obj_set_pos(w.overlay_subtitle, text_x, kOverlayPad + 70);

    // Thirst row: label, verdict, bar, scale labels.
    const int row_y = kOverlayPad + 2 * kOverlayDiscRadius + 28;
    w.overlay_thirst = make_box(card);
    allow_overflow(w.overlay_thirst, 40);
    lv_obj_set_pos(w.overlay_thirst, kOverlayPad, row_y);
    lv_obj_set_size(w.overlay_thirst, kOverlayInnerW, 100);
    lv_obj_t* tlabel = make_label(w.overlay_thirst, f.px22, theme::kTextPrimary);
    lv_label_set_text(tlabel, "Soil moisture");
    lv_obj_set_pos(tlabel, 0, 4);
    w.overlay_verdict = make_label(w.overlay_thirst, f.px26, theme::kTextPrimary);
    lv_obj_align(w.overlay_verdict, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t* track = make_box(w.overlay_thirst);
    lv_obj_set_pos(track, 0, 44);
    lv_obj_set_size(track, kOverlayInnerW, kBarH);
    lv_obj_set_style_radius(track, kBarH / 2, 0);
    lv_obj_set_style_bg_color(track, hex(theme::kTrack), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    overlay_bar_happy = make_box(track);
    lv_obj_set_style_bg_color(overlay_bar_happy, hex(theme::kOk), 0);
    lv_obj_set_style_bg_opa(overlay_bar_happy, theme::kHappyRangeOpa, 0);
    lv_obj_set_height(overlay_bar_happy, kBarH);
    overlay_bar_fill = make_box(track);
    lv_obj_set_style_radius(overlay_bar_fill, kBarH / 2, 0);
    lv_obj_set_style_bg_opa(overlay_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_height(overlay_bar_fill, kBarH);
    lv_obj_t* dry = make_label(w.overlay_thirst, f.px20, theme::kTextSecondary);
    lv_label_set_text(dry, "dry");
    lv_obj_set_pos(dry, 0, 70);
    w.overlay_happy = make_label(w.overlay_thirst, f.px20, theme::kTextSecondary);
    lv_obj_align(w.overlay_happy, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_t* wet = make_label(w.overlay_thirst, f.px20, theme::kTextSecondary);
    lv_label_set_text(wet, "wet");
    lv_obj_align(wet, LV_ALIGN_TOP_RIGHT, 0, 70);

    // Stats grid: up to two rows of three.
    overlay_stats = make_box(card);
    lv_obj_set_size(overlay_stats, kOverlayInnerW, 140);
    for (std::size_t i = 0; i < kMaxStats; ++i) {
      const int col = static_cast<int>(i) % kStatColumns;
      const int row = static_cast<int>(i) / kStatColumns;
      const int x = col * (kOverlayInnerW / kStatColumns);
      const int y = row * 70;
      lv_obj_t* l = make_label(overlay_stats, f.px20, theme::kTextSecondary);
      lv_obj_set_pos(l, x, y);
      lv_obj_t* v = make_label(overlay_stats, f.px30, theme::kTextPrimary);
      lv_obj_set_pos(v, x, y + 26);
      w.overlay_stat_label.push_back(l);
      w.overlay_stat_value.push_back(v);
    }

    // History (no source of history exists yet: the section says so).
    overlay_history = make_box(card);
    lv_obj_set_size(overlay_history, kOverlayInnerW, 110);
    lv_obj_set_pos(overlay_history, kOverlayPad, kOverlayH - kOverlayPad - 110);
    w.overlay_history_title = make_label(overlay_history, f.px20, theme::kTextSecondary);
    lv_obj_set_pos(w.overlay_history_title, 0, 0);
    lv_obj_t* baseline = make_box(overlay_history);
    lv_obj_set_pos(baseline, 0, 108);
    lv_obj_set_size(baseline, kOverlayInnerW, 1);
    lv_obj_set_style_bg_color(baseline, hex(theme::kTrack), 0);
    lv_obj_set_style_bg_opa(baseline, LV_OPA_COVER, 0);
    lv_obj_t* none = make_label(overlay_history, f.px20, theme::kTextMuted);
    lv_label_set_text(none, "No history yet");
    lv_obj_align(none, LV_ALIGN_CENTER, 0, 14);
  }

  // ---- update ---------------------------------------------------------------------------------

  void count() { ++writes; }

  void set_text(lv_obj_t* label, const std::string& text) {
    lv_label_set_text(label, text.c_str());
    count();
  }

  void set_hidden(lv_obj_t* obj, bool hidden) {
    if (hidden) {
      lv_obj_set_hidden(obj, true);
    } else {
      lv_obj_set_hidden(obj, false);
    }
    count();
  }

  void set_ring(const Disc& d, int permille, std::uint32_t colour) {
    if (permille < 0) {
      lv_obj_set_style_arc_opa(d.ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    } else {
      lv_obj_set_style_arc_opa(d.ring, LV_OPA_COVER, LV_PART_INDICATOR);
      lv_obj_set_style_arc_color(d.ring, hex(colour), LV_PART_INDICATOR);
      lv_arc_set_value(d.ring, permille);
    }
    count();
  }

  void update_header(const HeaderText& h) {
    const bool clock_changed = clock_text != h.clock;
    if (clock_changed) {
      set_text(w.clock, h.clock);
      clock_text = h.clock;
    }
    if (summary_text != h.summary) {
      set_text(w.summary, h.summary);
      summary_text = h.summary;
    }
    if (clock_changed) {
      // The summary follows the clock, whose width depends on its text.
      lv_obj_update_layout(w.clock);
      lv_obj_align_to(w.summary, w.clock, LV_ALIGN_OUT_RIGHT_MID, 28, 2);
    }
  }

  void update_tabs(const ViewModel& vm) {
    if (tab_layers != vm.tabs) {
      for (lv_obj_t* t : w.tabs) lv_obj_delete(t);
      w.tabs.clear();
      for (Layer layer : vm.tabs) {
        lv_obj_t* pill = make_box(tabs_box);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, 44);
        lv_obj_set_style_radius(pill, 22, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pill, 22, 0);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_clickable(pill, true);
        lv_obj_set_user_data(pill,
                             reinterpret_cast<void*>(static_cast<std::uintptr_t>(index_of(layer))));
        lv_obj_add_event_cb(pill, on_tab_click, LV_EVENT_CLICKED, this);
        lv_obj_t* label = make_label(pill, deps.fonts.px22, theme::kTextSecondary);
        lv_label_set_text(label, layer_label(layer).c_str());
        w.tabs.push_back(pill);
        count();
      }
      tab_layers = vm.tabs;
      tab_active.reset();
    }
    if (tab_active != vm.active_layer) {
      for (std::size_t i = 0; i < w.tabs.size(); ++i) {
        const bool active = (*tab_layers)[i] == vm.active_layer;
        lv_obj_set_style_bg_color(w.tabs[i],
                                  hex(active ? theme::kTextPrimary : theme::kTabInactive), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(w.tabs[i], 0),
                                    hex(active ? theme::kBackground : theme::kTextSecondary), 0);
        count();
      }
      tab_active = vm.active_layer;
    }
  }

  void update_rooms(const ViewModel& vm) {
    for (const RoomView& rv : vm.rooms) {
      if (rv.room_index >= w.room_pulse.size()) continue;
      const Severity sev = rv.pulse == Severity::Stale ? Severity::Ok : rv.pulse;
      auto& prev = room_pulse[rv.room_index];
      if (prev == sev) continue;
      lv_obj_t* line = w.room_pulse[rv.room_index];
      lv_anim_delete(line, room_pulse_exec);
      if (sev == Severity::Ok) {
        set_hidden(line, true);
      } else {
        lv_obj_set_style_line_color(line, hex(severity_colour(sev)), 0);
        set_hidden(line, false);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, line);
        lv_anim_set_exec_cb(&a, room_pulse_exec);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, kRoomPulseHalfPeriodMs);
        lv_anim_set_reverse_duration(&a, kRoomPulseHalfPeriodMs);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
      }
      prev = sev;
    }
  }

  PinLook look_of(const PinView& pv, Layer layer) const {
    const Pin& pin = config.pins[pv.pin_index];
    PinLook l;
    l.dim = !pv.on_active_layer;
    l.badge = (pv.active_severity == Severity::Warn || pv.active_severity == Severity::Danger)
                  ? pv.active_severity
                  : Severity::Ok;
    l.pulse = pv.on_active_layer && pv.active_severity == Severity::Danger;
    if (in_pot(pin.kind)) {
      const bool fresh = pv.primary.freshness == Freshness::Fresh && pv.primary.value.has_value();
      l.stale = !fresh;
      if (fresh) {
        l.ring_permille = ring_permille(*pv.primary.value);
        l.ring_colour = severity_colour(pv.primary.severity);
      }
    } else if (pv.on_active_layer) {
      if (pv.active_value.has_value()) {
        l.value_text = format_value(role_for_layer(layer), *pv.active_value);
        l.value_colour = l.badge == Severity::Ok ? theme::kTextPrimary : severity_colour(l.badge);
      } else {
        l.stale = true;
        l.value_text = "no data";
        l.value_colour = theme::kStale;
      }
    }
    return l;
  }

  void update_pins(const ViewModel& vm) {
    for (const PinView& pv : vm.pins) {
      const std::size_t i = pv.pin_index;
      if (i >= config.pins.size()) continue;
      const PinLook look = look_of(pv, vm.active_layer);
      auto& prev = pin_look[i];
      if (prev.has_value() && *prev == look) continue;
      const bool first = !prev.has_value();
      if (first || prev->dim != look.dim) {
        lv_obj_set_style_opa(w.pin[i], look.dim ? LV_OPA_40 : LV_OPA_COVER, 0);
        count();
      }
      if (first || prev->badge != look.badge) {
        if (look.badge != Severity::Ok) {
          lv_obj_set_style_bg_color(w.pin_badge[i], hex(severity_colour(look.badge)), 0);
        }
        set_hidden(w.pin_badge[i], look.badge == Severity::Ok);
      }
      if (first || prev->pulse != look.pulse) {
        lv_obj_t* ring = w.pin_pulse[i];
        lv_anim_delete(ring, pin_pulse_exec);
        set_hidden(ring, !look.pulse);
        if (look.pulse) {
          lv_anim_t a;
          lv_anim_init(&a);
          lv_anim_set_var(&a, ring);
          lv_anim_set_exec_cb(&a, pin_pulse_exec);
          lv_anim_set_values(&a, 0, 1000);
          lv_anim_set_duration(&a, kPulsePeriodMs);
          lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
          lv_anim_set_path_cb(&a, lv_anim_path_linear);
          lv_anim_set_early_apply(&a, true);
          lv_anim_start(&a);
        }
      }
      const Pin& pin = config.pins[i];
      if (in_pot(pin.kind)) {
        Disc& d = pin_disc[i];
        if (first || prev->ring_permille != look.ring_permille ||
            prev->ring_colour != look.ring_colour) {
          set_ring(d, look.ring_permille, look.ring_colour);
        }
        if (pin.kind == PinKind::Plant && (first || prev->stale != look.stale)) {
          const lv_image_dsc_t* photo = photo_for(pin);
          if (photo == nullptr) {
            set_disc_image(d, nullptr, look.stale ? &pin_leaf_stale : &pin_leaf);
            count();
          }
        }
      } else {
        if (first || prev->value_text != look.value_text) {
          set_text(w.pin_value[i], look.value_text);
          if (first || prev->value_text.empty() != look.value_text.empty()) {
            set_hidden(w.pin_value[i], look.value_text.empty());  // no empty backing pill
          }
        }
        if (first || prev->value_colour != look.value_colour) {
          lv_obj_set_style_text_color(w.pin_value[i], hex(look.value_colour), 0);
          count();
        }
        if (first || prev->stale != look.stale) {
          lv_obj_set_style_bg_color(pin_dot[i],
                                    hex(look.stale ? theme::kStale : theme::kTextPrimary), 0);
          count();
        }
      }
      prev = look;
    }
  }

  const std::string& room_name(const std::string& id) const {
    static const std::string kEmpty;
    for (const auto& r : config.rooms) {
      if (r.id == id) return r.name;
    }
    return kEmpty;
  }

  void update_hud(const ViewModel& vm) {
    HudLook look;
    for (const HudItem& item : vm.hud.items) {
      HudLook::Item li;
      li.pin = item.pin_index;
      li.percent = moisture_percent(item.thirst);
      li.permille = ring_permille(item.thirst);
      li.colour = severity_colour(item.severity);
      look.items.push_back(li);
    }
    look.overflow = vm.hud.overflow;
    if (hud_look.has_value() && *hud_look == look) return;

    const std::size_t total = look.items.size() + look.overflow;
    set_text(w.hud_count, std::to_string(total));
    lv_obj_align_to(w.hud_count, hud_title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    set_hidden(w.hud_count, total == 0);
    set_hidden(w.hud_empty, total != 0);
    for (std::size_t c = 0; c < w.hud_card.size(); ++c) {
      const bool used = c < look.items.size() && look.items[c].pin < config.pins.size();
      const bool was_same = hud_look.has_value() && c < hud_look->items.size() && used &&
                            hud_look->items[c] == look.items[c];
      if (!used) {
        hud_pin_ids[c].clear();
        set_hidden(w.hud_card[c], true);
        continue;
      }
      if (was_same) continue;
      const HudLook::Item& li = look.items[c];
      const Pin& pin = config.pins[li.pin];
      hud_pin_ids[c] = pin.id;
      set_text(w.hud_name[c], pin.name);
      set_text(w.hud_room[c], room_name(pin.room_id));
      set_text(w.hud_percent[c], std::to_string(li.percent) + "%");
      lv_obj_set_style_text_color(w.hud_percent[c], hex(li.colour), 0);
      set_ring(hud_disc[c], li.permille, li.colour);
      set_disc_image(hud_disc[c], photo_for(pin), &hud_leaf);
      set_hidden(w.hud_card[c], false);
    }
    if (look.overflow > 0) set_text(w.hud_more, "+" + std::to_string(look.overflow) + " more");
    set_hidden(w.hud_more, look.overflow == 0);
    hud_look = std::move(look);
  }

  static const OverlayRow* find_row(const OverlayView& ov, Role role) {
    for (const auto& r : ov.rows) {
      if (r.assessment.role == role) return &r;
    }
    return nullptr;
  }

  static StatLook stat_of(const OverlayRow* row, Role role, PinKind kind) {
    StatLook s;
    s.label = role_label(role, kind);
    if (row == nullptr) {
      s.value = "none";
      s.colour = theme::kTextMuted;
    } else if (row->assessment.freshness == Freshness::Fresh && row->assessment.value) {
      s.value = format_value(role, *row->assessment.value);
      const Severity sev = row->assessment.severity;
      s.colour = (sev == Severity::Warn || sev == Severity::Danger) ? severity_colour(sev)
                                                                    : theme::kTextPrimary;
    } else {
      s.value = "no data";
      s.colour = theme::kStale;
    }
    return s;
  }

  OverlayLook overlay_of(const ViewModel& vm, Timestamp now) const {
    OverlayLook o;
    if (!vm.overlay.has_value() || vm.overlay->pin_index >= config.pins.size()) return o;
    const OverlayView& ov = *vm.overlay;
    const Pin& pin = config.pins[ov.pin_index];
    o.open = true;
    o.pin = ov.pin_index;
    o.title = pin.name;
    o.pot = in_pot(pin.kind);
    const Role primary = primary_role(pin.kind);
    const OverlayRow* prow = find_row(ov, primary);
    std::string seen = "never seen";
    if (prow != nullptr && prow->last_updated.has_value()) {
      seen = format_seen(now - *prow->last_updated);
    }
    o.subtitle = room_name(pin.room_id) + "   " + seen;
    const bool fresh = prow != nullptr && prow->assessment.freshness == Freshness::Fresh &&
                       prow->assessment.value.has_value();
    o.stale = !fresh;
    if (o.pot) {
      if (prow != nullptr) {
        o.verdict = thirst_verdict(prow->assessment);
        o.status_colour = severity_colour(prow->assessment.severity);
        if (fresh) {
          o.ring_permille = ring_permille(*prow->assessment.value);
          o.bar_permille =
              1000 - std::min(1000, std::max(0, static_cast<int>(std::lround(
                                                    *prow->assessment.value * 1000.0))));
        }
        if (prow->assessment.band.has_value()) {
          o.happy_lo = moisture_percent(prow->assessment.band->comfort_max);
          o.happy_hi = moisture_percent(prow->assessment.band->comfort_min);
        }
      } else {
        o.verdict = "no data";
        o.status_colour = theme::kStale;
      }
      for (Role r : {Role::Temperature, Role::Humidity, Role::Battery}) {
        o.stats.push_back(stat_of(find_row(ov, r), r, pin.kind));
      }
      o.history_title = "Soil moisture, 7 days";
    } else {
      for (const auto& row : ov.rows) {
        if (o.stats.size() >= kMaxStats) break;
        o.stats.push_back(stat_of(&row, row.assessment.role, pin.kind));
      }
      o.history_title = role_label(primary, pin.kind) + ", 7 days";
    }
    return o;
  }

  void update_overlay(const ViewModel& vm, Timestamp now) {
    OverlayLook o = overlay_of(vm, now);
    if (overlay_look.has_value() && *overlay_look == o) return;
    set_hidden(w.scrim, !o.open);
    if (o.open) {
      const Pin& pin = config.pins[o.pin];
      set_text(w.overlay_title, o.title);
      set_text(w.overlay_subtitle, o.subtitle);
      set_hidden(overlay_disc.holder, !o.pot);
      set_hidden(overlay_dot_face, o.pot);
      set_hidden(w.overlay_thirst, !o.pot);
      if (o.pot) {
        set_ring(overlay_disc, o.ring_permille, o.status_colour);
        if (pin.kind == PinKind::Plant) {
          set_disc_image(overlay_disc, photo_for(pin),
                         o.stale ? &overlay_leaf_stale : &overlay_leaf);
        } else {
          set_disc_image(overlay_disc, nullptr, nullptr);
        }
        set_text(w.overlay_verdict, o.verdict);
        lv_obj_set_style_text_color(w.overlay_verdict, hex(o.status_colour), 0);
        if (o.bar_permille >= 0) {
          lv_obj_set_width(overlay_bar_fill,
                           std::max(kBarH, kOverlayInnerW * o.bar_permille / 1000));
          lv_obj_set_style_bg_color(overlay_bar_fill, hex(o.status_colour), 0);
        }
        set_hidden(overlay_bar_fill, o.bar_permille < 0);
        if (o.happy_lo >= 0) {
          lv_obj_set_x(overlay_bar_happy, kOverlayInnerW * o.happy_lo / 100);
          lv_obj_set_width(overlay_bar_happy, kOverlayInnerW * (o.happy_hi - o.happy_lo) / 100);
          set_text(w.overlay_happy, "happy " + std::to_string(o.happy_lo) + " to " +
                                        std::to_string(o.happy_hi) + "%");
        } else {
          set_text(w.overlay_happy, "");
        }
        set_hidden(overlay_bar_happy, o.happy_lo < 0);
        lv_obj_set_pos(overlay_stats, kOverlayPad, kOverlayPad + 2 * kOverlayDiscRadius + 28 + 118);
      } else {
        lv_obj_set_style_bg_color(overlay_dot, hex(o.stale ? theme::kStale : theme::kTextPrimary),
                                  0);
        lv_obj_set_pos(overlay_stats, kOverlayPad, kOverlayPad + 2 * kOverlayDiscRadius + 28);
      }
      for (std::size_t i = 0; i < kMaxStats; ++i) {
        const bool used = i < o.stats.size();
        set_text(w.overlay_stat_label[i], used ? o.stats[i].label : std::string());
        set_text(w.overlay_stat_value[i], used ? o.stats[i].value : std::string());
        if (used) {
          lv_obj_set_style_text_color(w.overlay_stat_value[i], hex(o.stats[i].colour), 0);
        }
      }
      set_text(w.overlay_history_title, o.history_title);
    }
    overlay_look = std::move(o);
  }
};

// ------------------------------------------------------------------------------------------------

Screen::Screen(lv_obj_t* parent, const Config& config, const lv_image_dsc_t* heat_image,
               ScreenDeps deps)
    : impl_(std::make_unique<Impl>(parent, config, heat_image, std::move(deps))) {}

Screen::~Screen() {
  if (impl_ && impl_->w.root != nullptr) lv_obj_delete(impl_->w.root);
}

void Screen::update(const ViewModel& vm, const HeaderText& header, Timestamp now) {
  impl_->update_header(header);
  impl_->update_tabs(vm);
  impl_->update_rooms(vm);
  impl_->update_pins(vm);
  impl_->update_hud(vm);
  impl_->update_overlay(vm, now);
}

void Screen::invalidate_heat() {
  if (impl_->w.heat_image != nullptr) lv_obj_invalidate(impl_->w.heat_image);
}

std::size_t Screen::writes() const {
  return impl_->writes;
}

const ScreenWidgets& Screen::widgets() const {
  return impl_->w;
}

}  // namespace hearthmap::ui
