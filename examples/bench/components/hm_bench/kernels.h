// Pixel kernels the benchmark times: plain C versions, and PIE (ESP32-P4 "xesppie") versions
// where the instruction set fits. Pure functions over caller-owned buffers; no allocation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hm_kernels {

// Word-at-a-time copy, eight 32-bit words per iteration. `bytes` may be any size.
void copy_words(void* dst, const void* src, std::size_t bytes);

// Constant-alpha blend of RGB565 `fg` over `bg` into `out` (out may equal bg), alpha 0..256.
// Per channel c with field mask M: out_c = (((fg & M) * a) >> 8) + (((bg & M) * (256 - a)) >> 8),
// masked to M. The PIE version computes exactly this, so the two must agree bit for bit.
void blend565_c(std::uint16_t* out, const std::uint16_t* fg, const std::uint16_t* bg, std::size_t n,
                unsigned alpha);

// Colour-table fill: out[i] = lut[idx[i]].
void lut_fill_c(std::uint16_t* out, const std::uint8_t* idx, const std::uint16_t* lut,
                std::size_t n);

// Pixel doubling: src is (w/2) x (h/2), dst is w x h (w, h even), both tightly packed.
void scale2x_c(std::uint16_t* dst, const std::uint16_t* src, int w, int h);

// 90-degree clockwise rotation of a w x h image into an h x w image (as the lvgl component's
// software fallback does for rotation 90).
void rot90_c(std::uint16_t* dst, const std::uint16_t* src, int w, int h);

// True when the PIE kernels below were compiled in (ESP32-P4 GCC with the xesppie extension).
bool pie_available();

// 128-bit vector copy. dst and src 16-byte aligned; any size (tail in C).
void copy_pie(void* dst, const void* src, std::size_t bytes);

// PIE version of blend565_c. out, fg, bg 16-byte aligned; any n (tail in C).
void blend565_pie(std::uint16_t* out, const std::uint16_t* fg, const std::uint16_t* bg,
                  std::size_t n, unsigned alpha);

}  // namespace hm_kernels
