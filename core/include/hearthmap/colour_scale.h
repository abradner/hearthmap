// colour_scale.h — value -> RGB565, quiet inside the comfort band.
//
// A diverging scale: the neutral colour anywhere inside [comfort_min, comfort_max]; towards the
// cold colour below it, saturating at scale_min; towards the hot colour above it, saturating at
// scale_max. Severity does not affect colour (pins and pulsing carry severity).
#pragma once

#include <cstdint>

#include "hearthmap/config.h"

namespace hearthmap {

/// 8-bit-per-channel colour, for palettes and arithmetic.
struct Rgb888 {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
};

/// 16-bit colour, R in bits 15..11, G in 10..5, B in 4..0, native integer (not byte-swapped).
/// Byte order on the wire is the display driver's business, not core's.
using Rgb565 = std::uint16_t;

/// Pack an Rgb888 into Rgb565 by truncating each channel's low bits.
constexpr Rgb565 to_rgb565(Rgb888 c) {
  return static_cast<Rgb565>(((c.r & 0xF8U) << 8U) | ((c.g & 0xFCU) << 3U) | (c.b >> 3U));
}

/// Unpack Rgb565 to Rgb888, replicating high bits into low bits so 0x1F -> 0xFF and
/// to_rgb565(from_rgb565(x)) == x for every x.
constexpr Rgb888 from_rgb565(Rgb565 c) {
  const auto r5 = static_cast<unsigned>((c >> 11U) & 0x1FU);
  const auto g6 = static_cast<unsigned>((c >> 5U) & 0x3FU);
  const auto b5 = static_cast<unsigned>(c & 0x1FU);
  return Rgb888{static_cast<std::uint8_t>((r5 << 3U) | (r5 >> 2U)),
                static_cast<std::uint8_t>((g6 << 2U) | (g6 >> 4U)),
                static_cast<std::uint8_t>((b5 << 3U) | (b5 >> 2U))};
}

/// Linear interpolation between two colours per channel; t is clamped to 0..1 and each channel
/// rounds to nearest (half away from zero).
constexpr Rgb888 lerp(Rgb888 a, Rgb888 b, double t) {
  const double u = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  auto ch = [u](std::uint8_t x, std::uint8_t y) {
    const double v = static_cast<double>(x) + (static_cast<double>(y) - x) * u;
    return static_cast<std::uint8_t>(v + 0.5);
  };
  return Rgb888{ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b)};
}

/// The three anchor colours of the scale. Defaults: a quiet warm grey, a clear blue, a clear
/// red. ui_lvgl may pass a different palette (e.g. a dark theme); core never assumes these.
struct Palette {
  Rgb888 neutral{0xD8, 0xD4, 0xCC};
  Rgb888 cold{0x2B, 0x6C, 0xD8};
  Rgb888 hot{0xD8, 0x3A, 0x2B};
};

/// Colour for `value` under `band`:
/// - comfort_min <= value <= comfort_max: palette.neutral.
/// - value < comfort_min: lerp(neutral, cold, t), t = (comfort_min - value) /
///   (comfort_min - scale_min); when comfort_min <= scale_min the cold side is a step: cold.
/// - value > comfort_max: lerp(neutral, hot, t), t = (value - comfort_max) /
///   (scale_max - comfort_max); when scale_max <= comfort_max: hot.
/// - Non-finite value: palette.neutral (callers should never pass one; heat cells with no data
///   are not coloured through this function).
/// Implemented in core (PR 3).
Rgb888 colour_for(const Band& band, double value, const Palette& palette = Palette{});

/// to_rgb565(colour_for(...)), the form ui_lvgl writes into the heat image.
inline Rgb565 colour565_for(const Band& band, double value, const Palette& palette = Palette{}) {
  return to_rgb565(colour_for(band, value, palette));
}

}  // namespace hearthmap
