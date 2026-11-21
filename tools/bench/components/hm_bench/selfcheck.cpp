// Present self-check: the same tiles drawn by LVGL and by the shipped present path
// (hm::ui::show_tile_frame + DisplayPresenter), compared byte for byte in the DSI frame buffer,
// single tiles and bursts of mixed sizes, then left on screen side by side for a photo.
#include "selfcheck.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_system.h"
#include "esphome/components/display/display.h"
#include "esphome/components/display/display_color_utils.h"
#include "esphome/core/application.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "hearthmap/ui/present.h"
#include "hm_blit/display_presenter.h"
#include "hm_blit/panel_rotation.h"

// Benchmark only: the frame-buffer readback needs the DSI panel handle (see hm_bench.cpp).
#define protected public
#include "esphome/components/mipi_dsi/mipi_dsi.h"
#undef protected

namespace esphome::hm_bench {

static const char* const TAG = "bench";

namespace {

namespace ui = ::hearthmap::ui;

// Pixel buffers live in PSRAM: the first version kept them in internal RAM through std::vector,
// ran out on the 112 x 112 case at the right edge, and aborted in operator new (no exceptions
// here), on every boot.
template <class T>
struct PsAlloc {
  using value_type = T;
  PsAlloc() = default;
  template <class U>
  PsAlloc(const PsAlloc<U>&) {}
  T* allocate(size_t n) {
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
    return static_cast<T*>(p);  // a null here faults at once, which the crash-skip below reports
  }
  void deallocate(T* p, size_t) { heap_caps_free(p); }
  bool operator==(const PsAlloc&) const { return true; }
  bool operator!=(const PsAlloc&) const { return false; }
};
using PsVec = std::vector<uint16_t, PsAlloc<uint16_t>>;

// Which case was running, kept across a software reset: a case that crashed the last boot is
// reported and skipped instead of crash-looping.
constexpr uint32_t kCaseMagic = 0x5E1FC4E5;
__NOINIT_ATTR uint32_t g_case_magic;
__NOINIT_ATTR int32_t g_case_running;
int32_t g_case_crashed = -1;

bool begin_case(int32_t id) {
  if (id == g_case_crashed) {
    ESP_LOGI(TAG, "bench selfcheck case %d : SKIPPED, it crashed the previous boot", (int)id);
    return false;
  }
  g_case_magic = kCaseMagic;
  g_case_running = id;
  return true;
}
void end_case() {
  g_case_running = -1;
}
using esphome::hearthmap_panel::DisplayPresenter;
using esphome::hearthmap_panel::NativeBlock;
using esphome::hearthmap_panel::to_native;

constexpr uint16_t kRed = 0xF800, kGreen = 0x07E0, kBlue = 0x001F, kWhite = 0xFFFF,
                   kYellow = 0xFFE0;

// Quadrant colours, a 1-px yellow border, a black diagonal, and a tint per variant so two
// frames of the same tile differ everywhere.
PsVec pattern(int w, int h, int variant) {
  PsVec p(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t c = (y < h / 2) ? (x < w / 2 ? kRed : kGreen) : (x < w / 2 ? kBlue : kWhite);
      if (x == 0 || y == 0 || x == w - 1 || y == h - 1) c = kYellow;
      if (w > 2 && h > 2 && x * h / w == y) c = 0x0000;
      if (variant != 0) c = static_cast<uint16_t>(c ^ 0x39E7);  // a different, still opaque colour
      p[static_cast<size_t>(y) * w + x] = c;
    }
  }
  return p;
}

struct Case {
  int x, y, w, h;
};

struct Tile {
  Case c;
  PsVec frame[2];
  PsVec blank;
  lv_image_dsc_t dsc{};
  lv_obj_t* obj = nullptr;
  PsVec lvgl_native[2];  // what LVGL put in the frame buffer for each variant
};

struct Ctx {
  mipi_dsi::MipiDsi* display = nullptr;
  lvgl::LvglComponent* lvgl = nullptr;
  lv_obj_t* screen = nullptr;
  lv_obj_t* old_screen = nullptr;
  uint16_t* fb = nullptr;
  int nw = 0, nh = 0;
  std::unique_ptr<DisplayPresenter> presenter;
  std::vector<std::unique_ptr<Tile>> tiles;
};

Ctx* g = nullptr;

void set_frame(Tile& t, const uint16_t* px) {
  t.dsc.data = reinterpret_cast<const uint8_t*>(px);
}

NativeBlock native_of(const Case& c) {
  PsVec scratch(static_cast<size_t>(c.w) * c.h);
  PsVec src(static_cast<size_t>(c.w) * c.h);
  return to_native(scratch.data(), src.data(), c.w, c.x, c.y, c.w, c.h, 1, g->nw, g->nh);
}

PsVec read_fb(const NativeBlock& b) {
  PsVec out(static_cast<size_t>(b.w) * b.h);
  for (int r = 0; r < b.h; ++r) {
    uint16_t* row = g->fb + static_cast<size_t>(b.y + r) * g->nw + b.x;
    const uintptr_t a0 = reinterpret_cast<uintptr_t>(row) & ~uintptr_t{63};
    const uintptr_t a1 = (reinterpret_cast<uintptr_t>(row + b.w) + 63) & ~uintptr_t{63};
    esp_cache_msync(reinterpret_cast<void*>(a0), a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    std::memcpy(&out[static_cast<size_t>(r) * b.w], row, static_cast<size_t>(b.w) * 2);
  }
  return out;
}

int mismatches(const PsVec& a, const PsVec& b, const NativeBlock& nb, int* first_x, int* first_y,
               uint16_t* got, uint16_t* want) {
  int bad = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    if (a[i] != b[i]) {
      if (bad == 0) {
        *first_x = static_cast<int>(i % nb.w);
        *first_y = static_cast<int>(i / nb.w);
        *got = a[i];
        *want = b[i];
      }
      bad++;
    }
  }
  return bad;
}

void lvgl_show(Tile& t, const uint16_t* px) {
  set_frame(t, px);
  lv_obj_invalidate(t.obj);
  lv_refr_now(g->lvgl->get_disp());
}

}  // namespace

