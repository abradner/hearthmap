import { describe, expect, it } from "vitest";
import { DEFAULT_BANDS } from "../src/lib/defaults";
import { buildScene, classify, heatColour, parseReadings, sampleReadings, soilBand, thirstOf } from "../src/preview/scene";
import { example, repoText } from "./helpers";

const fixture = (name: string) => parseReadings(repoText(`examples/flat/readings/${name}.json`));

describe("preview helpers (approximation of core)", () => {
  it("thirst handles inverted ranges, clamps, and refuses zero width", () => {
    expect(thirstOf({ dry: 2900, wet: 1300 }, 2750)).toBeCloseTo(0.906, 3);
    expect(thirstOf({ dry: 0, wet: 55 }, -5)).toBe(1);
    expect(thirstOf({ dry: 0, wet: 55 }, 90)).toBe(0);
    expect(thirstOf({ dry: 5, wet: 5 }, 5)).toBeUndefined();
  });
  it("classify: exactly on a threshold is the milder side; null thresholds never fire", () => {
    const t = DEFAULT_BANDS.temperature;
    expect(classify(27, t)).toBe("ok");
    expect(classify(27.01, t)).toBe("warn");
    expect(classify(32.01, t)).toBe("danger");
    expect(classify(11.9, t)).toBe("danger");
    expect(classify(-100, DEFAULT_BANDS.soil)).toBe("ok");
  });
  it("soilBand folds a one-sided happy range in (the fern: warn_above 0.615)", () => {
    const b = soilBand(DEFAULT_BANDS.soil, { dry: 15, wet: 80, happy_min: 40 });
    expect(b.warn_above).toBeCloseTo(0.615, 3);
    expect(b.comfort_max).toBeCloseTo(0.615, 3);
    expect(b.danger_above).toBe(0.9);
  });
  it("heat colour is neutral in band and saturates at the scale ends", () => {
    const t = DEFAULT_BANDS.temperature;
    expect(heatColour(21, t)).toEqual([30, 35, 41]);
    expect(heatColour(10, t)).toEqual([42, 120, 214]);
    expect(heatColour(40, t)).toEqual([227, 73, 72]);
    // R9: comfort_min <= scale_min makes the cold side neutral, not a step to full cold.
    expect(heatColour(0, { ...t, comfort_min: 10, scale_min: 10 })).toEqual([30, 35, 41]);
  });
});

describe("buildScene against the fixtures' documented outcomes (examples/README.md)", () => {
  it("thirsty-many: HUD basil, monstera, lavender, pothos, then +2 more", () => {
    const s = buildScene(example(), fixture("thirsty-many"), "soil");
    expect(s.hud.items.map((h) => h.name)).toEqual(["Basil", "Monstera", "Lavender", "Pothos"]);
    expect(s.hud.overflow).toBe(2);
    expect(s.hud.items[0]).toMatchObject({ percent: 0, severity: "danger", room: "Kitchen" });
    const pulse = Object.fromEntries(s.rooms.map((r) => [r.name, r.pulse]));
    expect(pulse).toMatchObject({ "Living room": "danger", Kitchen: "danger", Study: "danger", Balcony: "danger", Bedroom: "warn", Bathroom: "warn" });
  });
  it("stale-sensors: stale pins show no data and never pulse", () => {
    const s = buildScene(example(), fixture("stale-sensors"), "temperature");
    const pin = (id: string) => s.pins[example().pins.findIndex((p) => p.id === id)]!;
    expect(pin("clim_kitchen")).toMatchObject({ severity: "stale", label: "no data" });
    expect(pin("clim_study").severity).toBe("stale");
    expect(pin("plant_tomato").severity).not.toBe("stale"); // 4 h old, pin override 6 h
    expect(pin("plant_lavender").severity).not.toBe("stale"); // exactly 3 h: still fresh
    expect(pin("plant_snake")).toMatchObject({ severity: "stale", label: "no data" });
  });
  it("humid-bathroom: bathroom danger, bedroom warn, outdoor balcony never pulses", () => {
    const s = buildScene(example(), fixture("humid-bathroom"), "humidity");
    const pulse = Object.fromEntries(s.rooms.map((r) => [r.name, r.pulse]));
    expect(pulse.Bathroom).toBe("danger");
    expect(pulse.Bedroom).toBe("warn");
    expect(pulse.Balcony).toBeUndefined();
  });
  it("all-ok: empty HUD, nothing pulses on any layer", () => {
    for (const layer of ["temperature", "humidity", "soil", "air"] as const) {
      const s = buildScene(example(), fixture("all-ok"), layer);
      expect(s.hud.items).toEqual([]);
      expect(s.rooms.every((r) => r.pulse === undefined)).toBe(true);
    }
  });
});

describe("heat field", () => {
  it("gives outdoor rooms no heat colour (R10) and unmeasured rooms no data", () => {
    const c = example();
    const s = buildScene(c, fixture("all-ok"), "temperature");
    const at = (x: number, y: number) => s.heat.cells[Math.floor(y * s.heat.rows) * s.heat.cols + Math.floor(x * s.heat.cols)];
    expect(at(0.2, 0.8)).toBeUndefined(); // balcony, outdoor, though it has a temperature pin
    expect(typeof at(0.2, 0.3)).toBe("number"); // living room
    const noKitchenPin = { ...c, pins: c.pins.filter((p) => p.room !== "kitchen") };
    const s2 = buildScene(noKitchenPin, fixture("all-ok"), "temperature");
    const at2 = (x: number, y: number) => s2.heat.cells[Math.floor(y * s2.heat.rows) * s2.heat.cols + Math.floor(x * s2.heat.cols)];
    expect(at2(0.55, 0.2)).toBeNull();
  });
  it("is room-local: a hot room does not warm its neighbour", () => {
    const rs = fixture("all-ok");
    const hot = new Map(rs.readings);
    hot.set("sensor.example_kitchen_temperature", { value: 40, lastUpdated: rs.now });
    const s = buildScene(example(), { now: rs.now, readings: hot }, "temperature");
    const at = (x: number, y: number) => s.heat.cells[Math.floor(y * s.heat.rows) * s.heat.cols + Math.floor(x * s.heat.cols)] as number;
    expect(at(0.55, 0.2)).toBeCloseTo(40);
    expect(at(0.7, 0.2)).toBeLessThan(30); // study, next door
  });
  it("falls back to the default layer when the requested one is disabled", () => {
    const c = { ...example(), layers: { soil: { enabled: false } } };
    expect(buildScene(c, sampleReadings(c), "soil").layer).toBe("temperature");
  });
});

describe("readings input", () => {
  it("sample readings bind every pin's entities, all fresh", () => {
    const c = example();
    const rs = sampleReadings(c);
    const bound = c.pins.flatMap((p) => Object.values(p.entities));
    expect(bound.every((id) => rs.readings.has(id))).toBe(true);
    expect(buildScene(c, rs, "temperature").pins.every((p) => p.severity !== "stale")).toBe(true);
  });
  it("refuses a malformed fixture", () => {
    expect(() => parseReadings("{}")).toThrow(/readings fixture/);
    expect(() => parseReadings('{"now":"2026-01-01T00:00:00Z","readings":{"sensor.example_a":{"value":1}}}')).toThrow(/last_updated/);
  });
});
