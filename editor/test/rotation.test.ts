// canvas.map_rotation in the editor (docs/decisions.md D-080): the turn itself, the edit that
// sets it, what dragging on a turned view writes, the preview, and the round trip.
import { describe, expect, it } from "vitest";
import { blankConfig, fitMapAreaToPlan, movePin, rotatePlan } from "../src/lib/ops";
import { inViewFrame, mapRotationOf, planToView, planToWorld, rotatePoint, viewToPlan, worldToPlan } from "../src/lib/rotation";
import { loadConfig, saveConfig } from "../src/lib/autosave";
import { exportFiles, openFiles, zipFiles } from "../src/lib/files";
import { normaliseJsonText, serialiseConfig } from "../src/lib/serialise";
import { schemaIssues } from "../src/lib/validate";
import { buildScene, sampleReadings } from "../src/preview/scene";
import type { Config, MapPoint } from "../src/lib/types";
import { clone, example, exampleText } from "./helpers";

const turned = (r: 0 | 90 | 180 | 270): Config => {
  const c = example();
  c.canvas.map_rotation = r;
  return c;
};

describe("the turn: core's rotate_map_point(), in TypeScript", () => {
  it("one quarter turn goes clockwise as seen on the panel (y down)", () => {
    expect(rotatePoint([0, 0], 1)).toEqual([1, 0]);
    expect(rotatePoint([1, 0], 1)).toEqual([1, 1]);
    expect(rotatePoint([1, 1], 1)).toEqual([0, 1]);
    expect(rotatePoint([0, 1], 1)).toEqual([0, 0]);
    // The same dyadic cases core's map_rotation_test.cpp checks, exactly.
    expect(rotatePoint([0.25, 0.125], 1)).toEqual([0.875, 0.25]);
    expect(rotatePoint([0.25, 0.125], 2)).toEqual([0.75, 0.875]);
    expect(rotatePoint([0.25, 0.125], 3)).toEqual([0.125, 0.75]);
    expect(rotatePoint([0.25, 0.125], -1)).toEqual([0.125, 0.75]);
    expect(rotatePoint([0.25, 0.125], 0)).toEqual([0.25, 0.125]);
  });

  it("view and plan frames are inverse at every angle", () => {
    for (const r of [0, 90, 180, 270] as const) {
      for (const pin of example().pins) {
        const back = viewToPlan(planToView(pin.position, r), r);
        expect(back[0]).toBeCloseTo(pin.position[0], 12);
        expect(back[1]).toBeCloseTo(pin.position[1], 12);
      }
    }
  });

  it("a config without the field is turned 0 degrees", () => {
    expect(mapRotationOf(example())).toBe(0);
    expect(mapRotationOf(turned(270))).toBe(270);
  });

  it("inViewFrame turns rooms and pins once and drops the field, like apply_map_rotation()", () => {
    const v = inViewFrame(turned(90));
    expect(v.canvas.map_rotation).toBeUndefined();
    const basil = v.pins.find((p) => p.id === "plant_basil")!;
    expect(basil.position[0]).toBeCloseTo(0.94, 12); // (1 - 0.06, 0.625)
    expect(basil.position[1]).toBeCloseTo(0.625, 12);
    expect(inViewFrame(v)).toEqual(v); // twice is once
    expect(inViewFrame(example())).toEqual(example()); // 0 changes nothing
  });
});

describe("editing a turned view writes plan-frame coordinates", () => {
  // The flat turned 90 degrees into a 600 x 720 map area: basil (0.625, 0.06) in the plan is at
  // (0.94, 0.625) of the area on screen, world pixel (564, 450).
  const size = { width: 600, height: 720 };

  it("a world point maps to the plan point under it, rounded to the stored precision", () => {
    expect(worldToPlan({ x: 564, y: 450 }, size, 90)).toEqual([0.625, 0.06]);
    expect(worldToPlan({ x: 564, y: 450 }, size, 0)).toEqual([0.94, 0.625]);
    const w = planToWorld([0.625, 0.06], size, 90);
    expect(w.x).toBeCloseTo(564, 9);
    expect(w.y).toBeCloseTo(450, 9);
  });

  it("drags off the map are clamped onto it, in either frame", () => {
    for (const r of [0, 90, 180, 270] as const) {
      const p = worldToPlan({ x: -50, y: 9999 }, size, r);
      for (const v of p) expect(v >= 0 && v <= 1).toBe(true);
    }
  });

  it("a pin dragged on the turned view lands in the plan frame, the field untouched", () => {
    const c = turned(90);
    const i = c.pins.findIndex((p) => p.id === "plant_basil");
    // Drag basil 60 world px left on screen: on a 90-degree view that is down the plan (+y).
    const at = planToWorld(c.pins[i]!.position, size, 90);
    const moved = movePin(c, i, worldToPlan({ x: at.x - 60, y: at.y }, size, 90));
    expect(moved.pins[i]!.position).toEqual([0.625, 0.16]);
    expect(moved.canvas.map_rotation).toBe(90);
  });

  it("every stored coordinate survives the screen round trip at every angle", () => {
    for (const r of [0, 90, 180, 270] as const) {
      for (const room of example().rooms) {
        for (const p of room.polygon) expect(worldToPlan(planToWorld(p, size, r), size, r)).toEqual(p);
      }
    }
  });
});

