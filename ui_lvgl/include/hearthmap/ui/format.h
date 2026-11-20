// format.h — how values read on screen: text, percentages and status colours.
//
// Presentation only. Whether something is a problem is core's answer (Assessment::severity);
// these functions only turn answers into words and colours, and each is unit-tested.
#pragma once

#include <cstdint>
#include <string>

#include "hearthmap/config.h"
#include "hearthmap/severity.h"
#include "hearthmap/view_model.h"

namespace hearthmap::ui {

/// Status colour (theme.h) for a severity.
std::uint32_t severity_colour(Severity severity);

/// Soil moisture percent shown for a thirst: round((1 - thirst) * 100), clamped to 0..100.
int moisture_percent(double thirst);

/// Ring arc length in permille of a full turn for a thirst: the moisture fraction, at least 30
/// (the spec's 3% minimum, so a bone-dry pot still shows where the ring starts).
int ring_permille(double thirst);

/// A value in its role's domain as shown on a pin or in the overlay. Temperature one decimal
/// with a degree sign ("22.6°"), humidity and battery whole percent ("68%"), CO2 "620 ppm",
/// PM2.5 "6 ug/m3" (the built-in font has no micro or superscript glyph), VOC a bare number,
/// moisture (a thirst) as moisture percent.
std::string format_value(Role role, double value);

/// Label for a role in the overlay's stats ("Soil temp" for a temperature probe in a pot).
std::string role_label(Role role, PinKind kind);

/// The top bar's words for core's HouseSummary: "House 21.4°   52% RH   PM2.5 6" (temperature
/// one decimal with a degree sign, humidity and PM2.5 whole numbers, parts three spaces apart),
/// leaving out any part that is nullopt; "House: no data" when all are.
std::string summary_text(const HouseSummary& summary);

/// The tab label for a layer: "Temp", "Humidity", "Soil", "Air".
std::string layer_label(Layer layer);

/// "seen just now" (under a minute), "seen N min ago" (under two hours), "seen N h ago" (under
/// two days), else "seen N d ago". A negative age (clock skew) reads as just now.
std::string format_seen(Seconds age);

/// The overlay's thirst verdict for a plant's primary (moisture) assessment, e.g.
/// "4%  water now". Danger on the dry side: "water now"; warn on the dry side: "water soon";
/// warn or danger on the wet side (thirst below the band's comfort_min): "too wet"; ok:
/// "happy"; not fresh: "no data" without a percentage.
std::string thirst_verdict(const Assessment& moisture);

}  // namespace hearthmap::ui
