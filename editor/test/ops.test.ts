import { describe, expect, it } from "vitest";
import {
  addPin,
  addRoom,
  bindEntity,
  blankConfig,
  deletePin,
  deleteRoom,
  deleteVertex,
  fitMapAreaToAspect,
  insertVertex,
  movePin,
  moveVertex,
  photoIdFor,
  renameRoom,
  reorderRoom,
  setCalibration,
  setDefaultLayer,
  setHudMax,
  setLayerField,
  setOutdoor,
  setPinKind,
  setPinPhoto,
  setRoomId,
  setStaleAfter,
} from "../src/lib/ops";
import { schemaIssues } from "../src/lib/validate";
import type { Config, MapPoint } from "../src/lib/types";
import { clone, example } from "./helpers";

const sq = (x: number, y: number, s = 0.2): MapPoint[] => [
  [x, y],
  [x + s, y],
  [x + s, y + s],
  [x, y + s],
];

function twoRooms(): Config {
  return addRoom(addRoom(blankConfig(), sq(0, 0), "Kitchen"), sq(0.4, 0), "Study");
}

describe("rooms", () => {
  it("adds rooms with unique slug ids and rounded vertices", () => {
    let c = addRoom(blankConfig(), [
      [0.123456, 0],
      [0.5, 0],
      [0.5, 0.5],
    ]);
    c = addRoom(c, sq(0.6, 0.6), "Room 1");
    expect(c.rooms.map((r) => r.id)).toEqual(["room_1", "room_1_2"]);
    expect(c.rooms[0]!.polygon[0]).toEqual([0.1235, 0]);
    expect(schemaIssues(c)).toEqual([]);
  });

  it("renames a room; an auto id follows the name and pins follow the id", () => {
    let c = addPin(twoRooms(), "temperature", [0.1, 0.1]);
    c = renameRoom(c, 0, "Galley kitchen");
    expect(c.rooms[0]!.id).toBe("galley_kitchen");
    expect(c.pins[0]!.room).toBe("galley_kitchen");
  });

  it("keeps a hand-set id when renaming", () => {
    let c = setRoomId(twoRooms(), 0, "k1");
    c = renameRoom(c, 0, "Scullery");
    expect(c.rooms[0]!.id).toBe("k1");
    expect(c.rooms[0]!.name).toBe("Scullery");
  });

  it("marks and unmarks outdoor without leaving outdoor: false behind", () => {
    const c = setOutdoor(twoRooms(), 1, true);
    expect(c.rooms[1]!.outdoor).toBe(true);
    expect("outdoor" in setOutdoor(c, 1, false).rooms[1]!).toBe(false);
  });

  it("re-homes a deleted room's pins into the room that contains them, else keeps the dangling id", () => {
    let c = addRoom(blankConfig(), sq(0, 0, 0.5), "Big");
    c = addRoom(c, sq(0, 0, 0.2), "Nook"); // inside Big, but Big is first so owns the overlap
    c = addPin(c, "temperature", [0.8, 0.8]); // outside both: assigned to the first room
    c = { ...c, pins: [{ ...c.pins[0]!, room: "nook", position: [0.1, 0.1] }, { ...c.pins[0]!, id: "p2", room: "nook", position: [0.9, 0.9] }] };
    const after = deleteRoom(c, 1);
    expect(after.pins.map((p) => p.room)).toEqual(["big", "nook"]);
  });

  it("moves, inserts and deletes vertices within the 3..64 limits", () => {
    let c = twoRooms();
    c = moveVertex(c, 0, 2, [0.25, 0.25]);
    expect(c.rooms[0]!.polygon[2]).toEqual([0.25, 0.25]);
    c = insertVertex(c, 0, 1, [0.1, -0.5]);
    expect(c.rooms[0]!.polygon).toHaveLength(5);
    expect(c.rooms[0]!.polygon[1]).toEqual([0.1, 0]); // clamped into map space
    c = deleteVertex(deleteVertex(c, 0, 1), 0, 0);
    expect(c.rooms[0]!.polygon).toHaveLength(3);
    expect(deleteVertex(c, 0, 0)).toBe(c); // a triangle keeps its three
    let big = addRoom(blankConfig(), Array.from({ length: 64 }, (_, k) => [Math.cos(k / 10) / 2 + 0.5, Math.sin(k / 10) / 2 + 0.5] as MapPoint));
    big = insertVertex(big, 0, 1, [0.5, 0.5]);
    expect(big.rooms[0]!.polygon).toHaveLength(64);
    expect(() => moveVertex(c, 0, 7, [0, 0])).toThrow(RangeError);
  });

  it("reorders rooms and ignores moves past either end", () => {
    const c = twoRooms();
    expect(reorderRoom(c, 0, 1).rooms.map((r) => r.id)).toEqual(["study", "kitchen"]);
    expect(reorderRoom(c, 0, -1)).toBe(c);
  });

  it("refuses a 65th room (R3)", () => {
    let c = blankConfig();
    for (let k = 0; k < 64; k++) c = addRoom(c, sq(0, 0, 0.01));
    expect(() => addRoom(c, sq(0, 0))).toThrow(/64/);
  });
});