void add_selfcheck_steps(std::vector<std::function<void()>>& steps, mipi_dsi::MipiDsi* display,
                         lvgl::LvglComponent* lvgl) {
  steps.push_back([display, lvgl]() {
    g_case_crashed = -1;
    if (g_case_magic == kCaseMagic && g_case_running >= 0 &&
        esp_reset_reason() != ESP_RST_POWERON) {
      g_case_crashed = g_case_running;
      ESP_LOGI(TAG,
               "bench selfcheck : the previous boot crashed in case %d (reset reason %d); it will "
               "be skipped",
               (int)g_case_crashed, (int)esp_reset_reason());
    }
    end_case();
    delete g;
    g = new Ctx();
    g->display = display;
    g->lvgl = lvgl;
    void* fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(display->handle_, 1, &fb) != ESP_OK || fb == nullptr) {
      ESP_LOGI(TAG, "bench selfcheck : no frame buffer");
      return;
    }
    g->fb = static_cast<uint16_t*>(fb);
    g->nw = display->get_native_width();
    g->nh = display->get_native_height();
    g->presenter = std::make_unique<DisplayPresenter>(display, lvgl->get_rotation());
    const bool ok = g->presenter->begin();
    lv_display_t* disp = lvgl->get_disp();
    g->old_screen = lv_display_get_screen_active(disp);
    g->screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g->screen);
    lv_obj_set_style_bg_color(g->screen, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(g->screen, LV_OPA_COVER, 0);
    lv_screen_load(g->screen);
    for (int i = 0; i < 3; ++i) {
      lv_timer_handler();
      lv_refr_now(disp);
    }
    ESP_LOGI(TAG, "bench selfcheck start: native %dx%d, rotation %d, presenter begin %s, active=%d",
             g->nw, g->nh, (int)lvgl->get_rotation(), ok ? "ok" : "FAILED",
             lv_display_get_screen_active(disp) == g->screen);
  });

  // Single tiles: odd sizes, every corner of the screen, odd coordinates, the largest size.
  static const Case kCases[] = {{0, 0, 31, 17},     {1249, 783, 31, 17}, {333, 211, 31, 17},
                                {1, 1, 64, 64},     {1216, 736, 64, 64}, {517, 403, 64, 64},
                                {0, 688, 112, 112}, {1168, 0, 112, 112}, {700, 300, 48, 80},
                                {639, 399, 2, 2},   {101, 557, 128, 128}};
  steps.push_back([]() {
    if (g == nullptr || g->fb == nullptr) return;
    for (const Case& c : kCases) {
      auto t = std::make_unique<Tile>();
      t->c = c;
      t->frame[0] = pattern(c.w, c.h, 0);
      t->frame[1] = pattern(c.w, c.h, 1);
      t->blank.assign(static_cast<size_t>(c.w) * c.h, 0x0841);
      t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
      t->dsc.header.w = c.w;
      t->dsc.header.h = c.h;
      t->dsc.header.stride = c.w * 2;
      t->dsc.data_size = c.w * c.h * 2;
      set_frame(*t, t->blank.data());
      t->obj = lv_image_create(g->screen);
      lv_image_set_src(t->obj, &t->dsc);
      lv_obj_set_pos(t->obj, c.x, c.y);
      g->tiles.push_back(std::move(t));
    }
    lv_refr_now(g->lvgl->get_disp());
    int32_t case_id = 0;
    for (auto& tp : g->tiles) {
      Tile& t = *tp;
      if (!begin_case(case_id++)) continue;
      const NativeBlock nb = native_of(t.c);
      for (int v = 0; v < 2; ++v) {
        lvgl_show(t, t.frame[v].data());
        t.lvgl_native[v] = read_fb(nb);
      }
      // Present each variant over the other and compare with what LVGL drew.
      int total_bad = 0;
      for (int v = 0; v < 2; ++v) {
        lvgl_show(t, t.frame[1 - v].data());
        ui::PresentBlock why = ui::PresentBlock::Ok;
        const ui::TileShown shown =
            ui::show_tile_frame(t.obj, t.dsc, t.frame[v].data(), *g->presenter, &why);
        const PsVec got = read_fb(nb);
        int fx = -1, fy = -1;
        uint16_t gv = 0, wv = 0;
        const int bad = mismatches(got, t.lvgl_native[v], nb, &fx, &fy, &gv, &wv);
        total_bad += bad;
        ESP_LOGI(TAG,
                 "bench selfcheck tile %dx%d at %d,%d variant %d : %s (why %d), native %d,%d "
                 "%dx%d, %d of %u px "
                 "differ from LVGL%s%s",
                 t.c.w, t.c.h, t.c.x, t.c.y, v,
                 shown == ui::TileShown::Presented ? "presented" : "INVALIDATED", (int)why, nb.x,
                 nb.y, nb.w, nb.h, bad, (unsigned)got.size(),
                 bad ? (std::string(", first at ") + std::to_string(fx) + "," + std::to_string(fy) +
                        " got " + std::to_string(gv) + " want " + std::to_string(wv))
                           .c_str()
                     : "",
                 why == ui::PresentBlock::Covered
                     ? " (a later test tile overlaps it: refusal expected)"
                     : "");
      }
      App.feed_wdt();
      (void)total_bad;
    }
  });

  // Bursts: every tile flips frame in the same pass, 60 passes, then each is compared.
  steps.push_back([]() {
    if (g == nullptr || g->fb == nullptr || g->tiles.empty()) return;
    if (!begin_case(100)) return;
    int presented = 0, refused = 0, bad_tiles = 0, bad_px = 0;
    for (int pass = 0; pass < 60; ++pass) {
      const int v = pass & 1;
      for (auto& tp : g->tiles) {
        ui::PresentBlock why = ui::PresentBlock::Ok;
        if (ui::show_tile_frame(tp->obj, tp->dsc, tp->frame[v].data(), *g->presenter, &why) ==
            ui::TileShown::Presented) {
          presented++;
        } else {
          refused++;
          lv_refr_now(g->lvgl->get_disp());
        }
      }
      for (auto& tp : g->tiles) {
        const NativeBlock nb = native_of(tp->c);
        const PsVec got = read_fb(nb);
        int fx, fy;
        uint16_t gv, wv;
        const int bad = mismatches(got, tp->lvgl_native[v], nb, &fx, &fy, &gv, &wv);
        if (bad) {
          bad_tiles++;
          bad_px += bad;
        }
      }
      App.feed_wdt();
    }
    ESP_LOGI(TAG,
             "bench selfcheck burst : 60 passes x %u tiles, %d presented, %d fell back, %d tile "
             "checks failed "
             "(%d px)",
             (unsigned)g->tiles.size(), presented, refused, bad_tiles, bad_px);
  });

  // Side by side for a photo: left column drawn by LVGL, right column presented, same frames.
  steps.push_back([]() {
    if (g == nullptr || g->fb == nullptr) return;
    if (!begin_case(200)) return;
    for (auto& tp : g->tiles) lv_obj_delete(tp->obj);
    g->tiles.clear();
    lv_obj_t* l1 = lv_label_create(g->screen);
    lv_label_set_text(l1, "LVGL");
    lv_obj_set_style_text_color(l1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(l1, 200, 20);
    lv_obj_t* l2 = lv_label_create(g->screen);
    lv_label_set_text(l2, "present");
    lv_obj_set_style_text_color(l2, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(l2, 840, 20);
    static const Case kPairs[] = {{120, 80, 31, 17},
                                  {120, 140, 64, 64},
                                  {120, 240, 112, 112},
                                  {120, 400, 48, 80},
                                  {120, 520, 128, 128}};
    for (const Case& c : kPairs) {
      for (int side = 0; side < 2; ++side) {
        auto t = std::make_unique<Tile>();
        t->c = c;
        t->c.x += side * 640 + 1;  // odd x on the right
        t->frame[0] = pattern(c.w, c.h, 0);
        t->blank.assign(static_cast<size_t>(c.w) * c.h, 0x0841);
        t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        t->dsc.header.w = c.w;
        t->dsc.header.h = c.h;
        t->dsc.header.stride = c.w * 2;
        t->dsc.data_size = c.w * c.h * 2;
        set_frame(*t, side == 0 ? t->frame[0].data() : t->blank.data());
        t->obj = lv_image_create(g->screen);
        lv_image_set_src(t->obj, &t->dsc);
        lv_obj_set_pos(t->obj, t->c.x, t->c.y);
        g->tiles.push_back(std::move(t));
      }
    }
    lv_refr_now(g->lvgl->get_disp());
    int presented = 0;
    for (size_t i = 1; i < g->tiles.size(); i += 2) {
      Tile& t = *g->tiles[i];
      if (ui::show_tile_frame(t.obj, t.dsc, t.frame[0].data(), *g->presenter) ==
          ui::TileShown::Presented)
        presented++;
    }
    ESP_LOGI(TAG,
             "bench selfcheck sidebyside : %d of %u right-hand tiles presented; photograph now "
             "(held until the next run)",
             presented, (unsigned)(g->tiles.size() / 2));
    end_case();
  });
}

}  // namespace esphome::hm_bench
