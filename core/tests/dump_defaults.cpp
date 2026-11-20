// dump_defaults: print core's compiled-in defaults as JSON, for check_defaults.py.
//
// Test tooling, not part of core: it writes to stdout, which core itself never does.
#include <cstdio>
#include <optional>

#include "hearthmap/config.h"

namespace {

using hearthmap::Band;
using hearthmap::Layer;

void number(const char* key, double v, bool comma = true) {
  std::printf("      \"%s\": %.17g%s\n", key, v, comma ? "," : "");
}

void optional_number(const char* key, const std::optional<double>& v) {
  if (v) {
    number(key, *v);
  } else {
    std::printf("      \"%s\": null,\n", key);
  }
}

void band(const char* name, Layer layer, bool comma) {
  const Band b = hearthmap::default_band(layer);
  std::printf("    \"%s\": {\n", name);
  number("comfort_min", b.comfort_min);
  number("comfort_max", b.comfort_max);
  optional_number("warn_below", b.warn_below);
  optional_number("warn_above", b.warn_above);
  optional_number("danger_below", b.danger_below);
  optional_number("danger_above", b.danger_above);
  number("scale_min", b.scale_min);
  number("scale_max", b.scale_max, false);
  std::printf("    }%s\n", comma ? "," : "");
}

const char* layer_name(Layer l) {
  switch (l) {
    case Layer::Temperature: return "temperature";
    case Layer::Humidity: return "humidity";
    case Layer::Soil: return "soil";
    case Layer::Air: return "air";
  }
  return "?";
}

}  // namespace

int main() {
  const hearthmap::Config c;
  std::printf("{\n  \"bands\": {\n");
  band("temperature", Layer::Temperature, true);
  band("humidity", Layer::Humidity, true);
  band("soil", Layer::Soil, true);
  band("air", Layer::Air, false);
  std::printf("  },\n");
  bool all_enabled = true;
  bool layers_match_default_band = true;
  for (Layer l : {Layer::Temperature, Layer::Humidity, Layer::Soil, Layer::Air}) {
    const auto& lc = c.layer(l);
    const Band d = hearthmap::default_band(l);
    all_enabled = all_enabled && lc.enabled;
    layers_match_default_band =
        layers_match_default_band && lc.band.comfort_min == d.comfort_min &&
        lc.band.comfort_max == d.comfort_max && lc.band.warn_below == d.warn_below &&
        lc.band.warn_above == d.warn_above && lc.band.danger_below == d.danger_below &&
        lc.band.danger_above == d.danger_above && lc.band.scale_min == d.scale_min &&
        lc.band.scale_max == d.scale_max;
  }
  std::printf("  \"layers_enabled\": %s,\n", all_enabled ? "true" : "false");
  std::printf("  \"config_layers_use_default_band\": %s,\n",
              layers_match_default_band ? "true" : "false");
  std::printf("  \"default_layer\": \"%s\",\n", layer_name(c.default_layer));
  std::printf("  \"temperature_unit\": \"%s\",\n",
              c.temperature_unit == hearthmap::TemperatureUnit::Celsius ? "C" : "F");
  std::printf("  \"room_outdoor\": %s,\n", hearthmap::Room{}.outdoor ? "true" : "false");
  std::printf("  \"stale_after_s\": %lld,\n", static_cast<long long>(c.stale_after));
  std::printf("  \"hud_max_items\": %d,\n", static_cast<int>(c.hud.max_items));
  std::printf("  \"grid_cols\": %d,\n", static_cast<int>(c.heat.grid_cols));
  std::printf("  \"grid_rows\": %d,\n", static_cast<int>(c.heat.grid_rows));
  std::printf("  \"idw_power\": %.17g,\n", c.heat.idw_power);
  std::printf("  \"canvas_width\": %d,\n", static_cast<int>(c.canvas.width));
  std::printf("  \"canvas_height\": %d,\n", static_cast<int>(c.canvas.height));
  std::printf("  \"map_area\": [%d, %d, %d, %d]\n", static_cast<int>(c.canvas.map_area.x),
              static_cast<int>(c.canvas.map_area.y), static_cast<int>(c.canvas.map_area.width),
              static_cast<int>(c.canvas.map_area.height));
  std::printf("}\n");
  return 0;
}
