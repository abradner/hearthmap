#include "hm_bench.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#include "driver/ppa.h"
#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esphome/components/display/display.h"
#include "esphome/components/display/display_color_utils.h"
#include "esphome/core/application.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hearthmap/ui/blitter.h"
#include "hm_blit/ppa_blitter.h"
#include "kernels.h"

// Benchmark only: the frame-buffer measurements need the DSI panel handle, which MipiDsi keeps
// protected (and the class is final). Everything that header includes is included above, so
// the redefinition reaches only MipiDsi's own declaration. Never do this in shipped code.
#define protected public
#include "esphome/components/mipi_dsi/mipi_dsi.h"
#undef protected

namespace esphome::hm_bench {

static const char* const TAG = "bench";

namespace {

enum Mem : uint8_t { PS, IN };
const char* mem_name(Mem m) {
  return m == PS ? "psram" : "iram";
}

void* alloc_buf(size_t bytes, Mem m) {
  bytes = (bytes + 63) & ~size_t{63};
  const uint32_t caps = m == PS ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                : (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  return heap_caps_aligned_alloc(64, bytes, caps);
}

// Integer formatting: ESPHome's printf may lack float support.
std::string f2(double v) {
  char b[32];
  if (v < 0) v = 0;
  const auto whole = static_cast<unsigned long long>(v);
  const auto frac = static_cast<unsigned>((v - static_cast<double>(whole)) * 100.0 + 0.5);
  if (frac >= 100) {
    snprintf(b, sizeof b, "%llu.00", whole + 1);
  } else {
    snprintf(b, sizeof b, "%llu.%02u", whole, frac);
  }
  return b;
}

struct Res {
  double mpx{0};
  double us_call{0};
  uint32_t n{0};
  bool ok{true};
};

template <class F>
Res time_op(F&& f, uint64_t px, int64_t min_us = 40000) {
  Res r;
  if (!f()) {  // warm-up call, also the correctness run
    r.ok = false;
    return r;
  }
  const int64_t t0 = esp_timer_get_time();
  int64_t el = 0;
  do {
    if (!f()) {
      r.ok = false;
      return r;
    }
    r.n++;
    el = esp_timer_get_time() - t0;
  } while ((el < min_us && r.n < 5000) || r.n < 3);
  r.us_call = static_cast<double>(el) / r.n;
  r.mpx = static_cast<double>(px) * r.n / static_cast<double>(el);
  return r;
}

std::map<std::string, double> g_overhead;

void report(const char* name, int w, int h, Mem src, Mem dst, const Res& r, const char* note) {
  if (!r.ok) {
    ESP_LOGI(TAG, "bench %s size=%dx%d src=%s dst=%s : FAILED%s%s", name, w, h, mem_name(src),
             mem_name(dst), note[0] ? ", " : "", note);
    return;
  }
  auto it = g_overhead.find(name);
  const std::string ov = it == g_overhead.end() ? std::string("?") : f2(it->second);
  ESP_LOGI(
      TAG,
      "bench %s size=%dx%d src=%s dst=%s : %s Mpx/s, call overhead %s us, n=%u, %s us/call%s%s",
      name, w, h, mem_name(src), mem_name(dst), f2(r.mpx).c_str(), ov.c_str(), (unsigned)r.n,
      f2(r.us_call).c_str(), note[0] ? ", " : "", note);
}

// ---- hardware clients -----------------------------------------------------------------------

ppa_client_handle_t g_srm{nullptr}, g_blend{nullptr}, g_fill{nullptr};
async_memcpy_handle_t g_mcp{nullptr};
SemaphoreHandle_t g_mcp_sem{nullptr};

bool mcp_done_cb(async_memcpy_handle_t, async_memcpy_event_t*, void* arg) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(arg), &woken);
  return woken == pdTRUE;
}

ppa_in_pic_blk_config_t ppa_in(const void* buf, int w, int h, uint32_t cm) {
  ppa_in_pic_blk_config_t c{};
  c.buffer = buf;
  c.pic_w = w;
  c.pic_h = h;
  c.block_w = w;
  c.block_h = h;
  c.cm = cm;
  return c;
}
ppa_out_pic_blk_config_t ppa_out(void* buf, size_t bytes, int w, int h, uint32_t cm) {
  ppa_out_pic_blk_config_t c{};
  c.buffer = buf;
  c.buffer_size = (bytes + 63) & ~size_t{63};
  c.pic_w = w;
  c.pic_h = h;
  c.cm = cm;
  return c;
}

// ---- LVGL off-screen drawing --------------------------------------------------------------

lv_obj_t* g_canvas{nullptr};
lv_draw_buf_t g_canvas_buf;

bool lv_target(void* dst, int w, int h) {
  if (lv_draw_buf_init(&g_canvas_buf, w, h, LV_COLOR_FORMAT_RGB565, w * 2, dst,
                       static_cast<uint32_t>(w) * h * 2) != LV_RESULT_OK)
    return false;
  lv_canvas_set_draw_buf(g_canvas, &g_canvas_buf);
  return true;
}

void img_dsc(lv_image_dsc_t& d, lv_color_format_t cf, int w, int h, int bpp, const void* data) {
  d = lv_image_dsc_t{};
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = cf;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * bpp;
  d.data_size = static_cast<uint32_t>(w) * h * bpp;
  d.data = static_cast<const uint8_t*>(data);
}

template <class F>
void lv_draw_into(F&& draw) {
  lv_layer_t layer;
  lv_canvas_init_layer(g_canvas, &layer);
  draw(&layer);
  lv_canvas_finish_layer(g_canvas, &layer);
}

alignas(64) const uint16_t kFlashImage[128 * 128] = {};  // rodata (flash), all black

// ---- operations ------------------------------------------------------------------------------

struct Ctx {
  void* src;
  void* dst;
  void* aux;  // bg copy for blends, index buffer for LUT
  int w, h;
};

struct Op {
  const char* name;
  int src_bpp;     // bytes per source pixel
  int dst_bpp;     // bytes per destination pixel
  bool src_half;   // source is (w/2) x (h/2)
  bool needs_aux;  // aux buffer of dst size
  int max_px;      // largest size (pixels) this op is run at, 0 = all
  std::function<bool(Ctx&)> run;
  std::function<int(Ctx&)> check;  // mismatching samples after one run, or -1 if none
};

uint16_t pat565(int i) {
  return static_cast<uint16_t>((i * 2654435761U) >> 7);
}

// 64 sample indices spread over n, plus first and last.
template <class F>
int sample_mismatch(size_t n, F&& bad) {
  int m = 0;
  for (int k = 0; k < 66; ++k) {
    size_t i = k == 64 ? 0 : k == 65 ? n - 1 : (static_cast<size_t>(k) * 2654435761U) % n;
    if (bad(i)) m++;
  }
  return m;
}

int ch_err(uint16_t a, uint16_t b) {
  int er = std::abs(int(a >> 11) - int(b >> 11));
  int eg = std::abs(int((a >> 5) & 63) - int((b >> 5) & 63));
  int eb = std::abs(int(a & 31) - int(b & 31));
  return std::max(er, std::max(eg, eb));
}

uint16_t lut_[256];

std::vector<Op> make_ops() {
  std::vector<Op> ops;
  auto px = [](Ctx& c) { return static_cast<size_t>(c.w) * c.h; };
  auto same_copy = [px](Ctx& c) {
    auto* s = static_cast<uint16_t*>(c.src);
    auto* d = static_cast<uint16_t*>(c.dst);
    return sample_mismatch(px(c), [&](size_t i) { return s[i] != d[i]; });
  };

  ops.push_back({"memcpy", 2, 2, false, false, 0,
                 [px](Ctx& c) {
                   memcpy(c.dst, c.src, px(c) * 2);
                   return true;
                 },
                 same_copy});
  ops.push_back({"c_copy_words", 2, 2, false, false, 0,
                 [px](Ctx& c) {
                   ::hm_kernels::copy_words(c.dst, c.src, px(c) * 2);
                   return true;
                 },
                 same_copy});
  if (::hm_kernels::pie_available()) {
    ops.push_back({"pie_copy", 2, 2, false, false, 0,
                   [px](Ctx& c) {
                     ::hm_kernels::copy_pie(c.dst, c.src, px(c) * 2);
                     return true;
                   },
                   same_copy});
  }
  // Constant-alpha RGB565 blend (out = dst, in place over a bg copy kept in aux for checking).
  auto blend_check = [px](Ctx& c) {
    auto* f = static_cast<uint16_t*>(c.src);
    auto* b = static_cast<uint16_t*>(c.aux);
    auto* d = static_cast<uint16_t*>(c.dst);
    return sample_mismatch(px(c), [&](size_t i) {
      uint16_t e;
      ::hm_kernels::blend565_c(&e, f + i, b + i, 1, 128);
      return e != d[i];
    });
  };
  ops.push_back({"c_blend565_a128", 2, 2, false, true, 0,
                 [px](Ctx& c) {
                   ::hm_kernels::blend565_c(static_cast<uint16_t*>(c.dst),
                                            static_cast<uint16_t*>(c.src),
                                            static_cast<uint16_t*>(c.aux), px(c), 128);
                   return true;
                 },
                 blend_check});
  if (::hm_kernels::pie_available()) {
    ops.push_back({"pie_blend565_a128", 2, 2, false, true, 0,
                   [px](Ctx& c) {
                     ::hm_kernels::blend565_pie(static_cast<uint16_t*>(c.dst),
                                                static_cast<uint16_t*>(c.src),
                                                static_cast<uint16_t*>(c.aux), px(c), 128);
                     return true;
                   },
                   blend_check});
  }
  // Colour-table fill: src is a byte index image (src_bpp 1).
  ops.push_back({"c_lut_fill", 1, 2, false, false, 0,
                 [px](Ctx& c) {
                   ::hm_kernels::lut_fill_c(static_cast<uint16_t*>(c.dst),
                                            static_cast<uint8_t*>(c.src), lut_, px(c));
                   return true;
                 },
                 [px](Ctx& c) {
                   auto* s = static_cast<uint8_t*>(c.src);
                   auto* d = static_cast<uint16_t*>(c.dst);
                   return sample_mismatch(px(c), [&](size_t i) { return d[i] != lut_[s[i]]; });
                 }});
  auto scale2_check = [](Ctx& c) {
    auto* s = static_cast<uint16_t*>(c.src);
    auto* d = static_cast<uint16_t*>(c.dst);
    const int sw = c.w / 2;
    return sample_mismatch(static_cast<size_t>(c.w) * c.h, [&](size_t i) {
      const int x = i % c.w, y = i / c.w;
      return d[i] != s[(y / 2) * sw + x / 2];
    });
  };
  ops.push_back({"c_scale2x", 2, 2, true, false, 0,
                 [](Ctx& c) {
                   ::hm_kernels::scale2x_c(static_cast<uint16_t*>(c.dst),
                                           static_cast<uint16_t*>(c.src), c.w, c.h);
                   return true;
                 },
                 scale2_check});
  ops.push_back({"c_rot90", 2, 2, false, false, 0,
                 [](Ctx& c) {
                   ::hm_kernels::rot90_c(static_cast<uint16_t*>(c.dst),
                                         static_cast<uint16_t*>(c.src), c.w, c.h);
                   return true;
                 },
                 nullptr});

  // ---- LVGL software (or PPA when LV_USE_PPA) drawing into an RGB565 canvas ----
  auto lv_img = [](lv_color_format_t cf, int bpp, lv_opa_t opa) {
    return [cf, bpp, opa](Ctx& c) {
      lv_image_dsc_t d;
      img_dsc(d, cf, c.w, c.h, bpp, c.src);
      lv_draw_into([&](lv_layer_t* layer) {
        lv_draw_image_dsc_t dsc;
        lv_draw_image_dsc_init(&dsc);
        dsc.src = &d;
        dsc.opa = opa;
        lv_area_t a{0, 0, c.w - 1, c.h - 1};
        lv_draw_image(layer, &dsc, &a);
      });
      return true;
    };
  };
  ops.push_back({"lvgl_img_rgb565", 2, 2, false, false, 0,
                 lv_img(LV_COLOR_FORMAT_RGB565, 2, LV_OPA_COVER), same_copy});
  ops.push_back({"lvgl_img_rgb565_opa50", 2, 2, false, true, 0,
                 lv_img(LV_COLOR_FORMAT_RGB565, 2, LV_OPA_50), nullptr});
  ops.push_back({"lvgl_img_argb8888", 4, 2, false, false, 0,
                 lv_img(LV_COLOR_FORMAT_ARGB8888, 4, LV_OPA_COVER), nullptr});
  ops.push_back({"lvgl_img_flash_rgb565", 2, 2, false, false, 128 * 128,
                 [](Ctx& c) {
                   lv_image_dsc_t d;
                   img_dsc(d, LV_COLOR_FORMAT_RGB565, c.w, c.h, 2, kFlashImage);
                   lv_draw_into([&](lv_layer_t* layer) {
                     lv_draw_image_dsc_t dsc;
                     lv_draw_image_dsc_init(&dsc);
                     dsc.src = &d;
                     lv_area_t a{0, 0, c.w - 1, c.h - 1};
                     lv_draw_image(layer, &dsc, &a);
                   });
                   return true;
                 },
                 [px](Ctx& c) {  // flash image is black; the destination was pre-filled non-black
                   auto* d = static_cast<uint16_t*>(c.dst);
                   return sample_mismatch(px(c), [&](size_t i) { return d[i] != 0; });
                 }});
  auto lv_fill = [](lv_opa_t opa) {
    return [opa](Ctx& c) {
      lv_draw_into([&](lv_layer_t* layer) {
        lv_draw_fill_dsc_t dsc;
        lv_draw_fill_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x2060C0);
        dsc.opa = opa;
        lv_area_t a{0, 0, c.w - 1, c.h - 1};
        lv_draw_fill(layer, &dsc, &a);
      });
      return true;
    };
  };
  ops.push_back({"lvgl_fill", 0, 2, false, false, 0, lv_fill(LV_OPA_COVER), [px](Ctx& c) {
                   auto* d = static_cast<uint16_t*>(c.dst);
                   const uint16_t want = lv_color_to_u16(lv_color_hex(0x2060C0));
                   return sample_mismatch(px(c), [&](size_t i) { return d[i] != want; });
                 }});
  ops.push_back({"lvgl_fill_opa50", 0, 2, false, false, 0, lv_fill(LV_OPA_50), nullptr});
  ops.push_back({"lvgl_arc_ring", 0, 2, false, false, 128 * 128,
                 [](Ctx& c) {
                   lv_draw_into([&](lv_layer_t* layer) {
                     lv_draw_arc_dsc_t dsc;
                     lv_draw_arc_dsc_init(&dsc);
                     dsc.color = lv_color_hex(0xE04030);
                     dsc.width = 6;
                     dsc.start_angle = 0;
                     dsc.end_angle = 360;
                     dsc.center = {c.w / 2, c.h / 2};
                     dsc.radius = static_cast<uint16_t>(std::min(c.w, c.h) / 2);
                     dsc.opa = LV_OPA_70;
                     lv_draw_arc(layer, &dsc);
                   });
                   return true;
                 },
                 nullptr});

