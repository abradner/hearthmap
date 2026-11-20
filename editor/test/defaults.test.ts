// The editor's copies of the defaults must match their originals: the schema's "default"
// keywords and core's default_band() in config.h (parsed here, so a change there turns this red).
import { describe, expect, it } from "vitest";
import schema from "../../schema/hearthmap.schema.json";
import { DEFAULT_BANDS, DEFAULT_HEAT, DEFAULT_HUD_MAX_ITEMS, DEFAULT_LAYER, DEFAULT_STALE_AFTER_S } from "../src/lib/defaults";
import { BAND_FIELDS, LAYERS } from "../src/lib/types";
import { repoText } from "./helpers";

const s = schema as unknown as {
  properties: { stale_after_s: { default: number } };
  $defs: {
    hud: { properties: { max_items: { default: number } } };
    heat: { properties: Record<string, { default: number }> };
    layers: { properties: { default: { default: string } } };
  };
};

describe("defaults match the schema", () => {
  it("stale_after_s, hud, heat and the default layer", () => {
    expect(DEFAULT_STALE_AFTER_S).toBe(s.properties.stale_after_s.default);
    expect(DEFAULT_HUD_MAX_ITEMS).toBe(s.$defs.hud.properties.max_items.default);
    expect(DEFAULT_HEAT.grid_cols).toBe(s.$defs.heat.properties.grid_cols!.default);
    expect(DEFAULT_HEAT.grid_rows).toBe(s.$defs.heat.properties.grid_rows!.default);
    expect(DEFAULT_HEAT.idw_power).toBe(s.$defs.heat.properties.idw_power!.default);
    expect(DEFAULT_LAYER).toBe(s.$defs.layers.properties.default.default);
  });
});

describe("band defaults match core's default_band()", () => {
  const header = repoText("core/include/hearthmap/config.h");
  const names: Record<string, string> = { Temperature: "temperature", Humidity: "humidity", Soil: "soil", Air: "air" };
  // e.g.  case Layer::Soil: return Band{0.1, 0.6, std::nullopt, 0.7, std::nullopt, 0.9, 0.0, 1.0};
  const re = /case Layer::(\w+):\s*return Band\{([^}]*)\}/g;
  const parsed = new Map<string, (number | null)[]>();
  for (const m of header.matchAll(re)) {
    parsed.set(names[m[1]!]!, m[2]!.split(",").map((t) => (t.trim() === "std::nullopt" ? null : Number(t.trim()))));
  }
  it("parses all four layers from the header", () => {
    expect([...parsed.keys()].sort()).toEqual([...LAYERS].sort());
  });
  it.each(LAYERS)("%s", (layer) => {
    // Band's field order in config.h: comfort_min, comfort_max, warn_below, warn_above,
    // danger_below, danger_above, scale_min, scale_max.
    const order = ["comfort_min", "comfort_max", "warn_below", "warn_above", "danger_below", "danger_above", "scale_min", "scale_max"] as const;
    expect(order.slice().sort()).toEqual([...BAND_FIELDS].sort());
    const values = parsed.get(layer)!;
    expect(Object.fromEntries(order.map((f, i) => [f, values[i]]))).toEqual(DEFAULT_BANDS[layer]);
  });
});
