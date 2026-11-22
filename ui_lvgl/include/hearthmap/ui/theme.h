// theme.h — the UI spec's colours and fixed geometry, in one place.
//
// Values are the approved mockup's (ui spec, "Colours" and "Regions"). Colours are 0xRRGGBB;
// widgets convert them with lv_color_hex(). Nothing here decides anything: severity, thirst and
// layout of the map come from core.
#pragma once

#include <cstdint>

#include "hearthmap/colour_scale.h"

namespace hearthmap::ui::theme {

// Surfaces
inline constexpr std::uint32_t kBackground = 0x0D1013;
inline constexpr std::uint32_t kCard = 0x161B21;
inline constexpr std::uint32_t kTrack = 0x2A3038;
inline constexpr std::uint32_t kWall = 0x4A535D;
inline constexpr std::uint32_t kCardStroke = 0x3A424B;
inline constexpr std::uint32_t kTabInactive = 0x1B2026;
inline constexpr std::uint32_t kPlantDisc = 0x16261C;
inline constexpr std::uint32_t kOutdoorFill = 0x101A14;
/// Unmeasured room cells (CellState::NoData) are drawn as diagonal stripes of these two, so a
/// room without a sensor reads as "no data", never as a colour (docs/decisions.md D-005).
inline constexpr std::uint32_t kNoDataA = 0x14181D;
inline constexpr std::uint32_t kNoDataB = 0x1B2026;
inline constexpr int kNoDataStripePx = 8;

// Text
inline constexpr std::uint32_t kTextPrimary = 0xE6EDF3;
inline constexpr std::uint32_t kTextSecondary = 0x9AA4AF;
inline constexpr std::uint32_t kTextMuted = 0x6B7280;

// Status
inline constexpr std::uint32_t kDanger = 0xE5484D;
inline constexpr std::uint32_t kWarn = 0xF5A524;
inline constexpr std::uint32_t kOk = 0x30A46C;
inline constexpr std::uint32_t kStale = 0x6B7280;

// Plant placeholder leaf
inline constexpr std::uint32_t kLeaf = 0x3FA56B;
inline constexpr std::uint32_t kLeafStale = 0x4B5563;

// Overlay
/// The spec's scrim behind the detail card. Not drawn on the panel since D-052 (a full-screen
/// blend cost a long refresh each way); kept as the spec's value.
inline constexpr std::uint32_t kScrim = 0x050709;
inline constexpr std::uint8_t kScrimOpa = 184;      ///< 0.72 * 255, rounded.
inline constexpr std::uint8_t kHappyRangeOpa = 89;  ///< 0.35 * 255, rounded.

/// The heat field's comfort-quiet diverging palette (ui spec, "Heatmap").
inline constexpr Palette kHeatPalette{Rgb888{30, 35, 41}, Rgb888{42, 120, 214},
                                      Rgb888{227, 73, 72}};

// Pulses (UI spec, "Pins" and "Rooms"; D-053): drawn as sprites by ui_lvgl's pulse layer.
inline constexpr int kPulseRingFrom = 26;  ///< danger ring radius at the start of a cycle, px
inline constexpr int kPulseRingTo = 52;    ///< and at the end, where it has faded out
inline constexpr int kPulseRingWidth = 4;
/// The spec's 1.6 s, shortened to 1584 ms so whole frames of 33 ms (48 a cycle, 30 a second) and
/// every coarser rate the sprites may fall back to divide it (D-053).
inline constexpr std::uint32_t kPulseRingCycleMs = 1584;
inline constexpr int kRoomPulseWidth = 5;                 ///< room outline width, px
inline constexpr std::uint32_t kRoomPulseCycleMs = 2800;  ///< opacity 0.35 -> 1 -> 0.35
inline constexpr std::uint8_t kRoomPulseMinOpa = 89;      ///< 0.35 * 255
/// Pulses carry a dark keyline (kBackground) under their colour, so a red ring still reads on a
/// red room: this many px wider than the stroke in all, at this share of the stroke's opacity.
inline constexpr int kPulseHaloExtra = 4;
inline constexpr std::uint8_t kPulseHaloOpa = 200;

/// Sprite tiles and their repaints are aligned to this many pixels, a multiple of any display
/// driver's draw rounding (ESPHome rounds invalidated areas out to its draw_rounding, 2 on the
/// panel): a rounded repaint must stay inside its opaque tile, or LVGL draws everything under it.
inline constexpr int kTileAlign = 8;

// Geometry (device pixels, landscape 1280 x 800)
inline constexpr int kScreenWidth = 1280;
inline constexpr int kScreenHeight = 800;
inline constexpr int kTopBarHeight = 64;
/// The map region (ui spec, "Regions": x 0..920, y 64..800): where a config's map_area belongs,
/// below the top bar and clear of the HUD column. The editor's default for new projects is this
/// rectangle (editor/test/defaults.test.ts reads these constants).
inline constexpr int kMapRegionX = 0;
inline constexpr int kMapRegionY = kTopBarHeight;
inline constexpr int kMapRegionWidth = 920;
inline constexpr int kMapRegionHeight = 800 - kTopBarHeight;
inline constexpr int kHudX = 936;
inline constexpr int kHudY = 76;
inline constexpr int kHudWidth = 328;
inline constexpr int kHudCardHeight = 84;
inline constexpr int kHudCardGap = 10;

}  // namespace hearthmap::ui::theme
