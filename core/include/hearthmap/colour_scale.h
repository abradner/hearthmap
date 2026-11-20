// colour_scale.h — value -> RGB565, quiet inside the comfort band.
//
// A diverging scale: the neutral colour anywhere inside [comfort_min, comfort_max]; towards the
// cold colour below it, saturating at scale_min; towards the hot colour above it, saturating at
// scale_max. Severity does not affect colour (pins and pulsing carry severity).
//
// Performance contract (docs/decisions.md D-022): the heat field is coloured per heat cell,
// never per screen pixel, and through a ColourLut built once per band, not by calling
// colour_for() per cell. colour_for() is the reference the table is built from and tested
// against.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

/// The three anchor colours of the scale. Defaults are the UI spec's dark-theme heatmap colours
/// (docs/decisions.md D-025): neutral rgb(30,35,41), cold rgb(42,120,214), hot rgb(227,73,72).
/// ui_lvgl may pass a different palette; core never assumes these.
struct Palette {
  Rgb888 neutral{30, 35, 41};
  Rgb888 cold{42, 120, 214};
  Rgb888 hot{227, 73, 72};
};

/// Colour for `value` under `band`:
/// - comfort_min <= value <= comfort_max: palette.neutral.
/// - value < comfort_min: lerp(neutral, cold, t), t = (comfort_min - value) /
///   (comfort_min - scale_min); when comfort_min <= scale_min the cold side is neutral (no
///   room to fade, so no colour: not a step to full cold; D-021).
/// - value > comfort_max: lerp(neutral, hot, t), t = (value - comfort_max) /
///   (scale_max - comfort_max); when scale_max <= comfort_max the hot side is neutral.
/// - Non-finite value: palette.neutral (callers should never pass one; heat cells with no data
///   are not coloured through this function).
Rgb888 colour_for(const Band& band, double value, const Palette& palette = Palette{});

/// to_rgb565(colour_for(...)). The reference for one value; the heat image uses ColourLut.
inline Rgb565 colour565_for(const Band& band, double value, const Palette& palette = Palette{}) {
  return to_rgb565(colour_for(band, value, palette));
}

/// Default number of entries in a ColourLut.
inline constexpr std::size_t kDefaultColourLutSize = 256;

/// A band's colour scale sampled into a table, so colouring a heat cell is one clamp, one
/// multiply and one index, all in float (the ESP32-P4 has a single-precision FPU only).
///
/// Entry i holds colour565_for(band, lo + (hi - lo) * i / (n - 1), palette), where lo =
/// band.scale_min, hi = band.scale_max and n = entries.size(). Values beyond lo..hi saturate
/// exactly as colour_for() does, so clamping the index loses nothing. The table quantises the
/// comfort edges to the entry spacing; that is the accepted cost.
struct ColourLut {
  float lo = 0.0F;
  float hi = 1.0F;
  std::vector<Rgb565> entries;

  /// The entry nearest `value`: index = round((value - lo) / (hi - lo) * (n - 1)), clamped to
  /// 0..n-1. A non-finite value or an empty table gives 0 (black); callers colour only cells
  /// in CellState::Value, which are finite. A degenerate table (hi <= lo, from a band
  /// validate() rejects) gives entries.front() for value <= lo and entries.back() otherwise.
  /// Never allocates.
  Rgb565 lookup(float value) const;
};

/// Fill `out` with `size` entries for `band` and `palette` (a size below 2 is treated as 2).
/// Reuses out.entries' storage: once it has held `size` entries, a rebuild does not allocate.
/// Build once per config (or palette change) per layer, not per frame.
void build_colour_lut(const Band& band, const Palette& palette, std::size_t size, ColourLut& out);

/// Returning form of build_colour_lut(); allocates.
ColourLut build_colour_lut(const Band& band, const Palette& palette = Palette{},
                           std::size_t size = kDefaultColourLutSize);

}  // namespace hearthmap
