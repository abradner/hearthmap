import { describe, expect, it } from "vitest";
import { applyCalibrations, parseCalibrationFile, type CalibrationFile } from "../src/lib/calibrationImport";
import { schemaIssues, validateConfig } from "../src/lib/validate";
import { clone, example, examplePhotoIds, repoText } from "./helpers";

// A byte-for-byte copy of tools/calibrate/examples/calibration.json from the calibrate branch.
const text = repoText("editor/test/fixtures/calibration-example.json");
const file = (): CalibrationFile & Record<string, unknown> => JSON.parse(text) as CalibrationFile & Record<string, unknown>;

describe("parseCalibrationFile (tools/calibrate's format)", () => {
  it("accepts the tool's example output", () => {
    const f = parseCalibrationFile(text);
    expect(f.sensors.map((s) => s.status)).toEqual(["ok", "ok", "ok", "ok", "no_variation"]);
  });
  it("refuses a wrong version, a sensor map instead of an array, and a failed sensor carrying a calibration", () => {
    expect(() => parseCalibrationFile(JSON.stringify({ ...file(), version: 2 }))).toThrow(/not a calibration file/);
    const asMap = file();
    asMap.sensors = Object.fromEntries(asMap.sensors.map((s) => [s.entity_id, s])) as never;
    expect(() => parseCalibrationFile(JSON.stringify(asMap))).toThrow(/not a calibration file/);
    const bad = file();
    bad.sensors[4]!.calibration = { dry: 0, wet: 1, happy_min: 0, happy_max: 1 };
    expect(() => parseCalibrationFile(JSON.stringify(bad))).toThrow(/not a calibration file/);
    const missingHappy = file();
    delete (missingHappy.sensors[0]!.calibration as Partial<Record<string, number>>).happy_max;
    expect(() => parseCalibrationFile(JSON.stringify(missingHappy))).toThrow(/not a calibration file/);
    expect(() => parseCalibrationFile("{")).toThrow(/JSON/);
  });
});

describe("applyCalibrations", () => {
  it("applies ok sensors to matching soil/plant pins, skips failed ones with their message", () => {
    const r = applyCalibrations(example(), parseCalibrationFile(text));
    const by = Object.fromEntries(r.outcomes.map((o) => [o.entity_id, o]));
    expect(by["sensor.example_monstera_moisture"]).toMatchObject({ result: "applied", pins: ["plant_monstera"], confidence: "high", happy_is_default: true });
    expect(by["sensor.example_snake_moisture"]).toMatchObject({ result: "applied", confidence: "low" });
    expect(by["sensor.example_lavender_moisture"]).toMatchObject({ result: "skipped", confidence: "none" });
    expect(by["sensor.example_lavender_moisture"]?.message).toMatch(/no variation/);
    expect(by["sensor.example_monstera_moisture"]?.warnings.length).toBeGreaterThan(0);
    // The failed sensor's pin keeps its old calibration.
    expect(r.config.pins.find((p) => p.id === "plant_lavender")?.calibration).toEqual(example().pins.find((p) => p.id === "plant_lavender")?.calibration);
    expect(r.config.pins.find((p) => p.id === "plant_pothos")?.calibration).toEqual({ dry: 2884, wet: 1368, happy_min: 2429.2, happy_max: 1368 });
    // Evidence never reaches the config, which still validates (schema and editor checks).
    expect(JSON.stringify(r.config)).not.toContain("percentile");
    expect(schemaIssues(r.config)).toEqual([]);
    expect(validateConfig(r.config, examplePhotoIds()).filter((i) => i.severity === "error")).toEqual([]);
  });
  it("reports an ok sensor no pin binds, and refuses a zero-width range", () => {
    const f = parseCalibrationFile(text);
    const c = clone(example());
    c.pins = c.pins.filter((p) => p.id !== "plant_basil");
    f.sensors[2]!.calibration = { dry: 5, wet: 5, happy_min: 5, happy_max: 5 };
    const r = applyCalibrations(c, f);
    const by = Object.fromEntries(r.outcomes.map((o) => [o.entity_id, o]));
    expect(by["sensor.example_basil_moisture"]?.result).toBe("unmatched");
    expect(by["sensor.example_pothos_moisture_raw"]?.result).toBe("invalid");
    expect(r.config.pins.find((p) => p.id === "plant_pothos")?.calibration).toEqual({ dry: 2900, wet: 1300 });
  });
});
