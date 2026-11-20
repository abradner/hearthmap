import { describe, expect, it } from "vitest";
import type { Config } from "../src/lib/types";
import { editorIssues, issueTarget, schemaIssues, validateConfig, hasErrors, effectiveBand } from "../src/lib/validate";
import { clone, example, examplePhotoIds } from "./helpers";

const paths = (c: unknown, photos = examplePhotoIds()) => validateConfig(c, photos).map((i) => `${i.severity} ${i.path}`);

function mutate(f: (c: Config) => void): Config {
  const c = clone(example());
  f(c);
  return c;
}

describe("schema issues (the real schema)", () => {
  it("reports a missing required field at its own path", () => {
    const c = mutate((x) => {
      delete (x as Partial<Config>).rooms;
    });
    expect(schemaIssues(c)).toContainEqual(expect.objectContaining({ path: "/rooms", message: 'missing "rooms"' }));
  });

  it("rejects an id that breaks the pattern", () => {
    const c = mutate((x) => {
      x.rooms[0]!.id = "Living Room";
    });
    expect(schemaIssues(c).map((i) => i.path)).toContain("/rooms/0/id");
  });

  it("rejects a calibration on a temperature pin and drops the if-bookkeeping", () => {
    const c = mutate((x) => {
      x.pins[0]!.calibration = { dry: 0, wet: 1 };
    });
    const issues = schemaIssues(c);
    expect(issues).toContainEqual(expect.objectContaining({ path: "/pins/0", source: "schema" }));
    expect(issues.some((i) => i.message.includes('"if"'))).toBe(false);
  });

  it("rejects an unknown field", () => {
    const c = mutate((x) => {
      (x as unknown as Record<string, unknown>).token = "x";
    });
    expect(schemaIssues(c)).toContainEqual(expect.objectContaining({ message: 'unknown field "token"' }));
  });

  it("does not run editor checks on a schema-invalid config", () => {
    const c = mutate((x) => {
      x.version = 2 as 1;
      x.pins[0]!.room = "nowhere";
    });
    expect(validateConfig(c).every((i) => i.source === "schema")).toBe(true);
  });
});

