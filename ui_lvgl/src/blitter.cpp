#include "hearthmap/ui/blitter.h"

#include <algorithm>
#include <cstring>

namespace hearthmap::ui {

BlitClip clip_blit(const Surface565& dst, int x, int y, int src_w, int src_h, int scale) {
  BlitClip c;
  if (dst.px == nullptr || src_w <= 0 || src_h <= 0 || dst.w <= 0 || dst.h <= 0 || scale < 1) {
    return c;
  }
  // Skip whole source pixels that fall left of / above the destination.
  const int skip_x = x < 0 ? (-x + scale - 1) / scale : 0;
  const int skip_y = y < 0 ? (-y + scale - 1) / scale : 0;
  if (skip_x >= src_w || skip_y >= src_h) return c;
  c.src_x = skip_x;
  c.src_y = skip_y;
  c.x = x + skip_x * scale;
  c.y = y + skip_y * scale;
  if (c.x >= dst.w || c.y >= dst.h) return BlitClip{};
  c.w = std::min((src_w - skip_x) * scale, dst.w - c.x);
  c.h = std::min((src_h - skip_y) * scale, dst.h - c.y);
  return c;
}

void SoftwareBlitter::copy(const Surface565& dst, int x, int y, const View565& src) {
  if (src.px == nullptr) return;
  const BlitClip c = clip_blit(dst, x, y, src.w, src.h);
  if (c.empty()) return;
  for (int row = 0; row < c.h; ++row) {
    std::memcpy(dst.px + static_cast<std::ptrdiff_t>(c.y + row) * dst.stride + c.x,
                src.px + static_cast<std::ptrdiff_t>(c.src_y + row) * src.stride + c.src_x,
                static_cast<std::size_t>(c.w) * sizeof(std::uint16_t));
  }
}

void SoftwareBlitter::fill(const Surface565& dst, int x, int y, int w, int h,
                           std::uint16_t colour) {
  const BlitClip c = clip_blit(dst, x, y, w, h);
  if (c.empty()) return;
  for (int row = 0; row < c.h; ++row) {
    std::uint16_t* p = dst.px + static_cast<std::ptrdiff_t>(c.y + row) * dst.stride + c.x;
    std::fill(p, p + c.w, colour);
  }
}

namespace {
inline unsigned mix(unsigned s, unsigned d, unsigned a) {
  return (s * a + d * (255U - a) + 127U) / 255U;
}
}  // namespace

void SoftwareBlitter::blend(const Surface565& dst, int x, int y, const View565& src,
                            std::uint8_t alpha) {
  if (alpha == 0 || src.px == nullptr) return;
  if (alpha == 255) {
    copy(dst, x, y, src);
    return;
  }
  const BlitClip c = clip_blit(dst, x, y, src.w, src.h);
  if (c.empty()) return;
  const unsigned a = alpha;
  for (int row = 0; row < c.h; ++row) {
    std::uint16_t* d = dst.px + static_cast<std::ptrdiff_t>(c.y + row) * dst.stride + c.x;
    const std::uint16_t* s =
        src.px + static_cast<std::ptrdiff_t>(c.src_y + row) * src.stride + c.src_x;
    for (int i = 0; i < c.w; ++i) {
      const unsigned sp = s[i];
      const unsigned dp = d[i];
      const unsigned r = mix(sp >> 11U, dp >> 11U, a);
      const unsigned g = mix((sp >> 5U) & 0x3FU, (dp >> 5U) & 0x3FU, a);
      const unsigned b = mix(sp & 0x1FU, dp & 0x1FU, a);
      d[i] = static_cast<std::uint16_t>((r << 11U) | (g << 5U) | b);
    }
  }
}

void SoftwareBlitter::scale2x(const Surface565& dst, int x, int y, const View565& src) {
  if (src.px == nullptr) return;
  const BlitClip c = clip_blit(dst, x, y, src.w, src.h, 2);
  if (c.empty()) return;
  for (int row = 0; row < c.h; ++row) {
    const std::uint16_t* s =
        src.px + static_cast<std::ptrdiff_t>(c.src_y + row / 2) * src.stride + c.src_x;
    std::uint16_t* d = dst.px + static_cast<std::ptrdiff_t>(c.y + row) * dst.stride + c.x;
    if ((row & 1) != 0) {
      // The odd row repeats the even row above it, which is inside the clip.
      std::memcpy(d, d - dst.stride, static_cast<std::size_t>(c.w) * sizeof(std::uint16_t));
      continue;
    }
    for (int i = 0; i < c.w; ++i) d[i] = s[i / 2];
  }
}

}  // namespace hearthmap::ui