  // ---- PPA ----
  ops.push_back({"ppa_fill", 0, 2, false, false, 0,
                 [px](Ctx& c) {
                   ppa_fill_oper_config_t f{};
                   f.out = ppa_out(c.dst, px(c) * 2, c.w, c.h, PPA_FILL_COLOR_MODE_RGB565);
                   f.fill_block_w = c.w;
                   f.fill_block_h = c.h;
                   f.fill_argb_color.val = 0xFF2060C0;
                   f.mode = PPA_TRANS_MODE_BLOCKING;
                   return ppa_do_fill(g_fill, &f) == ESP_OK;
                 },
                 [px](Ctx& c) {
                   auto* d = static_cast<uint16_t*>(c.dst);
                   const uint16_t want = ((0x20 >> 3) << 11) | ((0x60 >> 2) << 5) | (0xC0 >> 3);
                   return sample_mismatch(px(c), [&](size_t i) { return d[i] != want; });
                 }});
  // RGB565 foreground at constant alpha 128 over RGB565 background, written in place.
  ops.push_back({"ppa_blend565_a128", 2, 2, false, true, 0,
                 [px](Ctx& c) {
                   ppa_blend_oper_config_t b{};
                   b.in_bg = ppa_in(c.dst, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.in_fg = ppa_in(c.src, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.out = ppa_out(c.dst, px(c) * 2, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
                   b.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
                   b.fg_alpha_fix_val = 128;
                   b.mode = PPA_TRANS_MODE_BLOCKING;
                   return ppa_do_blend(g_blend, &b) == ESP_OK;
                 },
                 [px](Ctx& c) {  // within one step per channel of the C blend
                   auto* f = static_cast<uint16_t*>(c.src);
                   auto* bg = static_cast<uint16_t*>(c.aux);
                   auto* d = static_cast<uint16_t*>(c.dst);
                   return sample_mismatch(px(c), [&](size_t i) {
                     uint16_t e;
                     ::hm_kernels::blend565_c(&e, f + i, bg + i, 1, 128);
                     return ch_err(e, d[i]) > 1;
                   });
                 }});
  // ARGB8888 foreground (per-pixel alpha) over RGB565 background, in place.
  ops.push_back({"ppa_blend_argb8888", 4, 2, false, true, 0,
                 [px](Ctx& c) {
                   ppa_blend_oper_config_t b{};
                   b.in_bg = ppa_in(c.dst, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.in_fg = ppa_in(c.src, c.w, c.h, PPA_BLEND_COLOR_MODE_ARGB8888);
                   b.out = ppa_out(c.dst, px(c) * 2, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
                   b.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
                   b.mode = PPA_TRANS_MODE_BLOCKING;
                   return ppa_do_blend(g_blend, &b) == ESP_OK;
                 },
                 nullptr});
  // A8 mask in one fixed colour over RGB565 (an anti-aliased ring or a scrim), in place.
  ops.push_back({"ppa_blend_a8_colour", 1, 2, false, true, 0,
                 [px](Ctx& c) {
                   ppa_blend_oper_config_t b{};
                   b.in_bg = ppa_in(c.dst, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.in_fg = ppa_in(c.src, c.w, c.h, PPA_BLEND_COLOR_MODE_A8);
                   b.out = ppa_out(c.dst, px(c) * 2, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
                   b.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
                   b.fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
                   b.fg_fix_rgb_val.r = 0xE0;
                   b.fg_fix_rgb_val.g = 0x40;
                   b.fg_fix_rgb_val.b = 0x30;
                   b.mode = PPA_TRANS_MODE_BLOCKING;
                   return ppa_do_blend(g_blend, &b) == ESP_OK;
                 },
                 nullptr});
  auto srm = [px](ppa_srm_rotation_angle_t rot, float scale) {
    return [rot, scale, px](Ctx& c) {
      ppa_srm_oper_config_t s{};
      const int iw = scale > 1.5f ? c.w / 2 : c.w, ih = scale > 1.5f ? c.h / 2 : c.h;
      s.in = ppa_in(c.src, iw, ih, PPA_SRM_COLOR_MODE_RGB565);
      const bool swap = rot == PPA_SRM_ROTATION_ANGLE_90 || rot == PPA_SRM_ROTATION_ANGLE_270;
      s.out =
          ppa_out(c.dst, px(c) * 2, swap ? c.h : c.w, swap ? c.w : c.h, PPA_SRM_COLOR_MODE_RGB565);
      s.rotation_angle = rot;
      s.scale_x = scale;
      s.scale_y = scale;
      s.mode = PPA_TRANS_MODE_BLOCKING;
      return ppa_do_scale_rotate_mirror(g_srm, &s) == ESP_OK;
    };
  };
  ops.push_back(
      {"ppa_srm_copy", 2, 2, false, false, 0, srm(PPA_SRM_ROTATION_ANGLE_0, 1.0f), same_copy});
  ops.push_back({"ppa_srm_rot90", 2, 2, false, false, 0, srm(PPA_SRM_ROTATION_ANGLE_90, 1.0f),
                 [px](Ctx& c) {  // counter-clockwise: src (x, y) -> dst (y, w - 1 - x), dst h wide
                   auto* s = static_cast<uint16_t*>(c.src);
                   auto* d = static_cast<uint16_t*>(c.dst);
                   return sample_mismatch(px(c), [&](size_t i) {
                     const int x = i % c.w, y = i / c.w;
                     return d[static_cast<size_t>(c.w - 1 - x) * c.h + y] != s[i];
                   });
                 }});
  ops.push_back(
      {"ppa_srm_scale2x", 2, 2, true, false, 0, srm(PPA_SRM_ROTATION_ANGLE_0, 2.0f), scale2_check});

  // ---- AXI GDMA memcpy ----
  ops.push_back({"gdma_axi_memcpy", 2, 2, false, false, 0,
                 [px](Ctx& c) {
                   if (g_mcp == nullptr) return false;
                   if (esp_async_memcpy(g_mcp, c.dst, c.src, px(c) * 2, mcp_done_cb, g_mcp_sem) !=
                       ESP_OK)
                     return false;
                   return xSemaphoreTake(g_mcp_sem, pdMS_TO_TICKS(1000)) == pdTRUE;
                 },
                 same_copy});
  return ops;
}

// ---- scene ------------------------------------------------------------------------------

uint16_t* g_scene_img_px{nullptr};
lv_image_dsc_t g_scene_img;
lv_obj_t* g_card{nullptr};
lv_obj_t* g_status{nullptr};
lv_obj_t* g_ring{nullptr};
constexpr int kTiles = 9;
constexpr int kTileMax = 128;
lv_obj_t* g_tile_obj[kTiles];
lv_image_dsc_t g_tile_dsc[kTiles];
uint16_t* g_tile_px{nullptr};  // kTiles tiles of kTileMax x kTileMax, PSRAM
int64_t g_flush_t0{0}, g_flush_us{0};
uint32_t g_flush_n{0}, g_flush_px{0};

void flush_event(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_FLUSH_START) {
    g_flush_t0 = esp_timer_get_time();
    auto* a = static_cast<lv_area_t*>(lv_event_get_param(e));
    if (a != nullptr) g_flush_px += lv_area_get_size(a);
  } else {
    g_flush_us += esp_timer_get_time() - g_flush_t0;
    g_flush_n++;
  }
}

struct BufCfg {
  const char* name;
  Mem mem;
  uint32_t bytes;  // 0 = ESPHome's own buffer
};

}  // namespace

void HmBench::build_scene_() {
  lv_display_t* disp = this->lvgl_->get_disp();
  lv_obj_t* scr = lv_display_get_screen_active(disp);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  const int iw = 920, ih = 736;
  g_scene_img_px = static_cast<uint16_t*>(alloc_buf(iw * ih * 2, PS));
  if (g_scene_img_px != nullptr) {
    for (int y = 0; y < ih; ++y)
      for (int x = 0; x < iw; ++x)
        g_scene_img_px[y * iw + x] = static_cast<uint16_t>(
            ((x * 31 / iw) << 11) | ((y * 63 / ih) << 5) | (((x + y) * 31 / (iw + ih))));
    img_dsc(g_scene_img, LV_COLOR_FORMAT_RGB565, iw, ih, 2, g_scene_img_px);
    lv_obj_t* img = lv_image_create(scr);
    lv_image_set_src(img, &g_scene_img);
    lv_obj_set_pos(img, 40, 40);
  }
  // Rings and lines over the right half of the map only, so the left half is a plain image.
  for (int i = 0; i < 6; ++i) {
    lv_obj_t* arc = lv_arc_create(scr);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, 100, 100);
    lv_obj_set_pos(arc, 560 + (i % 3) * 120, 120 + (i / 3) * 200);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 0, 360);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xE04030), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_70, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    if (i == 0) g_ring = arc;
  }
  static lv_point_precise_t pts[10][2];
  for (int i = 0; i < 10; ++i) {
    pts[i][0] = {0, 0};
    pts[i][1] = {120, static_cast<lv_value_precise_t>(30 + i * 7)};
    lv_obj_t* line = lv_line_create(scr);
    lv_line_set_points(line, pts[i], 2);
    lv_obj_set_pos(line, 520 + (i % 2) * 140, 520 + i * 18);
    lv_obj_set_style_line_width(line, 3, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0xF0C040), 0);
  }
  for (int i = 0; i < 8; ++i) {
    lv_obj_t* l = lv_label_create(scr);
    lv_label_set_text_fmt(l, "Room %d  21.%d C", i + 1, i);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_pos(l, 990, 60 + i * 80);
  }
  g_status = lv_label_create(scr);
  lv_obj_set_style_text_color(g_status, lv_color_hex(0x80FF80), 0);
  lv_obj_set_pos(g_status, 990, 740);
  lv_label_set_text(g_status, "hearthmap bench: starting");

  // Opaque tile images over the plain left half of the map, as pre-rendered ring frames are.
  g_tile_px =
      static_cast<uint16_t*>(alloc_buf(static_cast<size_t>(kTiles) * kTileMax * kTileMax * 2, PS));
  for (int i = 0; i < kTiles && g_tile_px != nullptr; ++i) {
    uint16_t* t = g_tile_px + static_cast<size_t>(i) * kTileMax * kTileMax;
    for (int k = 0; k < kTileMax * kTileMax; ++k) t[k] = pat565(k + i * 977);
    img_dsc(g_tile_dsc[i], LV_COLOR_FORMAT_RGB565, 64, 64, 2, t);
    g_tile_obj[i] = lv_image_create(scr);
    lv_image_set_src(g_tile_obj[i], &g_tile_dsc[i]);
    lv_obj_set_pos(g_tile_obj[i], 60 + (i % 3) * 150, 80 + (i / 3) * 200);
  }

  g_card = lv_obj_create(scr);
  lv_obj_remove_style_all(g_card);
  lv_obj_set_size(g_card, 600, 400);
  lv_obj_center(g_card);
  lv_obj_set_style_bg_color(g_card, lv_color_hex(0x203040), 0);
  lv_obj_set_style_bg_opa(g_card, LV_OPA_60, 0);
  lv_obj_set_style_radius(g_card, 16, 0);
  lv_obj_t* cl = lv_label_create(g_card);
  lv_label_set_text(cl, "Overlay card");
  lv_obj_center(cl);
  lv_obj_add_flag(g_card, LV_OBJ_FLAG_HIDDEN);

  lv_display_add_event_cb(disp, flush_event, LV_EVENT_FLUSH_START, nullptr);
  lv_display_add_event_cb(disp, flush_event, LV_EVENT_FLUSH_FINISH, nullptr);

  // Off-screen canvas for the draw-operation measurements; its screen is never loaded.
  lv_obj_t* hidden = lv_obj_create(nullptr);
  g_canvas = lv_canvas_create(hidden);
}

void HmBench::setup() {
  ppa_client_config_t pc{};
  pc.max_pending_trans_num = 1;
  pc.oper_type = PPA_OPERATION_SRM;
  if (ppa_register_client(&pc, &g_srm) != ESP_OK) g_srm = nullptr;
  pc.oper_type = PPA_OPERATION_BLEND;
  if (ppa_register_client(&pc, &g_blend) != ESP_OK) g_blend = nullptr;
  pc.oper_type = PPA_OPERATION_FILL;
  if (ppa_register_client(&pc, &g_fill) != ESP_OK) g_fill = nullptr;
  async_memcpy_config_t mc = ASYNC_MEMCPY_DEFAULT_CONFIG();
  mc.backlog = 4;
  mc.dma_burst_size = 64;
  if (esp_async_memcpy_install_gdma_axi(&mc, &g_mcp) != ESP_OK) g_mcp = nullptr;
  g_mcp_sem = xSemaphoreCreateBinary();
  for (int i = 0; i < 256; ++i) lut_[i] = pat565(i * 7 + 3);
  this->build_scene_();
  this->build_steps_();
  this->next_run_ms_ = millis() + this->start_delay_ms_;
}

void HmBench::build_steps_() {
  this->steps_.clear();
  auto& steps = this->steps_;
  HmBench* self = this;

  steps.push_back([self]() {
    ESP_LOGI(
        TAG,
        "bench run %u start: idf %s, lvgl %d.%d.%d, LV_USE_PPA=%d LV_USE_PPA_IMG=%d, pie=%d, "
        "ppa clients srm=%d blend=%d fill=%d, gdma_axi=%d, free iram %u (largest %u), psram %u",
        (unsigned)self->run_, esp_get_idf_version(), LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
        LVGL_VERSION_PATCH, (int)LV_USE_PPA, (int)LV_USE_PPA_IMG,
        (int)::hm_kernels::pie_available(), g_srm != nullptr, g_blend != nullptr, g_fill != nullptr,
        g_mcp != nullptr, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  });

  // ---- A: raw operation rates ----
  struct Size {
    int w, h;
  };
  static const Size kSizes[] = {{2, 2}, {64, 64}, {128, 128}, {320, 240}, {920, 736}, {1280, 800}};
  static std::vector<Op> ops = make_ops();
  for (size_t oi = 0; oi < ops.size(); ++oi) {
    for (const Size& sz : kSizes) {
      const Op& op = ops[oi];
      const int px_n = sz.w * sz.h;
      if (op.max_px != 0 && px_n > op.max_px) continue;
      const bool tiny = sz.w == 2;
      const bool small = px_n <= 128 * 128 || (px_n <= 320 * 240 && op.src_bpp <= 2);
      std::vector<std::pair<Mem, Mem>> mems{{PS, PS}};
      if (!tiny && small) {
        mems.push_back({IN, IN});
        mems.push_back({PS, IN});
        mems.push_back({IN, PS});
      }
      for (auto mm : mems) {
        steps.push_back([oi, sz, mm, tiny]() {
          const Op& op = ops[oi];
          int w = sz.w, h = sz.h;
          // DMA engines want whole cache lines: the tiny case for GDMA is 32 x 2 px (128 B).
          if (tiny && strcmp(op.name, "gdma_axi_memcpy") == 0) w = 32;
          const size_t n = static_cast<size_t>(w) * h;
          const size_t src_px = op.src_half ? n / 4 : n;
          const size_t src_bytes = std::max<size_t>(src_px * std::max(op.src_bpp, 1), 64);
          const size_t dst_bytes = n * op.dst_bpp;
          void* src = op.src_bpp > 0 ? alloc_buf(src_bytes, mm.first) : nullptr;
          void* dst = alloc_buf(dst_bytes, mm.second);
          void* aux = op.needs_aux ? alloc_buf(dst_bytes, mm.second) : nullptr;
          if ((op.src_bpp > 0 && src == nullptr) || dst == nullptr ||
              (op.needs_aux && aux == nullptr)) {
            ESP_LOGI(TAG, "bench %s size=%dx%d src=%s dst=%s : skipped (alloc failed)", op.name, w,
                     h, mem_name(mm.first), mem_name(mm.second));
          } else {
            // Patterns: src pseudo-random (ARGB with varied alpha), dst/aux a different pattern.
            if (src != nullptr) {
              auto* s8 = static_cast<uint8_t*>(src);
              for (size_t i = 0; i < src_bytes; ++i)
                s8[i] = static_cast<uint8_t>((i * 2654435761U) >> 13);
            }
            auto* d16 = static_cast<uint16_t*>(dst);
            for (size_t i = 0; i < n; ++i) d16[i] = static_cast<uint16_t>(0xFFFF - i);
            if (aux != nullptr) memcpy(aux, dst, dst_bytes);
            Ctx c{src, dst, aux, w, h};
            if (strncmp(op.name, "lvgl_", 5) == 0 && !lv_target(dst, w, h)) {
              ESP_LOGI(TAG, "bench %s size=%dx%d : lv_draw_buf_init failed", op.name, w, h);
            } else {
              // Correctness on one run first (before the timed in-place repeats change dst).
              bool ok = op.run(c);
              std::string note;
              if (!ok) {
                note = "first call failed";
              } else if (op.check) {
                const int bad = op.check(c);
                note =
                    bad == 0 ? "check ok" : ("check FAIL " + std::to_string(bad) + "/66 samples");
              }
              Res r = ok ? time_op([&]() { return op.run(c); }, n) : Res{0, 0, 0, false};
              if (tiny && r.ok) g_overhead[op.name] = r.us_call;
              report(op.name, w, h, mm.first, mm.second, r, note.c_str());
            }
          }
          heap_caps_free(src);
          heap_caps_free(dst);
          heap_caps_free(aux);
          App.feed_wdt();
        });
      }
    }
  }
  steps.push_back([]() {
    ESP_LOGI(TAG,
             "bench pie_lut_fill : not written; PIE has no gather/table-lookup instruction, so a "
             "colour-table fill stays scalar (see c_lut_fill)");
  });

  // ---- Blitter: the PPA implementation against the software reference, then its rates ----
  steps.push_back([]() {
    namespace ui = ::hearthmap::ui;
    ui::SoftwareBlitter sw;
    esphome::hearthmap_panel::PpaBlitter hw(true);  // every call the PPA allows goes to it
    if (!hw.begin()) {
      ESP_LOGI(TAG, "bench blitter_check : PPA clients refused");
      return;
    }
    const int W = 320, H = 240;
    const size_t n = static_cast<size_t>(W) * H;
    auto* a = static_cast<uint16_t*>(alloc_buf(n * 2, PS));
    auto* b = static_cast<uint16_t*>(alloc_buf(n * 2, PS));
    auto* src = static_cast<uint16_t*>(alloc_buf(n * 2, PS));
    if (a == nullptr || b == nullptr || src == nullptr) return;
    for (size_t i = 0; i < n; ++i) src[i] = pat565(static_cast<int>(i) * 5 + 1);
    const ui::Surface565 sa{a, W, H, W}, sb{b, W, H, W};
    struct Case {
      const char* op;
      int x, y, w, h;
    };
    const Case cases[] = {
        {"copy", 10, 7, 101, 53},    {"copy", -20, -9, 90, 40}, {"copy", 250, 200, 100, 80},
        {"fill", 3, 5, 77, 31},      {"fill", -5, 230, 50, 50}, {"blend", 11, 13, 120, 64},
        {"blend", 280, -10, 80, 60}, {"scale2x", 8, 6, 60, 40}, {"scale2x", -4, 2, 50, 30}};
    for (const Case& cs : cases) {
      for (size_t i = 0; i < n; ++i) a[i] = b[i] = static_cast<uint16_t>(0x8410 ^ (i * 7));
      const ui::View565 v{src, cs.w, cs.h, W};
      if (strcmp(cs.op, "copy") == 0) {
        sw.copy(sa, cs.x, cs.y, v);
        hw.copy(sb, cs.x, cs.y, v);
      } else if (strcmp(cs.op, "fill") == 0) {
        sw.fill(sa, cs.x, cs.y, cs.w, cs.h, 0x3A5F);
        hw.fill(sb, cs.x, cs.y, cs.w, cs.h, 0x3A5F);
      } else if (strcmp(cs.op, "blend") == 0) {
        sw.blend(sa, cs.x, cs.y, v, 153);
        hw.blend(sb, cs.x, cs.y, v, 153);
      } else {
        sw.scale2x(sa, cs.x, cs.y, v);
        hw.scale2x(sb, cs.x, cs.y, v);
      }
      int diff = 0, maxerr = 0;
      for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
          diff++;
          maxerr = std::max(maxerr, ch_err(a[i], b[i]));
        }
      }
      ESP_LOGI(TAG,
               "bench blitter_check %s at %d,%d %dx%d : %d of %u px differ from software, max %d "
               "step(s)/channel, ppa calls %u, sw calls %u",
               cs.op, cs.x, cs.y, cs.w, cs.h, diff, (unsigned)n, maxerr, (unsigned)hw.hw_calls(),
               (unsigned)hw.sw_calls());
    }
    // Cache trap: CPU writes still in the cache, in the rows an SRM writes, outside its block.
    for (int with_writeback = 0; with_writeback < 2; ++with_writeback) {
      for (size_t i = 0; i < n; ++i) b[i] = 0x1111;
      esp_cache_msync(b, n * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      for (size_t i = 0; i < n; ++i) b[i] = 0x2222;  // dirty in cache, not yet in PSRAM
      int lost = 0;
      if (with_writeback) {
        hw.copy(sb, 100, 50, ui::View565{src, 64, 64, W});
      } else {
        ppa_srm_oper_config_t op{};
        op.in = ppa_in(src, 64, 64, PPA_SRM_COLOR_MODE_RGB565);
        op.in.pic_w = W;
        op.in.pic_h = H;
        op.out = ppa_out(b, n * 2, W, H, PPA_SRM_COLOR_MODE_RGB565);
        op.out.block_offset_x = 100;
        op.out.block_offset_y = 50;
        op.scale_x = op.scale_y = 1.0f;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        ppa_do_scale_rotate_mirror(g_srm, &op);
      }
      for (int y = 50; y < 114; ++y)
        for (int x = 0; x < W; ++x)
          if ((x < 100 || x >= 164) && b[y * W + x] != 0x2222) lost++;
      ESP_LOGI(TAG, "bench cache_trap srm %s : %d CPU-written px outside the block lost (of %d)",
               with_writeback ? "via PpaBlitter (writes rows back first)" : "raw, no write-back",
               lost, 64 * (W - 64));
    }
    heap_caps_free(a);
    heap_caps_free(b);
    heap_caps_free(src);
  });
  {
    struct Bs {
      int w, h;
    };
    static const Bs kBs[] = {{8, 8},     {32, 32},   {64, 64},   {128, 128},
                             {320, 240}, {920, 736}, {1280, 800}};
    static const char* const kOps[] = {"copy", "fill", "blend", "scale2x"};
    for (const char* opn : kOps) {
      for (const Bs& bs : kBs) {
        steps.push_back([opn, bs]() {
          namespace ui = ::hearthmap::ui;
          const int W = 1280, H = 800;
          auto* d = static_cast<uint16_t*>(alloc_buf(static_cast<size_t>(W) * H * 2, PS));
          auto* s = static_cast<uint16_t*>(alloc_buf(static_cast<size_t>(bs.w) * bs.h * 2, PS));
          if (d == nullptr || s == nullptr) {
            heap_caps_free(d);
            heap_caps_free(s);
            return;
          }
          for (int i = 0; i < bs.w * bs.h; ++i) s[i] = pat565(i);
          const ui::Surface565 dst{d, W, H, W};
          const bool sc = strcmp(opn, "scale2x") == 0;
          const ui::View565 v{s, sc ? bs.w / 2 : bs.w, sc ? bs.h / 2 : bs.h, sc ? bs.w / 2 : bs.w};
          ui::SoftwareBlitter sw;
          esphome::hearthmap_panel::PpaBlitter hw(true);  // every call the PPA allows goes to it
          hw.begin();
          for (int impl = 0; impl < 2; ++impl) {
            ui::Blitter& bl =
                impl == 0 ? static_cast<ui::Blitter&>(sw) : static_cast<ui::Blitter&>(hw);
            // Offset into the surface, as a sprite on the map would be.
            const int x = bs.w >= W ? 0 : 16, y = bs.h >= H ? 0 : 8;
            Res r = time_op(
                [&]() {
                  if (opn[0] == 'c')
                    bl.copy(dst, x, y, v);
                  else if (opn[0] == 'f')
                    bl.fill(dst, x, y, bs.w, bs.h, 0x3A5F);
                  else if (opn[0] == 'b')
                    bl.blend(dst, x, y, v, 128);
                  else
                    bl.scale2x(dst, x, y, v);
                  return true;
                },
                static_cast<uint64_t>(bs.w) * bs.h);
            char name[32];
            snprintf(name, sizeof name, "blit_%s_%s", impl == 0 ? "sw" : "ppa", opn);
            ESP_LOGI(TAG,
                     "bench %s size=%dx%d src=psram dst=psram : %s Mpx/s, %s us/call, n=%u, into a "
                     "1280x800 surface",
                     name, bs.w, bs.h, f2(r.mpx).c_str(), f2(r.us_call).c_str(), (unsigned)r.n);
          }
          heap_caps_free(d);
          heap_caps_free(s);
          App.feed_wdt();
        });
      }
    }
  }

  // ---- D: straight to the DSI frame buffer ----
  struct Fb {
    int w, h;
  };
  static const Fb kFb[] = {{2, 2}, {64, 64}, {128, 128}, {240, 320}, {736, 920}, {800, 1280}};
  for (const Fb& f : kFb) {
    for (Mem m : {PS, IN}) {
      if (m == IN && f.w * f.h > 128 * 128) continue;
      steps.push_back([self, f, m]() {
        const size_t n = static_cast<size_t>(f.w) * f.h;
        auto* src = static_cast<uint16_t*>(alloc_buf(n * 2, m));
        if (src == nullptr) return;
        for (size_t i = 0; i < n; ++i) src[i] = pat565(static_cast<int>(i));
        // ESPHome's own flush path: esp_lcd_panel_draw_bitmap, DMA2D copy into the frame buffer.
        Res r = time_op(
            [&]() {
              self->display_->draw_pixels_at(0, 0, f.w, f.h, reinterpret_cast<const uint8_t*>(src),
                                             display::COLOR_BITNESS_565, false);
              return true;
            },
            n);
        if (f.w == 2) g_overhead["dsi_draw_pixels"] = r.us_call;
        report("dsi_draw_pixels", f.w, f.h, m, PS, r,
               "native orientation, DMA2D into frame buffer");
        heap_caps_free(src);
        App.feed_wdt();
      });
    }
  }
  // PPA SRM straight into the frame buffer: 1:1, and the 90-degree rotation the landscape UI needs.
  static const Fb kLand[] = {{2, 2}, {64, 64}, {128, 128}, {320, 240}, {920, 736}, {1280, 800}};
  for (const Fb& f : kLand) {
    steps.push_back([self, f]() {
      void* fb = nullptr;
      if (esp_lcd_dpi_panel_get_frame_buffer(self->display_->handle_, 1, &fb) != ESP_OK ||
          fb == nullptr) {
        ESP_LOGI(TAG, "bench ppa_*_to_fb : no frame buffer");
        return;
      }
      const int fbw = 800, fbh = 1280;
      const size_t n = static_cast<size_t>(f.w) * f.h;
      auto* src = static_cast<uint16_t*>(alloc_buf(n * 2, PS));
      if (src == nullptr) return;
      for (size_t i = 0; i < n; ++i) src[i] = pat565(static_cast<int>(i) * 3);
      for (int rot = 0; rot < 2; ++rot) {
        const bool r90 = rot == 1;
        const char* name = r90 ? "ppa_srm_rot90_to_fb" : "ppa_srm_copy_to_fb";
        // 1:1 copies a native-orientation block (h x w swapped so it fits the 800-wide buffer).
        const int iw = r90 ? f.w : f.h, ih = r90 ? f.h : f.w;
        Res r = time_op(
            [&]() {
              ppa_srm_oper_config_t s{};
              s.in = ppa_in(src, iw, ih, PPA_SRM_COLOR_MODE_RGB565);
              s.out = ppa_out(fb, fbw * fbh * 2, fbw, fbh, PPA_SRM_COLOR_MODE_RGB565);
              s.rotation_angle = r90 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_0;
              s.scale_x = s.scale_y = 1.0f;
              s.mode = PPA_TRANS_MODE_BLOCKING;
              return ppa_do_scale_rotate_mirror(g_srm, &s) == ESP_OK;
            },
            n);
        if (f.w == 2) g_overhead[name] = r.us_call;
        report(name, f.w, f.h, PS, PS, r,
               r90 ? "landscape block rotated into frame buffer" : "native block");
      }
      // CPU copy rows into the frame buffer plus the cache write-back the DSI needs.
      {
        const int iw = f.h, ih = f.w;  // native orientation
        Res r = time_op(
            [&]() {
              auto* d = static_cast<uint16_t*>(fb);
              for (int y = 0; y < ih; ++y)
                memcpy(d + static_cast<size_t>(y) * fbw, src + static_cast<size_t>(y) * iw, iw * 2);
              esp_cache_msync(fb, static_cast<size_t>(ih) * fbw * 2,
                              ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
              return true;
            },
            n);
        if (f.w == 2) g_overhead["cpu_rows_to_fb"] = r.us_call;
        report("cpu_rows_to_fb", f.w, f.h, PS, PS, r, "memcpy per row + cache write-back");
      }
      heap_caps_free(src);
      App.feed_wdt();
    });
  }
  steps.push_back([self]() {  // repaint what the frame-buffer tests scribbled over
    lv_obj_invalidate(lv_display_get_screen_active(self->lvgl_->get_disp()));
  });

  // ---- Tiles: what one small opaque tile costs through LVGL, and through a direct present ----
  static const int kTileSizes[] = {32, 64, 96, 128};
  static const BufCfg kTileBufs[] = {{"esphome_default", PS, 0}, {"iram_256000", IN, 256000}};
  for (const BufCfg& bc : kTileBufs) {
    steps.push_back([self, bc]() {
      lv_display_t* disp = self->lvgl_->get_disp();
      lv_obj_t* scr = lv_display_get_screen_active(disp);
      lv_draw_buf_t* orig = lv_display_get_buf_active(disp);
      void* orig_data = orig != nullptr ? orig->data : nullptr;
      const uint32_t orig_size = orig != nullptr ? orig->data_size : 0;
      void* buf = nullptr;
      if (bc.bytes != 0) {
        buf = alloc_buf(bc.bytes, bc.mem);
        if (buf == nullptr) return;
        lv_display_set_buffers(disp, buf, nullptr, bc.bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
      }
      lv_refr_now(disp);
      // Per-refresh fixed cost: a refresh with nothing invalid, and with a 2x2 area.
      {
        const int reps = 50;
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < reps; ++i) lv_refr_now(disp);
        const double empty_us = (esp_timer_get_time() - t0) / double(reps);
        g_flush_us = 0;
        g_flush_n = 0;
        t0 = esp_timer_get_time();
        for (int i = 0; i < reps; ++i) {
          lv_area_t a{70, 90, 71, 91};
          lv_obj_invalidate_area(scr, &a);
          lv_refr_now(disp);
        }
        const double tiny_us = (esp_timer_get_time() - t0) / double(reps);
        ESP_LOGI(TAG,
                 "bench tile lvgl_refresh buf=%s : empty %s us, one 2x2 area %s us (flush %s us in "
                 "%u calls), n=%d",
                 bc.name, f2(empty_us).c_str(), f2(tiny_us).c_str(),
                 f2(g_flush_us / double(reps)).c_str(), (unsigned)(g_flush_n / reps), reps);
      }
      for (int sz : kTileSizes) {
        for (int i = 0; i < kTiles; ++i) {
          g_tile_dsc[i].header.w = sz;
          g_tile_dsc[i].header.h = sz;
          g_tile_dsc[i].header.stride = sz * 2;
          g_tile_dsc[i].data_size = sz * sz * 2;
          lv_image_set_src(g_tile_obj[i], &g_tile_dsc[i]);
          lv_obj_set_size(g_tile_obj[i], sz, sz);
        }
        lv_refr_now(disp);
        for (int ntiles : {1, kTiles}) {
          const int reps = 20;
          g_flush_us = 0;
          g_flush_n = 0;
          g_flush_px = 0;
          int64_t total = 0;
          for (int r = 0; r < reps; ++r) {
            for (int i = 0; i < ntiles; ++i) lv_obj_invalidate(g_tile_obj[i]);
            const int64_t t0 = esp_timer_get_time();
            lv_refr_now(disp);
            total += esp_timer_get_time() - t0;
          }
          const double per_tile = total / double(reps) / ntiles;
          ESP_LOGI(TAG,
                   "bench tile lvgl_invalidate size=%dx%d tiles=%d buf=%s : %s us per tile (flush "
                   "%s us per tile, %u flush calls per refresh), n=%d",
                   sz, sz, ntiles, bc.name, f2(per_tile).c_str(),
                   f2(g_flush_us / double(reps) / ntiles).c_str(), (unsigned)(g_flush_n / reps),
                   reps);
          App.feed_wdt();
        }
      }
      for (int i = 0; i < kTiles; ++i) {
        g_tile_dsc[i].header.w = 64;
        g_tile_dsc[i].header.h = 64;
        g_tile_dsc[i].header.stride = 128;
        g_tile_dsc[i].data_size = 64 * 64 * 2;
        lv_image_set_src(g_tile_obj[i], &g_tile_dsc[i]);
        lv_obj_set_size(g_tile_obj[i], 64, 64);
      }
      if (bc.bytes != 0) {
        lv_display_set_buffers(disp, orig_data, nullptr, orig_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
        heap_caps_free(buf);
      }
      lv_obj_invalidate(scr);
      lv_refr_now(disp);
    });
  }
  // Direct present: rotate the landscape tile on the CPU, then put it on the panel either through
  // the display's public draw_pixels_at (DMA2D) or by writing the frame buffer rows directly.
  steps.push_back([self]() {
    void* fbv = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(self->display_->handle_, 1, &fbv) != ESP_OK ||
        fbv == nullptr)
      return;
    auto* fb = static_cast<uint16_t*>(fbv);
    const int fbw = 800;
    auto* rot = static_cast<uint16_t*>(alloc_buf(kTileMax * kTileMax * 2, IN));
    auto* rot_ps = static_cast<uint16_t*>(alloc_buf(kTileMax * kTileMax * 2, PS));
    if (rot == nullptr || rot_ps == nullptr || g_tile_px == nullptr) return;
    for (int sz : kTileSizes) {
      const uint16_t* src = g_tile_px;  // tile 0, landscape sz x sz, PSRAM
      // Landscape (lx, ly) -> native (799 - ly, lx) for the 90-degree panel rotation: a tile at
      // landscape (x, y) lands at native x = 800 - y - sz, native y = x.
      const int lx = 60, ly = 80;
      const int nx = fbw - ly - sz, ny = lx;
      for (int mode = 0; mode < 4; ++mode) {
        const char* name = mode == 0   ? "present_rot_iram_draw_pixels"
                           : mode == 1 ? "present_rot_psram_draw_pixels"
                           : mode == 2 ? "present_rot_direct_fb"
                                       : "present_rot_only";
        Res r = time_op(
            [&]() {
              if (mode == 2) {
                // Rotate straight into the frame buffer rows, keeping the cache coherent with
                // DMA2D's writes: invalidate the rows' lines first, write, write them back.
                for (int y = 0; y < sz; ++y) {
                  uint16_t* row = fb + static_cast<size_t>(ny + y) * fbw + nx;
                  uintptr_t a0 = reinterpret_cast<uintptr_t>(row) & ~uintptr_t{63};
                  uintptr_t a1 = (reinterpret_cast<uintptr_t>(row + sz) + 63) & ~uintptr_t{63};
                  esp_cache_msync(reinterpret_cast<void*>(a0), a1 - a0,
                                  ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                  // native row y holds landscape column (ny + y - lx) = y, read bottom-up
                  for (int x = 0; x < sz; ++x)
                    row[x] = src[static_cast<size_t>(sz - 1 - x) * sz + y];
                  esp_cache_msync(reinterpret_cast<void*>(a0), a1 - a0,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                }
                return true;
              }
              uint16_t* dst = mode == 1 ? rot_ps : rot;
              ::hm_kernels::rot90_c(dst, src, sz, sz);
              if (mode == 3) return true;
              self->display_->draw_pixels_at(nx, ny, sz, sz, reinterpret_cast<const uint8_t*>(dst),
                                             display::COLOR_BITNESS_565, false);
              return true;
            },
            static_cast<uint64_t>(sz) * sz, 20000);
        ESP_LOGI(TAG, "bench tile %s size=%dx%d : %s us per tile, %s Mpx/s, n=%u", name, sz, sz,
                 f2(r.us_call).c_str(), f2(r.mpx).c_str(), (unsigned)r.n);
      }
      App.feed_wdt();
    }
    heap_caps_free(rot);
    heap_caps_free(rot_ps);
    lv_obj_invalidate(lv_display_get_screen_active(self->lvgl_->get_disp()));
  });

  // ---- B/C: scene repaints under several draw-buffer placements ----
  static const BufCfg kBufs[] = {{"esphome_default", PS, 0},  {"psram_256000", PS, 256000},
                                 {"iram_256000", IN, 256000}, {"psram_128000", PS, 128000},
                                 {"iram_128000", IN, 128000}, {"iram_64000", IN, 64000}};
  for (const BufCfg& bc : kBufs) {
    steps.push_back([self, bc]() {
      lv_display_t* disp = self->lvgl_->get_disp();
      lv_obj_t* scr = lv_display_get_screen_active(disp);
      lv_draw_buf_t* orig = lv_display_get_buf_active(disp);
      void* orig_data = orig != nullptr ? orig->data : nullptr;
      const uint32_t orig_size = orig != nullptr ? orig->data_size : 0;
      void* buf = nullptr;
      if (bc.bytes != 0) {
        buf = alloc_buf(bc.bytes, bc.mem);
        if (buf == nullptr) {
          ESP_LOGI(
              TAG, "bench scene * buf=%s : skipped (alloc failed, free iram %u)", bc.name,
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
          return;
        }
        lv_display_set_buffers(disp, buf, nullptr, bc.bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
      }
      struct Case {
        const char* name;
        lv_area_t area;
        bool card;
      };
      lv_area_t ring{};
      if (g_ring != nullptr) lv_obj_get_coords(g_ring, &ring);
      const Case cases[] = {
          {"full_screen", {0, 0, 1279, 799}, false},
          {"map_920x736", {40, 40, 959, 775}, false},
          {"plain_image_400x300", {60, 60, 459, 359}, false},
          {"overlay_card_600x400", {340, 200, 939, 599}, true},
          {"ring_100x100", ring, false},
      };
      for (const Case& cs : cases) {
        if (cs.card) {
          lv_obj_remove_flag(g_card, LV_OBJ_FLAG_HIDDEN);
          lv_refr_now(disp);
        }
        const int reps = 5;
        int64_t total = 0;
        g_flush_us = 0;
        g_flush_n = 0;
        g_flush_px = 0;
        for (int i = 0; i < reps; ++i) {
          lv_obj_invalidate_area(scr, &cs.area);
          const int64_t t0 = esp_timer_get_time();
          lv_refr_now(disp);
          total += esp_timer_get_time() - t0;
          App.feed_wdt();
        }
        if (cs.card) {
          lv_obj_add_flag(g_card, LV_OBJ_FLAG_HIDDEN);
          lv_refr_now(disp);
        }
        const double ms = total / 1000.0 / reps, fl = g_flush_us / 1000.0 / reps;
        const double px_per = static_cast<double>(lv_area_get_size(&cs.area));
        ESP_LOGI(TAG,
                 "bench scene %s buf=%s : %s ms/refresh (render %s ms, flush %s ms in %u calls), "
                 "%s Mpx/s "
                 "rendered, %u kpx flushed, n=%d",
                 cs.name, bc.name, f2(ms).c_str(), f2(ms - fl).c_str(), f2(fl).c_str(),
                 (unsigned)(g_flush_n / reps),
                 f2(ms > fl ? px_per / ((ms - fl) * 1000.0) : 0).c_str(),
                 (unsigned)(g_flush_px / reps / 1000), reps);
      }
      if (bc.bytes != 0) {
        lv_display_set_buffers(disp, orig_data, nullptr, orig_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
        heap_caps_free(buf);
      }
    });
  }
  steps.push_back([self]() { ESP_LOGI(TAG, "bench run %u done", (unsigned)self->run_); });
}

void HmBench::loop() {
  const uint32_t now = millis();
  if (this->step_ == 0) {
    if (static_cast<int32_t>(now - this->next_run_ms_) < 0) return;
  }
  if (this->step_ < this->steps_.size()) {
    if (g_status != nullptr && (this->step_ % 16) == 0) {
      lv_label_set_text_fmt(g_status, "hearthmap bench: run %u, step %u/%u", (unsigned)this->run_,
                            (unsigned)this->step_, (unsigned)this->steps_.size());
    }
    this->steps_[this->step_++]();
    return;
  }
  this->step_ = 0;
  this->run_++;
  this->next_run_ms_ = now + this->period_ms_;
  if (g_status != nullptr)
    lv_label_set_text_fmt(g_status, "hearthmap bench: run %u done, next in %us",
                          (unsigned)(this->run_ - 1), (unsigned)(this->period_ms_ / 1000));
}

}  // namespace esphome::hm_bench
