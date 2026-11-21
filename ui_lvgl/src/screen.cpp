#include "hearthmap/ui/screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hearthmap/geometry.h"
#include "hearthmap/ui/format.h"
#include "hearthmap/ui/theme.h"
#include "image_util.h"
// The cover-check result is only writable through LVGL's private struct (heat_cover_cb). The root
// of the LVGL tree is on the include path in both builds (the host's CMake, and LVGL's own ESP-IDF
// component description).
#include "lvgl_private.h"

namespace hearthmap::ui {

namespace {

using detail::LeafImage;

// ---- geometry of the spec ------------------------------------------------------------------

constexpr int kPlainDotRadius = 7;
constexpr int kPlainDotBorder = 3;
constexpr int kPlantRadius = 22;
constexpr int kPinRingWidth = 6;
// Pulses are stepped, not animated (components/hearthmap/README.md, "Measuring"): every frame
// of a property animation re-renders the object's whole area and everything under it in
// software, which on the panel kept LVGL busy for most of every second. A stepped pulse changes
// a few times a second, each change touches only the object's own small area, and objects are
// spread over kPulseSlots offsets so their steps do not all land in the same refresh.
constexpr std::uint32_t kPulseTickMs = 50;  // how often the pulse clock looks; changes are rarer
constexpr std::uint32_t kPulseSlots = 8;
// Danger ring around a pin: a ripple of kRingFrames frames, then a gap, every kRingCycleMs.
constexpr int kPulseWidth = 4;
constexpr std::uint32_t kRingCycleMs = 1600;
struct RingFrame {
  int radius;  // 0: hidden
  lv_opa_t opa;
};
constexpr RingFrame kRingFrames[] = {{30, 255}, {40, 150}, {50, 60}, {0, 0}};
constexpr int kPulseTo = 50;  // largest ring radius: the holder's half size
// Room outline: opacity steps over kRoomCycleMs (the old eased ramp ran 0.35..1..0.35 over 2.8 s).
constexpr std::uint32_t kRoomCycleMs = 2800;
constexpr lv_opa_t kRoomSteps[] = {255, 172, 89, 172};
constexpr int kRoomPulseWidth = 5;
constexpr int kBadgeW = 28;
constexpr int kHudTextWidth = 146;  // card 328 - text x 84 - "100%" at 28 px - margins
constexpr int kPillMaxWidth = 120;  // room labels keep clear of a value pill this wide
constexpr int kLabelInsetLeft = 12;
constexpr int kLabelInsetRight = 8;
constexpr int kLabelInsetTop = 8;
constexpr int kHudBottomMargin = 12;
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

// One line, `width` wide, ellipsis at the end. A DOTS label with content height wraps instead,
// so the height is pinned to one line of `font`.
void single_line(lv_obj_t* label, std::int32_t width, const lv_font_t* font) {
  lv_obj_set_size(label, width, lv_font_get_line_height(font));
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
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

/// The step of a cycle of `steps` equal steps over `cycle_ms` that object `index` shows at
/// `now_ms`. Objects are offset by their slot so that their steps fall at different times.
std::size_t pulse_step(std::uint32_t now_ms, std::size_t index, std::uint32_t cycle_ms,
                       std::size_t steps) {
  const std::uint32_t step_ms = cycle_ms / static_cast<std::uint32_t>(steps);
  const auto slot = static_cast<std::uint32_t>(index % kPulseSlots);
  const std::uint32_t shifted = now_ms + slot * (step_ms / kPulseSlots);
  return static_cast<std::size_t>((shifted / step_ms) % steps);
}

// LVGL 9.6's image widget never reports that it covers an area unless it also has an opaque
// background: its class handler runs lv_obj's cover check first (no background: "not cover") and
// only ever downgrades that verdict. The heat image is an opaque RGB565 picture, so for an area
// inside it, drawn at full opacity, it does cover: saying so lets LVGL start each refresh of the
// map at the image instead of filling the screen's background under it first. The public
// lv_event_set_cover_res() can only weaken a verdict, so the private struct is written.
void heat_cover_cb(lv_event_t* e) {
  auto* info = static_cast<lv_cover_check_info_t*>(lv_event_get_param(e));
  if (info == nullptr || info->res == LV_COVER_RES_MASKED) return;
  lv_obj_t* img = lv_event_get_current_target_obj(e);
  const void* src = lv_image_get_src(img);
  if (src == nullptr || lv_image_src_get_type(src) != LV_IMAGE_SRC_VARIABLE) return;
  const auto* dsc = static_cast<const lv_image_dsc_t*>(src);
  if (dsc->header.cf != LV_COLOR_FORMAT_RGB565 || dsc->data == nullptr) return;
  if (lv_obj_get_style_image_opa(img, LV_PART_MAIN) < LV_OPA_MAX) return;
  if (lv_obj_get_style_opa(img, LV_PART_MAIN) < LV_OPA_MAX) return;
  lv_area_t coords;
  lv_obj_get_coords(img, &coords);
  if (lv_image_get_scale_x(img) != LV_SCALE_NONE || lv_image_get_scale_y(img) != LV_SCALE_NONE ||
      lv_image_get_rotation(img) != 0)
    return;
  if (static_cast<std::int32_t>(dsc->header.w) != lv_area_get_width(&coords) ||
      static_cast<std::int32_t>(dsc->header.h) != lv_area_get_height(&coords))
    return;
  if (lv_area_is_in(info->area, &coords, 0)) info->res = LV_COVER_RES_COVER;
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
  /// Per room, its outline as one short line object per edge (points relative to each object,
  /// which sits on its edge), so a pulse step invalidates thin strips along the walls, never the
  /// room's interior. lv_line sizes itself from its own origin to its farthest point, so one
  /// polyline object at the map's origin would invalidate everything from the map's top-left
  /// corner to the room's far corner.
  std::vector<std::vector<lv_obj_t*>> room_edges;
  std::vector<std::array<lv_point_precise_t, 2>> edge_points;  // sized once: lines keep pointers
  /// The pulse step each object shows now: -1 when not pulsing (hidden), -2 when it has just
  /// started and no step is shown yet.
  std::vector<int> room_step;
  std::vector<int> ring_step;
  lv_timer_t* pulse_timer = nullptr;
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
  std::optional<bool> summary_offline;
  std::optional<bool> heat_dimmed;
  std::optional<std::vector<Layer>> tab_layers;
  std::optional<Layer> tab_active;
  std::vector<std::optional<Severity>> room_pulse;
  std::vector<std::optional<PinLook>> pin_look;
  std::optional<HudLook> hud_look;
  std::optional<OverlayLook> overlay_look;
  std::vector<std::string> hud_pin_ids;  // per card, for TapHudItem
  std::map<std::string, const lv_image_dsc_t*, std::less<>> photo_cache;
  /// Pre-sized photos by (photo id, disc diameter), where the provider has them.
  std::map<std::pair<std::string, int>, const lv_image_dsc_t*> sized_photo_cache;

  Impl(lv_obj_t* parent, const Config& cfg, const lv_image_dsc_t* heat, ScreenDeps d)
      : config(cfg), deps(std::move(d)) {
    load_photos();
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
    // Display coordinates -> canvas: the root sits at its parent's origin, wherever that is.
    lv_area_t root;
    lv_obj_get_coords(self(e)->w.root, &root);
    p.x -= root.x1;
    p.y -= root.y1;
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

  // LVGL frees the tree with its parent: if the embedder deletes the parent first, forget every
  // widget so update() and ~Screen() never touch freed objects, and stop the pulse clock.
  static void on_root_deleted(lv_event_t* e) {
    Impl* s = self(e);
    s->w = ScreenWidgets{};
    s->room_edges.clear();
    s->room_step.clear();
    s->ring_step.clear();
    s->stop_pulse_timer();
  }

  ~Impl() { stop_pulse_timer(); }

  void stop_pulse_timer() {
    if (pulse_timer != nullptr) lv_timer_delete(pulse_timer);
    pulse_timer = nullptr;
  }

  void build_root(lv_obj_t* parent) {
    w.root = make_box(parent);
    lv_obj_add_event_cb(w.root, on_root_deleted, LV_EVENT_DELETE, this);
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

  // ---- room label layout (config only, so it never changes on update) ----------------------

  struct Box {
    std::int32_t x1, y1, x2, y2;
    bool hits(const Box& o) const { return x1 <= o.x2 && o.x1 <= x2 && y1 <= o.y2 && o.y1 <= y2; }
  };

  static std::int32_t text_width(const std::string& t, const lv_font_t* font) {
    lv_point_t sz;
    lv_text_get_size(&sz, t.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
  }

  // Map-pixel boxes a room label must not sit under: every pin's disc or dot, its badge, and for
  // plain pins the widest value pill they may show (kPillMaxWidth). Boxes are inclusive and
  // follow build_pin()'s geometry exactly.
  std::vector<Box> pin_obstacles() const {
    std::vector<Box> out;
    const std::int32_t pill_half = (lv_font_get_line_height(deps.fonts.px22) + 2) / 2;
    for (const Pin& pin : config.pins) {
      const lv_point_precise_t c = map_px(pin.position);
      if (in_pot(pin.kind)) {
        out.push_back(Box{c.x - kPlantRadius, c.y - kPlantRadius - kBadgeH / 2,
                          c.x + kPlantRadius + kBadgeW / 2, c.y + kPlantRadius});
      } else {
        constexpr std::int32_t r = kPlainDotRadius + kPlainDotBorder;
        out.push_back(Box{c.x - r, c.y - r, c.x + r, c.y + r});                       // dot
        out.push_back(Box{c.x - r + 2, c.y - r - kBadgeH - 8, c.x - r + 2 + kBadgeW,  // badge
                          c.y - r - 8});
        out.push_back(Box{c.x + r, c.y - pill_half, c.x + r + kPillMaxWidth,  // pill
                          c.y + pill_half});
      }
    }
    return out;
  }

  // Width-limited to the room (ellipsis), hidden when fewer than three characters would fit,
  // and placed in the first corner of the room's bounding box (top-left, top-right,
  // bottom-left, bottom-right) that lies inside the room and is clear of every pin. When no
  // corner is clear it stays top-left.
  void place_room_label(lv_obj_t* label, std::size_t r) {
    const Room& room = config.rooms[r];
    const lv_font_t* font = deps.fonts.px20;
    std::int32_t x1 = LV_COORD_MAX, y1 = LV_COORD_MAX, x2 = -LV_COORD_MAX, y2 = -LV_COORD_MAX;
    for (const auto& v : room.polygon) {
      const lv_point_precise_t p = map_px(v);
      x1 = std::min<std::int32_t>(x1, p.x);
      y1 = std::min<std::int32_t>(y1, p.y);
      x2 = std::max<std::int32_t>(x2, p.x);
      y2 = std::max<std::int32_t>(y2, p.y);
    }
    const std::int32_t full = text_width(room.name, font);
    // Narrow rooms get tighter insets before the name is shortened.
    std::int32_t inset_l = kLabelInsetLeft;
    std::int32_t inset_r = kLabelInsetRight;
    if ((x2 - x1) - inset_l - inset_r < full) {
      inset_l = 6;
      inset_r = 4;
    }
    const std::int32_t avail = (x2 - x1) - inset_l - inset_r;
    const std::int32_t least =
        room.name.size() <= 3 ? full : text_width(room.name.substr(0, 3) + "...", font);
    if (full > avail && avail < least) {
      lv_obj_set_hidden(label, true);
      return;
    }
    const std::int32_t lw = std::min(full, avail);
    const std::int32_t lh = lv_font_get_line_height(font);
    lv_obj_set_size(label, lw, lh);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);

    const std::int32_t left = x1 + inset_l;
    const std::int32_t right = x2 - inset_r - lw;
    const std::int32_t top = y1 + kLabelInsetTop;
    const std::int32_t bottom = y2 - kLabelInsetTop - lh;
    const Box candidates[] = {{left, top, left + lw - 1, top + lh - 1},
                              {right, top, right + lw - 1, top + lh - 1},
                              {left, bottom, left + lw - 1, bottom + lh - 1},
                              {right, bottom, right + lw - 1, bottom + lh - 1}};
    const std::vector<Box> obstacles = pin_obstacles();
    const CanvasRect& a = config.canvas.map_area;
    auto inside = [&](std::int32_t x, std::int32_t y) {
      return point_in_polygon(room.polygon, MapPoint{(x + 0.5) / a.width, (y + 0.5) / a.height});
    };
    Box chosen = candidates[0];
    for (const Box& b : candidates) {
      bool clear =
          inside(b.x1, b.y1) && inside(b.x2, b.y1) && inside(b.x1, b.y2) && inside(b.x2, b.y2);
      // The glyphs leave a couple of pixels of the line box empty: a touch there is not a hit.
      const Box ink{b.x1 + 2, b.y1 + 2, b.x2 - 2, b.y2 - 2};
      for (const Box& o : obstacles) clear = clear && !ink.hits(o);
      if (clear) {
        chosen = b;
        break;
      }
    }
    lv_obj_set_pos(label, chosen.x1, chosen.y1);
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
    lv_obj_add_event_cb(w.heat_image, heat_cover_cb, LV_EVENT_COVER_CHECK, nullptr);

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
    build_room_pulses();
    for (std::size_t r = 0; r < config.rooms.size(); ++r) {
      lv_obj_t* label = make_label(w.map, deps.fonts.px20, theme::kTextSecondary);
      lv_label_set_text(label, config.rooms[r].name.c_str());
      place_room_label(label, r);
      w.room_label.push_back(label);
    }
    room_pulse.assign(config.rooms.size(), std::nullopt);

    // Danger pulses below every pin, then the pins.
    w.pin_pulse.assign(config.pins.size(), nullptr);
    ring_step.assign(config.pins.size(), -1);
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
      lv_obj_set_size(ring, 2 * kRingFrames[0].radius, 2 * kRingFrames[0].radius);
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

  // One hidden container per room at the room's bounding box (it carries the pulse colour, for
  // embedders and tests), holding one line object per edge.
  void build_room_pulses() {
    constexpr std::int32_t pad = kRoomPulseWidth;  // room for the width and the round caps
    std::size_t edges = 0;
    for (const auto& pts : room_points) edges += pts.size() > 1 ? pts.size() - 1 : 0;
    edge_points.assign(edges, {});
    room_edges.assign(config.rooms.size(), {});
    room_step.assign(config.rooms.size(), -1);
    std::size_t e = 0;
    for (std::size_t r = 0; r < config.rooms.size(); ++r) {
      const auto& pts = room_points[r];
      lv_obj_t* box = make_box(w.map);
      allow_overflow(box, pad);
      std::int32_t x1 = LV_COORD_MAX, y1 = LV_COORD_MAX, x2 = -LV_COORD_MAX, y2 = -LV_COORD_MAX;
      for (const auto& p : pts) {
        x1 = std::min<std::int32_t>(x1, static_cast<std::int32_t>(p.x));
        y1 = std::min<std::int32_t>(y1, static_cast<std::int32_t>(p.y));
        x2 = std::max<std::int32_t>(x2, static_cast<std::int32_t>(p.x));
        y2 = std::max<std::int32_t>(y2, static_cast<std::int32_t>(p.y));
      }
      if (pts.empty()) x1 = y1 = x2 = y2 = 0;
      lv_obj_set_pos(box, x1, y1);
      lv_obj_set_size(box, x2 - x1 + 1, y2 - y1 + 1);
      lv_obj_set_style_line_opa(box, LV_OPA_COVER, 0);
      for (std::size_t k = 0; k + 1 < pts.size(); ++k, ++e) {
        const auto ax = static_cast<std::int32_t>(pts[k].x);
        const auto ay = static_cast<std::int32_t>(pts[k].y);
        const auto bx = static_cast<std::int32_t>(pts[k + 1].x);
        const auto by = static_cast<std::int32_t>(pts[k + 1].y);
        const std::int32_t ox = std::min(ax, bx) - pad;
        const std::int32_t oy = std::min(ay, by) - pad;
        auto& ep = edge_points[e];
        ep[0].x = static_cast<lv_value_precise_t>(ax - ox);
        ep[0].y = static_cast<lv_value_precise_t>(ay - oy);
        ep[1].x = static_cast<lv_value_precise_t>(bx - ox);
        ep[1].y = static_cast<lv_value_precise_t>(by - oy);
        lv_obj_t* line = lv_line_create(box);
        lv_obj_remove_style_all(line);
        lv_obj_set_clickable(line, false);
        lv_obj_set_pos(line, ox - x1, oy - y1);
        lv_line_set_points(line, ep.data(), 2);
        // The object ends at its farthest point; the width and cap need `pad` more.
        lv_obj_set_size(line, std::abs(bx - ax) + 2 * pad + 1, std::abs(by - ay) + 2 * pad + 1);
        lv_obj_set_style_line_width(line, kRoomPulseWidth, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
        room_edges[r].push_back(line);
      }
      lv_obj_set_hidden(box, true);
      w.room_pulse.push_back(box);
    }
  }

  // ---- the pulse clock -------------------------------------------------------------------------

  static void on_pulse_tick(lv_timer_t* t) {
    static_cast<Impl*>(lv_timer_get_user_data(t))->apply_pulse_steps(lv_tick_get());
  }

  /// Show every running pulse's step for `now_ms`, writing only what changed; stop the clock
  /// when nothing pulses.
  void apply_pulse_steps(std::uint32_t now_ms) {
    bool any = false;
    for (std::size_t r = 0; r < room_step.size(); ++r) {
      if (room_step[r] == -1) continue;
      any = true;
      const auto step =
          static_cast<int>(pulse_step(now_ms, r, kRoomCycleMs, std::size(kRoomSteps)));
      if (step == room_step[r]) continue;
      room_step[r] = step;
      for (lv_obj_t* line : room_edges[r]) lv_obj_set_style_line_opa(line, kRoomSteps[step], 0);
    }
    for (std::size_t i = 0; i < ring_step.size(); ++i) {
      if (ring_step[i] == -1) continue;
      any = true;
      const auto step =
          static_cast<int>(pulse_step(now_ms, i, kRingCycleMs, std::size(kRingFrames)));
      if (step == ring_step[i]) continue;
      ring_step[i] = step;
      show_ring_frame(w.pin_pulse[i], kRingFrames[step]);
    }
    if (!any) stop_pulse_timer();
  }

  static void show_ring_frame(lv_obj_t* ring, const RingFrame& f) {
    if (f.radius == 0) {
      lv_obj_set_style_opa(ring, LV_OPA_TRANSP, 0);  // stays "shown": the pulse is running
      return;
    }
    lv_obj_set_style_opa(ring, LV_OPA_COVER, 0);
    lv_obj_set_size(ring, 2 * f.radius, 2 * f.radius);
    lv_obj_set_style_border_opa(ring, f.opa, 0);
  }

  void ensure_pulse_timer() {
    if (pulse_timer == nullptr) pulse_timer = lv_timer_create(on_pulse_tick, kPulseTickMs, this);
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
        set_disc_image(d, photo_for(pin, 2 * kPlantRadius), &pin_leaf);
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

  // Each photo id is asked of the provider once, at construction, and the pointer reused by
  // every widget that shows it (ports.h: descriptors live as long as the Screen).
  void load_photos() {
    if (deps.photos == nullptr) return;
    for (const Pin& pin : config.pins) {
      if (!pin.photo.has_value() || photo_cache.count(*pin.photo) != 0) continue;
      photo_cache.emplace(*pin.photo, deps.photos->plant_photo(*pin.photo));
      for (int diameter : {2 * kPlantRadius, 2 * kHudDiscRadius, 2 * kOverlayDiscRadius}) {
        const lv_image_dsc_t* sized = deps.photos->plant_photo_sized(*pin.photo, diameter);
        // Only an exact fit is used as pre-sized: anything else would still need scaling.
        if (sized != nullptr && static_cast<int>(sized->header.w) == diameter &&
            static_cast<int>(sized->header.h) == diameter) {
          sized_photo_cache.emplace(std::make_pair(*pin.photo, diameter), sized);
        }
      }
    }
  }

  /// The photo for a disc of `diameter`: pre-sized when the provider has it, else the one
  /// plant_photo() gave (scaled to cover), else nullptr (the leaf).
  const lv_image_dsc_t* photo_for(const Pin& pin, int diameter) const {
    if (!pin.photo.has_value()) return nullptr;
    auto sized = sized_photo_cache.find(std::make_pair(*pin.photo, diameter));
    if (sized != sized_photo_cache.end()) return sized->second;
    auto it = photo_cache.find(*pin.photo);
    return it == photo_cache.end() ? nullptr : it->second;
  }

  static void set_disc_image(Disc& d, const lv_image_dsc_t* photo, const LeafImage* leaf) {
    if (photo != nullptr) {
      lv_image_set_src(d.image, photo);
      // A photo already the disc's size is centred, not scaled: no software transform.
      const bool fits =
          static_cast<int32_t>(photo->header.w) == lv_obj_get_style_width(d.image, LV_PART_MAIN) &&
          static_cast<int32_t>(photo->header.h) == lv_obj_get_style_height(d.image, LV_PART_MAIN);
      lv_image_set_inner_align(d.image, fits ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_COVER);
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

    // Gap between cards: the spec's 10 px, reduced (never the cards) so the cards plus the
    // "+N more" line end kHudBottomMargin above the screen's bottom edge. Below 4 px it stops
    // shrinking, and the pool holds only the cards that then fit: with a larger max_items the
    // items beyond the pool are counted in "+N more" (update_hud).
    const int start = theme::kHudY + kHudTitleHeight;
    // int32_t is long on the ESP32-P4 toolchain: name every conversion to int.
    const int more_h = static_cast<int>(lv_font_get_line_height(f.px20));
    const int usable =
        static_cast<int>(config.canvas.height) - kHudBottomMargin - more_h - 2 - start;
    const int fits = std::max(1, usable / (theme::kHudCardHeight + 4));
    const auto cap = static_cast<std::size_t>(
        std::min<std::int32_t>(std::max<std::int32_t>(0, config.hud.max_items), fits));
    hud_pin_ids.assign(cap, std::string());
    const int n = static_cast<int>(cap);
    int gap = theme::kHudCardGap;
    if (n > 0) {
      const int room = static_cast<int>(config.canvas.height) - kHudBottomMargin - more_h - 2 -
                       start - n * theme::kHudCardHeight;
      gap = std::max(4, std::min(theme::kHudCardGap, room / n));
    }
    for (std::size_t i = 0; i < cap; ++i) {
      lv_obj_t* card = make_box(w.root);
      lv_obj_set_size(card, theme::kHudWidth, theme::kHudCardHeight);
      lv_obj_set_pos(card, theme::kHudX,
                     start + static_cast<int>(i) * (theme::kHudCardHeight + gap));
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
      single_line(name, kHudTextWidth, f.px22);
      lv_obj_t* room = make_label(card, f.px20, theme::kTextSecondary);
      lv_obj_set_pos(room, 84, 45);
      single_line(room, kHudTextWidth, f.px20);
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
    lv_obj_set_pos(w.hud_more, theme::kHudX + 4, start + n * (theme::kHudCardHeight + gap) + 2);
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
    single_line(w.overlay_title, kOverlayW - text_x - kOverlayPad, f.px34);
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

  void update_header(const HeaderText& h, bool offline) {
    const bool clock_changed = clock_text != h.clock;
    if (clock_changed) {
      set_text(w.clock, h.clock);
      clock_text = h.clock;
    }
    // Offline replaces the summary (D-046): its averages come from readings nobody is
    // confirming any more, and the warning is what matters while the source is away.
    static const std::string kOffline = "Offline";
    const std::string& summary = offline ? kOffline : h.summary;
    if (summary_text != summary) {
      set_text(w.summary, summary);
      summary_text = summary;
    }
    if (summary_offline != offline) {
      lv_obj_set_style_text_color(w.summary, hex(offline ? theme::kWarn : theme::kTextSecondary),
                                  0);
      count();
      summary_offline = offline;
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
      lv_obj_t* box = w.room_pulse[rv.room_index];
      if (sev == Severity::Ok) {
        room_step[rv.room_index] = -1;
        set_hidden(box, true);
      } else {
        const lv_color_t colour = hex(severity_colour(sev));
        lv_obj_set_style_line_color(box, colour, 0);
        for (lv_obj_t* line : room_edges[rv.room_index]) {
          lv_obj_set_style_line_color(line, colour, 0);
        }
        room_step[rv.room_index] = -2;  // running; the step is written below
        set_hidden(box, false);
        ensure_pulse_timer();
        apply_pulse_steps(lv_tick_get());
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
        // Moisture feeding a disabled soil layer is not judged (D-020): a neutral ring.
        l.ring_colour = config.layer(Layer::Soil).enabled ? severity_colour(pv.primary.severity)
                                                          : theme::kTextSecondary;
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
    } else {
      // Off the active layer: no value, but a silent sensor still looks silent (principle 1).
      l.stale = pv.primary.freshness != Freshness::Fresh;
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
        // Dim the disc or the dot and value, never the badge (it stays at full strength).
        const lv_opa_t opa = look.dim ? LV_OPA_40 : LV_OPA_COVER;
        if (pin_disc[i].holder != nullptr) {
          lv_obj_set_style_opa(pin_disc[i].holder, opa, 0);
        } else {
          lv_obj_set_style_opa(pin_dot[i], opa, 0);
          lv_obj_set_style_opa(w.pin_value[i], opa, 0);
        }
        count();
      }
      if (first || prev->badge != look.badge) {
        if (look.badge != Severity::Ok) {
          lv_obj_set_style_bg_color(w.pin_badge[i], hex(severity_colour(look.badge)), 0);
        }
        set_hidden(w.pin_badge[i], look.badge == Severity::Ok);
      }
      if (first || prev->pulse != look.pulse) {
        set_hidden(w.pin_pulse[i], !look.pulse);
        ring_step[i] = look.pulse ? -2 : -1;  // -2: running; the frame is written below
        if (look.pulse) {
          ensure_pulse_timer();
          apply_pulse_steps(lv_tick_get());
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
          const lv_image_dsc_t* photo = photo_for(pin, 2 * kPlantRadius);
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
    // Items beyond the card pool (a max_items larger than the screen holds) join "+N more".
    const std::size_t total = vm.hud.items.size() + vm.hud.overflow;
    if (look.items.size() > w.hud_card.size()) look.items.resize(w.hud_card.size());
    look.overflow = total - look.items.size();
    if (hud_look.has_value() && *hud_look == look) return;

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
      set_disc_image(hud_disc[c], photo_for(pin, 2 * kHudDiscRadius), &hud_leaf);
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

  OverlayLook overlay_of(const ViewModel& vm, Timestamp now, bool restamped) const {
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
    // Re-stamped times say only "the source is connected" (D-012), never when the sensor last
    // reported, so they get no "seen" text at all (D-046).
    o.subtitle = room_name(pin.room_id);
    if (!restamped) {
      std::string seen = "never seen";
      if (prow != nullptr && prow->last_updated.has_value()) {
        seen = format_seen(now - *prow->last_updated);
      }
      o.subtitle += "   " + seen;
    }
    const bool fresh = prow != nullptr && prow->assessment.freshness == Freshness::Fresh &&
                       prow->assessment.value.has_value();
    o.stale = !fresh;
    if (o.pot) {
      if (prow != nullptr) {
        if (config.layer(Layer::Soil).enabled) {
          o.verdict = thirst_verdict(prow->assessment);
          o.status_colour = severity_colour(prow->assessment.severity);
        } else {
          // Not judged (D-020): the percentage alone, in a neutral colour, never "happy".
          o.verdict =
              fresh ? std::to_string(moisture_percent(*prow->assessment.value)) + "%" : "no data";
          o.status_colour = theme::kTextSecondary;
        }
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

  void update_heat_dim(bool behind) {
    if (heat_dimmed == behind || w.heat_image == nullptr) return;
    // A style change invalidates the whole image: one full map repaint each way (D-047).
    lv_obj_set_style_image_opa(w.heat_image, behind ? LV_OPA_40 : LV_OPA_COVER, 0);
    count();
    heat_dimmed = behind;
  }

  void update_overlay(const ViewModel& vm, Timestamp now, bool restamped) {
    OverlayLook o = overlay_of(vm, now, restamped);
    if (overlay_look.has_value() && *overlay_look == o) return;
    const OverlayLook* prev = overlay_look.has_value() ? &*overlay_look : nullptr;
    if (prev == nullptr || prev->open != o.open) set_hidden(w.scrim, !o.open);
    if (o.open) {
      // Opening, or switching to another pin, rewrites every group; otherwise only the groups
      // whose shown state changed (a minute passing touches the subtitle and nothing else).
      const bool all = prev == nullptr || !prev->open || prev->pin != o.pin || prev->pot != o.pot;
      const Pin& pin = config.pins[o.pin];
      if (all || prev->title != o.title) set_text(w.overlay_title, o.title);
      if (all || prev->subtitle != o.subtitle) set_text(w.overlay_subtitle, o.subtitle);
      if (all) {
        set_hidden(overlay_disc.holder, !o.pot);
        set_hidden(overlay_dot_face, o.pot);
        set_hidden(w.overlay_thirst, !o.pot);
        lv_obj_set_pos(overlay_stats, kOverlayPad,
                       kOverlayPad + 2 * kOverlayDiscRadius + 28 + (o.pot ? 118 : 0));
      }
      if (o.pot) {
        if (all || prev->ring_permille != o.ring_permille ||
            prev->status_colour != o.status_colour) {
          set_ring(overlay_disc, o.ring_permille, o.status_colour);
        }
        if (all || prev->stale != o.stale) {
          if (pin.kind == PinKind::Plant) {
            set_disc_image(overlay_disc, photo_for(pin, 2 * kOverlayDiscRadius),
                           o.stale ? &overlay_leaf_stale : &overlay_leaf);
          } else {
            set_disc_image(overlay_disc, nullptr, nullptr);
          }
          count();
        }
        if (all || prev->verdict != o.verdict || prev->status_colour != o.status_colour) {
          set_text(w.overlay_verdict, o.verdict);
          lv_obj_set_style_text_color(w.overlay_verdict, hex(o.status_colour), 0);
        }
        if (all || prev->bar_permille != o.bar_permille || prev->status_colour != o.status_colour) {
          if (o.bar_permille >= 0) {
            lv_obj_set_width(overlay_bar_fill,
                             std::max(kBarH, kOverlayInnerW * o.bar_permille / 1000));
            lv_obj_set_style_bg_color(overlay_bar_fill, hex(o.status_colour), 0);
          }
          set_hidden(overlay_bar_fill, o.bar_permille < 0);
        }
        if (all || prev->happy_lo != o.happy_lo || prev->happy_hi != o.happy_hi) {
          if (o.happy_lo >= 0) {
            lv_obj_set_x(overlay_bar_happy, kOverlayInnerW * o.happy_lo / 100);
            lv_obj_set_width(overlay_bar_happy, kOverlayInnerW * (o.happy_hi - o.happy_lo) / 100);
            set_text(w.overlay_happy, "happy " + std::to_string(o.happy_lo) + " to " +
                                          std::to_string(o.happy_hi) + "%");
          } else {
            set_text(w.overlay_happy, "");
          }
          set_hidden(overlay_bar_happy, o.happy_lo < 0);
        }
      } else if (all || prev->stale != o.stale) {
        lv_obj_set_style_bg_color(overlay_dot, hex(o.stale ? theme::kStale : theme::kTextPrimary),
                                  0);
        count();
      }
      for (std::size_t i = 0; i < kMaxStats; ++i) {
        const bool used = i < o.stats.size();
        const bool was_used = prev != nullptr && i < prev->stats.size();
        if (!all && used == was_used && (!used || prev->stats[i] == o.stats[i])) continue;
        set_text(w.overlay_stat_label[i], used ? o.stats[i].label : std::string());
        set_text(w.overlay_stat_value[i], used ? o.stats[i].value : std::string());
        if (used) {
          lv_obj_set_style_text_color(w.overlay_stat_value[i], hex(o.stats[i].colour), 0);
        }
      }
      if (all || prev->history_title != o.history_title) {
        set_text(w.overlay_history_title, o.history_title);
      }
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

void Screen::update(const ViewModel& vm, const HeaderText& header, Timestamp now,
                    const SourceStatus& status) {
  if (impl_->w.root == nullptr) return;  // the tree went with its parent
  impl_->update_header(header, status.offline);
  impl_->update_heat_dim(status.heat_behind);
  impl_->update_tabs(vm);
  impl_->update_rooms(vm);
  impl_->update_pins(vm);
  impl_->update_hud(vm);
  impl_->update_overlay(vm, now, status.restamped);
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