describe("rotatePlan: one undoable edit", () => {
  it("steps right and left through the four angles, writing no default", () => {
    let c = example();
    c = rotatePlan(c, 90);
    expect(c.canvas.map_rotation).toBe(90);
    c = rotatePlan(c, 90);
    expect(c.canvas.map_rotation).toBe(180);
    c = rotatePlan(c, 90);
    c = rotatePlan(c, 90);
    expect("map_rotation" in c.canvas).toBe(false); // back to 0: the default is not written
    expect(rotatePlan(example(), -90).canvas.map_rotation).toBe(270);
  });

  it("leaves rooms and pins in the plan's frame and its input untouched", () => {
    const before = example();
    const snapshot = clone(before);
    const after = rotatePlan(before, 90);
    expect(before).toEqual(snapshot);
    expect(after.rooms).toEqual(before.rooms);
    expect(after.pins).toEqual(before.pins);
  });

  it("gives the map area the turned proportions (Match plan shape, turned)", () => {
    // With the floor plan's own size: 1200 x 1000 in the plan is 1000 x 1200 on a quarter turn.
    const withPlan = rotatePlan(blankConfig(), 90, { width: 1200, height: 1000 });
    expect(withPlan.canvas.map_area).toEqual({ x: 0, y: 64, width: 613, height: 736 });
    expect(rotatePlan(withPlan, 90, { width: 1200, height: 1000 }).canvas.map_area).toEqual({ x: 0, y: 64, width: 883, height: 736 });
    // Without a plan image, the current area's proportions are swapped.
    const ex = rotatePlan(example(), 90); // 864 x 720 -> 720:864
    expect(ex.canvas.map_area).toEqual({ x: 0, y: 64, width: 613, height: 736 });
  });

  it("Match plan shape uses the turned proportions when the plan is turned", () => {
    expect(fitMapAreaToPlan(blankConfig(), 1200, 1000).canvas.map_area).toEqual({ x: 0, y: 64, width: 883, height: 736 });
    const r = { ...blankConfig(), canvas: { ...blankConfig().canvas, map_rotation: 270 as const } };
    expect(fitMapAreaToPlan(r, 1200, 1000).canvas.map_area).toEqual({ x: 0, y: 64, width: 613, height: 736 });
  });
});

describe("export, import and the preview", () => {
  it("exports the field after map_area and the schema accepts it; 45 is refused", () => {
    const c = turned(90);
    expect(schemaIssues(c)).toEqual([]);
    const text = serialiseConfig(c);
    expect(text.indexOf('"map_rotation": 90')).toBeGreaterThan(text.indexOf('"map_area"'));
    expect(normaliseJsonText(text)).toBe(text);
    expect(JSON.parse(text)).toEqual(c);
    const bad = clone(c) as unknown as { canvas: { map_rotation: number } };
    bad.canvas.map_rotation = 45;
    expect(schemaIssues(bad).length).toBeGreaterThan(0);
  });

  it("survives export to a zip, opening it again, and autosave", () => {
    const c = turned(270);
    const { files } = exportFiles(c, new Map());
    const opened = openFiles([{ name: "flat.zip", bytes: zipFiles(files) }]);
    expect((opened.config as Config).canvas.map_rotation).toBe(270);
    const mem = new Map<string, string>();
    const store = { getItem: (k: string) => mem.get(k) ?? null, setItem: (k: string, v: string) => void mem.set(k, v), removeItem: (k: string) => void mem.delete(k) };
    expect(saveConfig(store, c)).toBe(true);
    expect(loadConfig(store)?.canvas.map_rotation).toBe(270);
  });

  it("a config without the field exports without it", () => {
    expect(serialiseConfig(example())).toBe(normaliseJsonText(exampleText()));
    expect(serialiseConfig(example())).not.toContain("map_rotation");
  });

  it("the 1280 x 800 preview draws the map as the panel will", () => {
    const c = turned(90);
    c.canvas.map_area = { x: 40, y: 76, width: 600, height: 720 };
    const scene = buildScene(c, sampleReadings(c), "soil");
    const i = c.pins.findIndex((p) => p.id === "plant_basil");
    const pin = scene.pins.find((p) => p.index === i)!;
    // Where core's hit-test test and the rotated-90 golden put basil: canvas (604, 526).
    expect(pin.x).toBeCloseTo(604, 9);
    expect(pin.y).toBeCloseTo(526, 9);
    // The living room, the plan's top-left, is the area's top-right on screen.
    const living = scene.rooms[0]!.points.map((p) => [p.x, p.y] as MapPoint);
    expect(Math.max(...living.map((p) => p[0]))).toBeCloseTo(640, 9);
    expect(Math.min(...living.map((p) => p[1]))).toBeCloseTo(76, 9);
  });
});
