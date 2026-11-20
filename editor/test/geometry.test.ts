import { describe, expect, it } from "vitest";
import {
  centroid,
  isSimplePolygon,
  nearestEdge,
  pointInPolygon,
  roomIndexAt,
  roundCoord,
  snap,
  worldToMap,
  mapToWorld,
} from "../src/lib/geometry";
import type { MapPoint } from "../src/lib/types";

const square: MapPoint[] = [
  [0.2, 0.2],
  [0.6, 0.2],
  [0.6, 0.6],
  [0.2, 0.6],
];

describe("pointInPolygon (core's half-open rule)", () => {
  it("contains interior points and excludes exterior ones", () => {
    expect(pointInPolygon(square, [0.4, 0.4])).toBe(true);
    expect(pointInPolygon(square, [0.7, 0.4])).toBe(false);
  });
  it("puts left and top edges inside, right and bottom edges outside", () => {
    expect(pointInPolygon(square, [0.2, 0.4])).toBe(true); // left
    expect(pointInPolygon(square, [0.4, 0.2])).toBe(true); // top
    expect(pointInPolygon(square, [0.6, 0.4])).toBe(false); // right
    expect(pointInPolygon(square, [0.4, 0.6])).toBe(false); // bottom
  });
  it("gives a shared wall to exactly one of two adjacent rooms", () => {
    const right: MapPoint[] = [
      [0.6, 0.2],
      [0.9, 0.2],
      [0.9, 0.6],
      [0.6, 0.6],
    ];
    const p: MapPoint = [0.6, 0.4];
    expect([pointInPolygon(square, p), pointInPolygon(right, p)]).toEqual([false, true]);
  });
  it("handles concave outlines", () => {
    const l: MapPoint[] = [
      [0, 0],
      [1, 0],
      [1, 0.5],
      [0.5, 0.5],
      [0.5, 1],
      [0, 1],
    ];
    expect(pointInPolygon(l, [0.75, 0.75])).toBe(false);
    expect(pointInPolygon(l, [0.25, 0.75])).toBe(true);
  });
});

describe("roomIndexAt", () => {
  it("returns the earliest room where rooms overlap, -1 outside all", () => {
    const rooms = [{ polygon: square }, { polygon: square }];
    expect(roomIndexAt(rooms, [0.3, 0.3])).toBe(0);
    expect(roomIndexAt(rooms, [0.9, 0.9])).toBe(-1);
  });
});

describe("isSimplePolygon", () => {
  it("accepts a square and a triangle", () => {
    expect(isSimplePolygon(square)).toBe(true);
    expect(isSimplePolygon(square.slice(0, 3))).toBe(true);
  });
  it("rejects a bow tie", () => {
    expect(
      isSimplePolygon([
        [0, 0],
        [1, 1],
        [1, 0],
        [0, 1],
      ]),
    ).toBe(false);
  });
  it("rejects a repeated vertex and fewer than three points", () => {
    expect(isSimplePolygon([...square, [0.2, 0.2]])).toBe(false);
    expect(
      isSimplePolygon([
        [0, 0],
        [1, 1],
      ]),
    ).toBe(false);
  });
  it("rejects a spike that folds back along one line", () => {
    expect(
      isSimplePolygon([
        [0, 0],
        [1, 0],
        [0.5, 0],
        [0.5, 1],
      ]),
    ).toBe(false);
  });
  it("rejects a vertex touching a non-adjacent edge", () => {
    expect(
      isSimplePolygon([
        [0, 0],
        [1, 0],
        [1, 1],
        [0.5, 0],
        [0, 1],
      ]),
    ).toBe(false);
  });
});

describe("snap", () => {
  const verts = [
    { x: 100, y: 100 },
    { x: 200, y: 100 },
  ];
  it("snaps to the nearest vertex within tolerance, and vertex beats axis", () => {
    const r = snap({ x: 104, y: 97 }, verts, [{ x: 105, y: 0 }], 10);
    expect(r).toMatchObject({ kind: "vertex", point: { x: 100, y: 100 } });
  });
  it("snaps each axis independently to the reference coordinates", () => {
    const r = snap({ x: 205, y: 300 }, [], [{ x: 200, y: 296 }], 10);
    expect(r).toMatchObject({ kind: "axis", point: { x: 200, y: 296 }, guides: { x: 200, y: 296 } });
    const onlyX = snap({ x: 205, y: 350 }, [], [{ x: 200, y: 296 }], 10);
    expect(onlyX).toMatchObject({ kind: "axis", point: { x: 200, y: 350 }, guides: { x: 200 } });
  });
  it("leaves the point alone beyond tolerance", () => {
    expect(snap({ x: 150, y: 150 }, verts, verts, 10)).toMatchObject({ kind: "none", point: { x: 150, y: 150 } });
  });
  it("picks the nearer of two candidate vertices", () => {
    const r = snap({ x: 195, y: 100 }, [{ x: 188, y: 100 }, { x: 200, y: 100 }], [], 10);
    expect(r.point).toEqual({ x: 200, y: 100 });
  });
});

describe("nearestEdge", () => {
  it("finds the edge and the projected point", () => {
    const poly = [
      { x: 0, y: 0 },
      { x: 100, y: 0 },
      { x: 100, y: 100 },
      { x: 0, y: 100 },
    ];
    expect(nearestEdge(poly, { x: 50, y: 5 })).toMatchObject({ edge: 0, point: { x: 50, y: 0 }, distance: 5 });
    expect(nearestEdge(poly, { x: 2, y: 50 })).toMatchObject({ edge: 3 }); // the closing edge
  });
});

describe("coordinate conversion", () => {
  it("rounds to four decimals, clamps to 0..1 and never writes -0", () => {
    expect(worldToMap({ x: 480.04, y: -3 }, { width: 960, height: 800 })).toEqual([0.5, 0]);
    expect(worldToMap({ x: 2000, y: 400 }, { width: 960, height: 800 })).toEqual([1, 0.5]);
    expect(Object.is(roundCoord(-0.00001), 0)).toBe(true);
    expect(mapToWorld([0.5, 0.25], { width: 960, height: 800 })).toEqual({ x: 480, y: 200 });
  });
  it("centroid of a square is its centre", () => {
    const [x, y] = centroid(square);
    expect(x).toBeCloseTo(0.4);
    expect(y).toBeCloseTo(0.4);
  });
});
