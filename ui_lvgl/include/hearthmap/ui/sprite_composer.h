// sprite_composer.h — composing a pulse sprite's frame (docs/decisions.md D-053, D-054): a
// per-pixel blend from look-up tables, kept separate from the Blitter (blitter.h), which carries
// the plain moves (copy, fill, constant-alpha blend, scale). The default is plain C++
// (cpu_sprite_composer()); a target with SIMD can pass its own through ScreenDeps. Pixels are
// RGB565, row-major, `stride` in pixels.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hearthmap::ui {

/// One colour laid over a tile with a coverage mask and an opacity (a room's outline).
struct Overlay {
  const std::uint8_t* coverage = nullptr;  ///< Per tile pixel, 0..255.
  std::uint16_t colour = 0;                ///< RGB565.
  std::uint8_t opa = 0;                    ///< 0..255; 0 draws nothing.
};

/// One ring laid over a tile: the tile's distances to the ring's centre and the ring's current
/// alpha by distance.
struct RingLayer {
  const std::uint16_t* distance = nullptr;  ///< Per tile pixel, 1/16 px.
  const std::uint8_t* alpha = nullptr;      ///< By distance; nullptr: nothing drawn.
  std::size_t size = 0;
  std::uint16_t colour = 0;  ///< RGB565.
};

/// Compose the rows [y0, y1) and columns [x0, x1) of a `stride`-wide tile: where `full` and
/// `below` differ the pixel belongs to something drawn over the pulses (a pin) and is copied from
/// `full`; elsewhere `below` gets each overlay in order, then each ring layer in its colour.
struct ComposeJob {
  const std::uint16_t* below = nullptr;
  const std::uint16_t* full = nullptr;
  std::uint16_t* out = nullptr;
  std::int32_t stride = 0;
  std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  const Overlay* overlays = nullptr;
  std::size_t overlay_count = 0;
  const RingLayer* rings = nullptr;
  std::size_t ring_count = 0;
};

class SpriteComposer {
 public:
  virtual ~SpriteComposer() = default;
  virtual void compose(const ComposeJob& job) const;
};

/// The plain C++ implementation.
const SpriteComposer& cpu_sprite_composer();

/// `bg` with `fg` over it at alpha `a` (0..255), per 5/6/5 channel.
inline std::uint16_t blend565(std::uint16_t bg, std::uint16_t fg, std::uint32_t a) {
  if (a == 0) return bg;
  if (a >= 255) return fg;
  // Spread the channels so one multiply blends all three: green in the high half-word.
  const std::uint32_t b = (bg | (static_cast<std::uint32_t>(bg) << 16)) & 0x07E0F81FU;
  const std::uint32_t f = (fg | (static_cast<std::uint32_t>(fg) << 16)) & 0x07E0F81FU;
  const std::uint32_t a5 = (a + 4U) >> 3;  // 0..32
  const std::uint32_t r = (b + (((f - b) * a5) >> 5)) & 0x07E0F81FU;
  return static_cast<std::uint16_t>(r | (r >> 16));
}

/// RGB565 of a 0xRRGGBB colour.
inline std::uint16_t rgb565(std::uint32_t rgb) {
  return static_cast<std::uint16_t>(((rgb >> 8U) & 0xF800U) | ((rgb >> 5U) & 0x07E0U) |
                                    ((rgb >> 3U) & 0x001FU));
}

}  // namespace hearthmap::ui