describe("editor checks", () => {
  it("passes the example with only the expected warnings", () => {
    expect(paths(example()).filter((p) => p.startsWith("error"))).toEqual([]);
  });

  it("finds duplicate room and pin ids", () => {
    const c = mutate((x) => {
      x.rooms[1]!.id = x.rooms[0]!.id;
      x.pins[1]!.id = x.pins[0]!.id;
    });
    const p = paths(c);
    expect(p).toContain("error /rooms/1/id");
    expect(p).toContain("error /pins/1/id");
  });

  it("finds a pin naming an unknown room", () => {
    expect(paths(mutate((x) => void (x.pins[0]!.room = "attic")))).toContain("error /pins/0/room");
  });

  it("warns when a pin sits outside its room", () => {
    expect(paths(mutate((x) => void (x.pins[0]!.position = [0.9, 0.9])))).toContain("warning /pins/0/position");
  });

  it("finds a self-intersecting room outline", () => {
    const c = mutate((x) => {
      x.rooms[0]!.polygon = [
        [0, 0],
        [0.4, 0.6],
        [0.4, 0],
        [0, 0.6],
      ];
    });
    expect(paths(c)).toContain("error /rooms/0/polygon");
  });

  it("finds a zero-width calibration", () => {
    const i = example().pins.findIndex((p) => p.id === "plant_basil");
    expect(paths(mutate((x) => void (x.pins[i]!.calibration = { dry: 5, wet: 5 })))).toContain(`error /pins/${i}/calibration`);
  });

  it("finds a happy value outside dry..wet, on a normal and an inverted range", () => {
    const i = example().pins.findIndex((p) => p.id === "plant_pothos"); // dry 2900, wet 1300
    expect(paths(mutate((x) => void (x.pins[i]!.calibration = { dry: 2900, wet: 1300, happy_max: 1200 })))).toContain(
      `error /pins/${i}/calibration/happy_max`,
    );
    expect(paths(mutate((x) => void (x.pins[i]!.calibration = { dry: 2900, wet: 1300, happy_min: 2000 })))).not.toContain(
      `error /pins/${i}/calibration/happy_min`,
    );
  });

  it("finds happy_min wetter than happy_max, judging wetness by direction", () => {
    const i = example().pins.findIndex((p) => p.id === "plant_pothos");
    // Inverted range: lower numbers are wetter. happy_min 1500 is wetter than happy_max 2500.
    expect(paths(mutate((x) => void (x.pins[i]!.calibration = { dry: 2900, wet: 1300, happy_min: 1500, happy_max: 2500 })))).toContain(
      `error /pins/${i}/calibration/happy_min`,
    );
    // The same numbers the right way round on that range are fine.
    expect(paths(mutate((x) => void (x.pins[i]!.calibration = { dry: 2900, wet: 1300, happy_min: 2500, happy_max: 1500 })))).not.toContain(
      `error /pins/${i}/calibration/happy_min`,
    );
  });

  it("finds band ordering violations, skipping null thresholds", () => {
    expect(paths(mutate((x) => void (x.layers = { temperature: { warn_above: 20 } })))).toContain("error /layers/temperature");
    expect(paths(mutate((x) => void (x.layers = { temperature: { warn_above: null, danger_above: null } })))).not.toContain(
      "error /layers/temperature",
    );
    expect(paths(mutate((x) => void (x.layers = { air: { scale_min: 3000 } })))).toContain("error /layers/air");
  });

  it("finds a disabled default layer", () => {
    expect(paths(mutate((x) => void (x.layers = { default: "soil", soil: { enabled: false } })))).toContain("error /layers/default");
    expect(paths(mutate((x) => void (x.layers = { temperature: { enabled: false } })))).toContain("error /layers/default");
  });

  it("requires every temperature band field with Fahrenheit (R7)", () => {
    expect(paths(mutate((x) => void (x.temperature_unit = "F")))).toContain("error /layers/temperature");
    const full = mutate((x) => {
      x.temperature_unit = "F";
      x.layers = {
        temperature: {
          comfort_min: 66,
          comfort_max: 75,
          warn_below: 61,
          warn_above: 81,
          danger_below: 54,
          danger_above: 90,
          scale_min: 50,
          scale_max: 93,
        },
      };
    });
    expect(paths(full).filter((p) => p.startsWith("error"))).toEqual([]);
  });

  it("refuses moisture on a pin that is not soil or plant (R1)", () => {
    const c = mutate((x) => void (x.pins[0]!.entities.moisture = "sensor.example_living_moisture"));
    // After the round-1 schema lands this is also a schema error; either way it is an error at that pin.
    expect(validateConfig(c).some((i) => i.severity === "error" && i.path.startsWith("/pins/0"))).toBe(true);
    expect(editorIssues(c).map((i) => i.path)).toContain("/pins/0/entities/moisture");
  });

  it("checks the canvas: orientation and map area inside it", () => {
    expect(paths(mutate((x) => void (x.canvas.orientation = "portrait")))).toContain("error /canvas");
    expect(paths(mutate((x) => void (x.canvas.map_area = { x: 400, y: 0, width: 960, height: 800 })))).toContain("error /canvas/map_area");
    // Omitted map area: the default is the whole canvas, which always fits (R14).
    expect(paths(mutate((x) => void delete x.canvas.map_area))).not.toContain("error /canvas/map_area");
  });

  it("requires at least one room", () => {
    expect(editorIssues(mutate((x) => void (x.rooms = []))).map((i) => i.path)).toContain("/rooms");
  });

  it("warns on an entity bound twice, at every place it is bound", () => {
    const c = mutate((x) => void (x.pins[1]!.entities.humidity = x.pins[0]!.entities.temperature));
    const p = paths(c);
    expect(p).toContain("warning /pins/0/entities/temperature");
    expect(p).toContain("warning /pins/1/entities/humidity");
  });

  it("warns when a photo id has no image loaded, and not when it does", () => {
    const i = example().pins.findIndex((p) => p.photo === "basil");
    expect(paths(example(), new Set())).toContain(`warning /pins/${i}/photo`);
    expect(paths(example(), new Set(["basil"]))).not.toContain(`warning /pins/${i}/photo`);
  });

  it("warns when an enabled layer has no pin feeding it, ignoring display-only roles", () => {
    const c = mutate((x) => {
      x.pins = x.pins.filter((p) => p.kind === "plant");
    });
    // Plants bind temperature/humidity on some pins, but those are display-only (D-004).
    expect(paths(c)).toContain("warning /layers/temperature");
    expect(paths(c)).not.toContain("warning /layers/soil");
  });

  it("fills band defaults and keeps explicit nulls", () => {
    const b = effectiveBand(mutate((x) => void (x.layers = { soil: { warn_above: null } })), "soil");
    expect(b.warn_above).toBeNull();
    expect(b.danger_above).toBe(0.9);
  });
});

describe("issueTarget", () => {
  it("maps paths to the item they belong to", () => {
    expect(issueTarget("/rooms/3/polygon")).toEqual({ kind: "room", index: 3 });
    expect(issueTarget("/pins/12")).toEqual({ kind: "pin", index: 12 });
    expect(issueTarget("/layers/air")).toEqual({ kind: "layer", layer: "air" });
    expect(issueTarget("/layers/default")).toEqual({ kind: "layer", layer: "default" });
    expect(issueTarget("/canvas/map_area")).toEqual({ kind: "project" });
    expect(issueTarget("/pins/x")).toEqual({ kind: "project" });
  });

  it("hasErrors ignores warnings", () => {
    expect(hasErrors([{ severity: "warning", path: "/", message: "", source: "editor" }])).toBe(false);
  });
});
