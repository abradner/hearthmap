import { describe, expect, it } from "vitest";
import { KEY_ORDER, canonicalise, normaliseJsonText, serialiseConfig } from "../src/lib/serialise";
import type { Config } from "../src/lib/types";
import { example } from "./helpers";

describe("serialise", () => {
  it("takes key order from the schema", () => {
    expect(KEY_ORDER.pin.slice(0, 3)).toEqual(["id", "kind", "name"]);
    expect(KEY_ORDER.top[0]).toBe("$schema");
    expect(KEY_ORDER.calibration).toEqual(["dry", "wet", "happy_min", "happy_max"]);
  });
  it("puts keys added out of order back into schema order, but keeps entities in author order", () => {
    const c = example();
    const p = c.pins.find((x) => x.id === "plant_tomato")!;
    const { photo, calibration: _calibration, ...rest } = p;
    const shuffled = { ...rest, stale_after_s: 99, calibration: { wet: 65, dry: 2 }, photo } as typeof p;
    c.pins[c.pins.indexOf(p)] = shuffled;
    const out = canonicalise({ heat: c.heat, ...c });
    const tomato = out.pins.find((x) => x.id === "plant_tomato")!;
    expect(Object.keys(tomato)).toEqual(["id", "kind", "name", "position", "room", "entities", "photo", "calibration", "stale_after_s"]);
    expect(Object.keys(tomato.calibration!)).toEqual(["dry", "wet"]);
    expect(Object.keys(out)[0]).toBe("$schema");
    const bath = out.pins.find((x) => x.id === "hum_bathroom")!;
    expect(Object.keys(bath.entities)).toEqual(["humidity", "temperature"]);
  });
  it("drops undefined values and ends with a newline", () => {
    const c = { ...example(), stale_after_s: undefined } as unknown as Config;
    const text = serialiseConfig(c);
    expect(text.endsWith("}\n")).toBe(true);
    expect(text).not.toContain("stale_after_s\": null");
    expect(JSON.parse(text)).not.toHaveProperty("stale_after_s");
  });
  it("normalisation ignores formatting only", () => {
    expect(normaliseJsonText('{"a":[0.0, 1.50]}')).toBe('{\n  "a": [\n    0,\n    1.5\n  ]\n}\n');
  });
});
