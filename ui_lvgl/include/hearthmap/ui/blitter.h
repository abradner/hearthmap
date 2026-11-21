// blitter.h — the one interface every large pixel move goes through, so a board's accelerator
// (the ESP32-P4's PPA, a SIMD loop) can replace the plain C++ loops without the UI knowing.
//
// RGB565 only, little-endian, as LVGL draws for the panel. No LVGL, no allocation, no clock: a
// surface is caller-owned memory. SoftwareBlitter is the default and what the host uses; a target
// supplies a faster implementation at build time and loses only speed without one
// (docs/decisions.md D-054, D-057).
#pragma once

#include <cstdint>

namespace hearthmap::ui {

/// A writable RGB565 image: `stride` is in pixels (>= w).
struct Surface565 {
  std::uint16_t* px = nullptr;
  int w = 0;
  int h = 0;
  int stride = 0;
};

/// A read-only RGB565 image: `stride` is in pixels (>= w).
struct View565 {
  const std::uint16_t* px = nullptr;
  int w = 0;
  int h = 0;
  int stride = 0;
};

/// Pixel moves on RGB565 surfaces. Every operation clips to `dst` (a destination origin may be
/// negative or past the edge; nothing outside `dst` is written) and is a no-op for an empty
/// source or an empty clip. Source and destination must not overlap.
class Blitter {
 public:
  virtual ~Blitter() = default;

  /// Opaque copy of `src` with its top-left at (x, y) in `dst`.
  virtual void copy(const Surface565& dst, int x, int y, const View565& src) = 0;

  /// Fill the w x h rectangle at (x, y) with `colour`.
  virtual void fill(const Surface565& dst, int x, int y, int w, int h, std::uint16_t colour) = 0;

  /// `src` over `dst` at (x, y) at constant opacity `alpha` (0 leaves dst, 255 is a copy). Per
  /// channel: out = (src * alpha + dst * (255 - alpha) + 127) / 255, rounded to the nearest. An
  /// accelerated implementation may differ from this by at most one step per channel.
  virtual void blend(const Surface565& dst, int x, int y, const View565& src,
                     std::uint8_t alpha) = 0;

  /// `src` doubled in both directions (each pixel becomes 2 x 2) with its top-left at (x, y).
  virtual void scale2x(const Surface565& dst, int x, int y, const View565& src) = 0;
};

/// Plain C++ loops. Exact, portable, and the reference the accelerated versions are tested
/// against.
class SoftwareBlitter : public Blitter {
 public:
  void copy(const Surface565& dst, int x, int y, const View565& src) override;
  void fill(const Surface565& dst, int x, int y, int w, int h, std::uint16_t colour) override;
  void blend(const Surface565& dst, int x, int y, const View565& src, std::uint8_t alpha) override;
  void scale2x(const Surface565& dst, int x, int y, const View565& src) override;
};

/// The clipped part of an operation: `src_x`, `src_y` is the first source pixel used, `x`, `y`
/// its destination, `w`, `h` the size in destination pixels. Empty when w or h is 0. Shared by
/// implementations so they all clip the same way. `scale` is 1, or 2 for scale2x (then src_x and
/// src_y count source pixels and the destination block starts on a whole source pixel).
struct BlitClip {
  int src_x = 0;
  int src_y = 0;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};
BlitClip clip_blit(const Surface565& dst, int x, int y, int src_w, int src_h, int scale = 1);

}  // namespace hearthmap::ui