describe("pins", () => {
  it("needs a room first", () => {
    expect(() => addPin(blankConfig(), "plant", [0.5, 0.5])).toThrow(/room/);
  });

  it("suggests the room under the pin, falling back to the first room", () => {
    const c = addPin(addPin(twoRooms(), "temperature", [0.5, 0.1]), "humidity", [0.9, 0.9]);
    expect(c.pins.map((p) => p.room)).toEqual(["study", "kitchen"]);
    expect(c.pins[0]!.name).toBe("Study temperature");
  });

  it("gives plants and soil probes a starting calibration, others none", () => {
    const c = addPin(addPin(twoRooms(), "plant", [0.1, 0.1]), "air_quality", [0.1, 0.1]);
    expect(c.pins[0]!.calibration).toEqual({ dry: 0, wet: 100 });
    expect(c.pins[1]!.calibration).toBeUndefined();
  });

  it("moves a pin; its room follows into another room but not out of all rooms", () => {
    let c = addPin(twoRooms(), "temperature", [0.1, 0.1]);
    c = movePin(c, 0, [0.5, 0.1]);
    expect(c.pins[0]!.room).toBe("study");
    c = movePin(c, 0, [0.9, 0.9]);
    expect(c.pins[0]!.room).toBe("study");
    expect(c.pins[0]!.position).toEqual([0.9, 0.9]);
  });

  it("changing kind drops a forbidden calibration, photo and moisture (R1), and adds a needed calibration", () => {
    let c = addPin(twoRooms(), "plant", [0.1, 0.1]);
    c = bindEntity(c, 0, "moisture", "sensor.example_a_moisture");
    c = setPinPhoto(c, 0, "fern");
    const t = setPinKind(c, 0, "temperature");
    expect(t.pins[0]).not.toHaveProperty("calibration");
    expect(t.pins[0]).not.toHaveProperty("photo");
    expect(t.pins[0]!.entities).not.toHaveProperty("moisture");
    const s = setPinKind(t, 0, "soil");
    expect(s.pins[0]!.calibration).toEqual({ dry: 0, wet: 100 });
    // Plant -> soil keeps the calibration, drops the photo, keeps moisture.
    const s2 = setPinKind(c, 0, "soil");
    expect(s2.pins[0]!.entities.moisture).toBe("sensor.example_a_moisture");
    expect(s2.pins[0]).not.toHaveProperty("photo");
  });

  it("binds, trims and unbinds entities; refuses moisture on a non-soil pin", () => {
    let c = addPin(twoRooms(), "temperature", [0.1, 0.1]);
    c = bindEntity(c, 0, "temperature", "  sensor.example_t ");
    expect(c.pins[0]!.entities).toEqual({ temperature: "sensor.example_t" });
    expect(bindEntity(c, 0, "temperature", "").pins[0]!.entities).toEqual({});
    expect(() => bindEntity(c, 0, "moisture", "sensor.example_m")).toThrow(/soil and plant/);
  });

  it("sets a calibration without carrying undefined happy fields", () => {
    const c = setCalibration(addPin(twoRooms(), "plant", [0.1, 0.1]), 0, { dry: 3, wet: 40, happy_min: undefined });
    expect(c.pins[0]!.calibration).toEqual({ dry: 3, wet: 40 });
  });

  it("photos: plant pins only; ids unique among other pins", () => {
    let c = addPin(addPin(twoRooms(), "plant", [0.1, 0.1]), "plant", [0.15, 0.1]);
    c = { ...c, pins: c.pins.map((p) => ({ ...p, name: "Boston fern" })) };
    c = setPinPhoto(c, 0, photoIdFor(c, 0));
    expect(c.pins[0]!.photo).toBe("boston-fern");
    expect(photoIdFor(c, 1)).toBe("boston-fern-2");
    expect(photoIdFor(c, 0)).toBe("boston-fern"); // its own photo does not count as taken
    const t = addPin(c, "temperature", [0.1, 0.1]);
    expect(() => setPinPhoto(t, 2, "x")).toThrow(/plant/);
  });

  it("deletes a pin", () => {
    expect(deletePin(addPin(twoRooms(), "plant", [0.1, 0.1]), 0).pins).toEqual([]);
  });

  it("refuses a 257th pin (R3)", () => {
    let c = twoRooms();
    c = { ...c, pins: Array.from({ length: 256 }, (_, k) => ({ id: `p${k}`, kind: "temperature" as const, name: "t", position: [0.1, 0.1] as MapPoint, room: "kitchen", entities: { temperature: "sensor.example_t" } })) };
    expect(() => addPin(c, "temperature", [0.1, 0.1])).toThrow(/256/);
  });
});

