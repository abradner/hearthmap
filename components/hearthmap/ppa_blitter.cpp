#include "ppa_blitter.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "esphome/core/hal.h"
#include "loop_breadcrumb.h"

namespace esphome::hearthmap_panel {

namespace ui = ::hearthmap::ui;

namespace {
constexpr uintptr_t kLine = 64;  // PPA output alignment: L1 and L2 cache line on this build

ppa_in_pic_blk_config_t in_block(const ui::View565& v, int sx, int sy, int w, int h, uint32_t cm) {
  ppa_in_pic_blk_config_t c{};
  c.buffer = v.px;
  c.pic_w = static_cast<uint32_t>(v.stride);
  c.pic_h = static_cast<uint32_t>(v.h);
  c.block_w = static_cast<uint32_t>(w);
  c.block_h = static_cast<uint32_t>(h);
  c.block_offset_x = static_cast<uint32_t>(sx);
  c.block_offset_y = static_cast<uint32_t>(sy);
  c.cm = cm;
  return c;
}

ppa_out_pic_blk_config_t out_block(const ui::Surface565& s, int x, int y, uint32_t cm) {
  ppa_out_pic_blk_config_t c{};
  c.buffer = s.px;
  c.buffer_size = static_cast<uint32_t>(s.stride) * static_cast<uint32_t>(s.h) * 2U;
  c.pic_w = static_cast<uint32_t>(s.stride);
  c.pic_h = static_cast<uint32_t>(s.h);
  c.block_offset_x = static_cast<uint32_t>(x);
  c.block_offset_y = static_cast<uint32_t>(y);
  c.cm = cm;
  return c;
}

ui::View565 as_view(const ui::Surface565& s) {
  return {s.px, s.w, s.h, s.stride};
}
}  // namespace

PpaBlitter::~PpaBlitter() {
  if (blend_ != nullptr) ppa_unregister_client(blend_);
  if (fill_ != nullptr) ppa_unregister_client(fill_);
}

bool PpaBlitter::begin() {
  ppa_client_config_t cfg{};
  cfg.max_pending_trans_num = 1;
  cfg.oper_type = PPA_OPERATION_BLEND;
  if (ppa_register_client(&cfg, &blend_) != ESP_OK) blend_ = nullptr;
  cfg.oper_type = PPA_OPERATION_FILL;
  if (ppa_register_client(&cfg, &fill_) != ESP_OK) fill_ = nullptr;
  return blend_ != nullptr && fill_ != nullptr;
}

bool PpaBlitter::big_enough_(const ui::BlitClip& c, uint32_t min_px) const {
  return force_hw_ || static_cast<uint32_t>(c.w) * static_cast<uint32_t>(c.h) >= min_px;
}

bool PpaBlitter::dst_ok_(const ui::Surface565& dst) const {
  const auto addr = reinterpret_cast<uintptr_t>(dst.px);
  const uint32_t bytes = static_cast<uint32_t>(dst.stride) * static_cast<uint32_t>(dst.h) * 2U;
  return (addr % kLine) == 0 && (bytes % kLine) == 0 &&
         (esp_ptr_internal(dst.px) || esp_ptr_external_ram(dst.px));
}

bool PpaBlitter::src_ok_(const ui::View565& src) {
  return esp_ptr_internal(src.px) || esp_ptr_external_ram(src.px);
}

void PpaBlitter::writeback_rows_(const ui::Surface565& dst, int y, int h) const {
  auto* start = reinterpret_cast<uint8_t*>(dst.px + static_cast<ptrdiff_t>(y) * dst.stride);
  const size_t len = static_cast<size_t>(h) * static_cast<size_t>(dst.stride) * 2U;
  esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void PpaBlitter::copy(const ui::Surface565& dst, int x, int y, const ui::View565& src) {
  const ui::BlitClip c = ui::clip_blit(dst, x, y, src.w, src.h);
  if (c.empty() || src.px == nullptr) return;
  if (blend_ != nullptr && big_enough_(c, kCopyMinPx) && dst_ok_(dst) && src_ok_(src)) {
    // The blend engine as a copy: the source is the background, and the foreground (the same
    // memory read as A8, any readable bytes would do) is made fully transparent, so the output
    // is the background exactly. LVGL 9.6's PPA image path does the same (lv_draw_ppa_img.c).
    ppa_blend_oper_config_t op{};
    op.in_bg = in_block(src, c.src_x, c.src_y, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
    op.in_fg = in_block(src, c.src_x, c.src_y, c.w, c.h, PPA_BLEND_COLOR_MODE_A8);
    op.out = out_block(dst, c.x, c.y, PPA_BLEND_COLOR_MODE_RGB565);
    op.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.bg_alpha_fix_val = 0xFF;
    op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.fg_alpha_fix_val = 0;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    writeback_rows_(dst, c.y, c.h);
    const CrumbScope crumb(Crumb::PpaCopy, millis());
    if (ppa_do_blend(blend_, &op) == ESP_OK) {
      hw_calls_++;
      return;
    }
  }
  sw_calls_++;
  SoftwareBlitter::copy(dst, x, y, src);
}

void PpaBlitter::fill(const ui::Surface565& dst, int x, int y, int w, int h, std::uint16_t colour) {
  const ui::BlitClip c = ui::clip_blit(dst, x, y, w, h);
  if (c.empty()) return;
  if (fill_ != nullptr && big_enough_(c, kFillMinPx) && dst_ok_(dst)) {
    ppa_fill_oper_config_t op{};
    op.out = out_block(dst, c.x, c.y, PPA_FILL_COLOR_MODE_RGB565);
    op.fill_block_w = static_cast<uint32_t>(c.w);
    op.fill_block_h = static_cast<uint32_t>(c.h);
    // RGB565 -> ARGB8888 by bit replication; the PPA truncates back, so the colour is exact.
    const uint32_t r5 = colour >> 11U, g6 = (colour >> 5U) & 0x3FU, b5 = colour & 0x1FU;
    op.fill_argb_color.val = 0xFF000000U | (((r5 << 3U) | (r5 >> 2U)) << 16U) |
                             (((g6 << 2U) | (g6 >> 4U)) << 8U) | ((b5 << 3U) | (b5 >> 2U));
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const CrumbScope crumb(Crumb::PpaFill, millis());
    if (ppa_do_fill(fill_, &op) == ESP_OK) {
      hw_calls_++;
      return;
    }
  }
  sw_calls_++;
  SoftwareBlitter::fill(dst, x, y, w, h, colour);
}

void PpaBlitter::blend(const ui::Surface565& dst, int x, int y, const ui::View565& src,
                       std::uint8_t alpha) {
  if (alpha == 0 || alpha == 255 || src.px == nullptr) {
    // Nothing, or a copy: let copy() choose.
    if (alpha == 255) {
      copy(dst, x, y, src);
    }
    return;
  }
  const ui::BlitClip c = ui::clip_blit(dst, x, y, src.w, src.h);
  if (c.empty()) return;
  if (blend_ != nullptr && big_enough_(c, kBlendMinPx) && dst_ok_(dst) && src_ok_(src)) {
    ppa_blend_oper_config_t op{};
    op.in_bg = in_block(as_view(dst), c.x, c.y, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
    op.in_fg = in_block(src, c.src_x, c.src_y, c.w, c.h, PPA_BLEND_COLOR_MODE_RGB565);
    op.out = out_block(dst, c.x, c.y, PPA_BLEND_COLOR_MODE_RGB565);
    op.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.fg_alpha_fix_val = alpha;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const CrumbScope crumb(Crumb::PpaBlend, millis());
    if (ppa_do_blend(blend_, &op) == ESP_OK) {
      hw_calls_++;
      return;
    }
  }
  sw_calls_++;
  SoftwareBlitter::blend(dst, x, y, src, alpha);
}

void PpaBlitter::scale2x(const ui::Surface565& dst, int x, int y, const ui::View565& src) {
  // Software only: the PPA's scaler interpolates, which is not pixel doubling (header).
  sw_calls_++;
  SoftwareBlitter::scale2x(dst, x, y, src);
}

}  // namespace esphome::hearthmap_panel

#endif  // USE_ESP32_VARIANT_ESP32P4