describe("layers and settings", () => {
  it("sets, nulls and removes layer fields, cleaning up empty objects", () => {
    let c = setLayerField(blankConfig(), "air", "warn_above", 1000);
    expect(c.layers).toEqual({ air: { warn_above: 1000 } });
    c = setLayerField(c, "air", "danger_above", null);
    expect(c.layers?.air).toEqual({ warn_above: 1000, danger_above: null });
    c = setLayerField(setLayerField(c, "air", "warn_above", undefined), "air", "danger_above", undefined);
    expect(c).not.toHaveProperty("layers");
  });

  it("sets and clears the default layer, stale timeout and HUD cap", () => {
    let c = setDefaultLayer(blankConfig(), "soil");
    expect(c.layers).toEqual({ default: "soil" });
    expect(setDefaultLayer(c, undefined)).not.toHaveProperty("layers");
    c = setHudMax(setStaleAfter(c, 600), 7);
    expect([c.stale_after_s, c.hud]).toEqual([600, { max_items: 7 }]);
    expect(setHudMax(setStaleAfter(c, undefined), undefined)).not.toHaveProperty("hud");
  });

  it("fits the map area to a floor plan's proportions inside the panel's map space", () => {
    expect(fitMapAreaToAspect(blankConfig(), 2000, 1000).canvas.map_area).toEqual({ x: 0, y: 64, width: 920, height: 460 });
    expect(fitMapAreaToAspect(blankConfig(), 1000, 2000).canvas.map_area).toEqual({ x: 0, y: 64, width: 368, height: 736 });
    const c = blankConfig();
    expect(fitMapAreaToAspect(c, 0, 10)).toBe(c);
  });

  it("keeps the fitted map area inside a canvas smaller than the panel's", () => {
    // Review finding: on a 1280 x 768 canvas the region's bottom (800) is off the canvas.
    const short = { ...blankConfig(), canvas: { ...blankConfig().canvas, height: 768 } };
    const fitted = fitMapAreaToAspect(short, 1250, 1000).canvas.map_area!;
    expect(fitted.y + fitted.height).toBeLessThanOrEqual(768);
    expect(fitted).toEqual({ x: 0, y: 64, width: 880, height: 704 });
    const narrow = { ...blankConfig(), canvas: { ...blankConfig().canvas, width: 800, height: 600 } };
    const n = fitMapAreaToAspect(narrow, 2000, 1000).canvas.map_area!;
    expect(n.x + n.width).toBeLessThanOrEqual(800);
    expect(n.y + n.height).toBeLessThanOrEqual(600);
    // A canvas too small for the region's origin falls back to its own corner.
    const tiny = { ...blankConfig(), canvas: { ...blankConfig().canvas, width: 100, height: 50 } };
    const t = fitMapAreaToAspect(tiny, 100, 50).canvas.map_area!;
    expect(t).toEqual({ x: 0, y: 0, width: 100, height: 50 });
  });
});

describe("purity", () => {
  it("never mutates its input", () => {
    const before = example();
    const frozen = clone(before);
    renameRoom(before, 0, "Lounge");
    movePin(before, 0, [0.9, 0.9]);
    setPinKind(before, 10, "temperature");
    deleteRoom(before, 0);
    setLayerField(before, "air", "warn_above", undefined);
    expect(before).toEqual(frozen);
  });
});
