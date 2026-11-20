// Plane geometry for the editor. Two spaces:
//   - map space: what the config stores, 0..1 over the map area, y down (schema/README.md);
//   - world space: map-area pixels (map space times map_area width/height). The editor
//     measures snapping and hit distances here, so they are undistorted whatever the aspect.
// Screen space (world after pan/zoom) is the UI's business, not this module's.

import type { MapPoint } from "./types";

export interface Vec {
  x: number;
  y: number;
}

export interface Size {
  width: number;
  height: number;
}

export const COORD_DECIMALS = 4;

export function roundCoord(v: number): number {
  const f = 10 ** COORD_DECIMALS;
  const r = Math.round(v * f) / f;
  return Object.is(r, -0) ? 0 : r;
}

export function clamp01(v: number): number {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

export function mapToWorld(p: MapPoint, size: Size): Vec {
  return { x: p[0] * size.width, y: p[1] * size.height };
}

/** World -> map space, clamped into 0..1 and rounded to the stored precision. */
export function worldToMap(v: Vec, size: Size): MapPoint {
  return [roundCoord(clamp01(v.x / size.width)), roundCoord(clamp01(v.y / size.height))];
}

export function dist(a: Vec, b: Vec): number {
  return Math.hypot(a.x - b.x, a.y - b.y);
}

/**
 * Even-odd point-in-polygon with core's boundary rule (geometry.h): an edge (a, b) counts a
 * crossing when (a.y > p.y) != (b.y > p.y) and p.x is left of the edge at p.y. Points on left
 * and top edges are inside, on right and bottom edges outside, so adjacent rooms never both
 * claim a point.
 */
export function pointInPolygon(poly: readonly MapPoint[], p: MapPoint): boolean {
  let inside = false;
  const n = poly.length;
  for (let i = 0, j = n - 1; i < n; j = i++) {
    const a = poly[j] as MapPoint;
    const b = poly[i] as MapPoint;
    if (a[1] > p[1] !== b[1] > p[1]) {
      const xAtY = a[0] + ((p[1] - a[1]) * (b[0] - a[0])) / (b[1] - a[1]);
      if (p[0] < xAtY) inside = !inside;
    }
  }
  return inside;
}

/** Index of the first room whose polygon contains `p` (earlier rooms own overlaps), or -1. */
export function roomIndexAt(rooms: readonly { polygon: readonly MapPoint[] }[], p: MapPoint): number {
  for (let i = 0; i < rooms.length; i++) {
    if (pointInPolygon((rooms[i] as { polygon: readonly MapPoint[] }).polygon, p)) return i;
  }
  return -1;
}

function cross(o: Vec, a: Vec, b: Vec): number {
  return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

function onSegment(a: Vec, b: Vec, p: Vec): boolean {
  return (
    Math.min(a.x, b.x) <= p.x &&
    p.x <= Math.max(a.x, b.x) &&
    Math.min(a.y, b.y) <= p.y &&
    p.y <= Math.max(a.y, b.y)
  );
}

/** True when closed segments ab and cd share at least one point (touching counts). */
export function segmentsIntersect(a: Vec, b: Vec, c: Vec, d: Vec): boolean {
  const d1 = cross(c, d, a);
  const d2 = cross(c, d, b);
  const d3 = cross(a, b, c);
  const d4 = cross(a, b, d);
  if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))) {
    return true;
  }
  if (d1 === 0 && onSegment(c, d, a)) return true;
  if (d2 === 0 && onSegment(c, d, b)) return true;
  if (d3 === 0 && onSegment(a, b, c)) return true;
  if (d4 === 0 && onSegment(a, b, d)) return true;
  return false;
}

const v = (p: MapPoint): Vec => ({ x: p[0], y: p[1] });

/**
 * A polygon is simple when no two non-adjacent edges touch, and no vertex repeats. Fewer than
 * three vertices is not a polygon at all, so not simple.
 */
export function isSimplePolygon(poly: readonly MapPoint[]): boolean {
  const n = poly.length;
  if (n < 3) return false;
  for (let i = 0; i < n; i++) {
    for (let j = i + 1; j < n; j++) {
      const pi = poly[i] as MapPoint;
      const pj = poly[j] as MapPoint;
      if (pi[0] === pj[0] && pi[1] === pj[1]) return false;
    }
  }
  for (let i = 0; i < n; i++) {
    const a1 = v(poly[i] as MapPoint);
    const a2 = v(poly[(i + 1) % n] as MapPoint);
    for (let j = i + 1; j < n; j++) {
      // Adjacent edges share a vertex by construction; skip them.
      if (j === i + 1 || (i === 0 && j === n - 1)) continue;
      const b1 = v(poly[j] as MapPoint);
      const b2 = v(poly[(j + 1) % n] as MapPoint);
      if (segmentsIntersect(a1, a2, b1, b2)) return false;
    }
  }
  // Collinear adjacent edges that fold back on themselves (a spike) overlap without the test
  // above seeing it: check that consecutive edges do not reverse along one line.
  for (let i = 0; i < n; i++) {
    const a = v(poly[(i + n - 1) % n] as MapPoint);
    const b = v(poly[i] as MapPoint);
    const c = v(poly[(i + 1) % n] as MapPoint);
    if (cross(a, b, c) === 0) {
      const dot = (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y);
      if (dot < 0) return false;
    }
  }
  return true;
}

/** Closest point to `p` on segment ab, and the parameter t (0 at a, 1 at b). */
export function projectOnSegment(p: Vec, a: Vec, b: Vec): { point: Vec; t: number } {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const len2 = dx * dx + dy * dy;
  if (len2 === 0) return { point: { ...a }, t: 0 };
  let t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return { point: { x: a.x + t * dx, y: a.y + t * dy }, t };
}

/**
 * The polygon edge nearest `p` (all in world space). `edge` is the index of the edge's first
 * vertex; inserting at `edge + 1` splits that edge.
 */
export function nearestEdge(poly: readonly Vec[], p: Vec): { edge: number; point: Vec; distance: number } {
  let best = { edge: -1, point: { x: 0, y: 0 }, distance: Infinity };
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i] as Vec;
    const b = poly[(i + 1) % poly.length] as Vec;
    const { point } = projectOnSegment(p, a, b);
    const d = dist(p, point);
    if (d < best.distance) best = { edge: i, point, distance: d };
  }
  return best;
}

export type SnapKind = "none" | "vertex" | "axis";

export interface SnapResult {
  point: Vec;
  kind: SnapKind;
  /** The world-space guide lines the UI may draw: vertical at x, horizontal at y. */
  guides: { x?: number; y?: number };
}

/**
 * Snap a world point. Vertex snap wins: if any of `vertices` lies within `tolerance`, the
 * nearest one is returned exactly. Otherwise each axis snaps independently to the nearest
 * `axisRefs` coordinate within tolerance, which makes edges horizontal or vertical relative to
 * the neighbouring vertices.
 */
export function snap(p: Vec, vertices: readonly Vec[], axisRefs: readonly Vec[], tolerance: number): SnapResult {
  let nearest: Vec | undefined;
  let nearestD = Infinity;
  for (const q of vertices) {
    const d = dist(p, q);
    if (d <= tolerance && d < nearestD) {
      nearest = q;
      nearestD = d;
    }
  }
  if (nearest) return { point: { ...nearest }, kind: "vertex", guides: {} };

  let x = p.x;
  let y = p.y;
  let bestDx = Infinity;
  let bestDy = Infinity;
  const guides: SnapResult["guides"] = {};
  for (const r of axisRefs) {
    const dx = Math.abs(r.x - p.x);
    if (dx <= tolerance && dx < bestDx) {
      bestDx = dx;
      x = r.x;
      guides.x = r.x;
    }
    const dy = Math.abs(r.y - p.y);
    if (dy <= tolerance && dy < bestDy) {
      bestDy = dy;
      y = r.y;
      guides.y = r.y;
    }
  }
  const kind: SnapKind = guides.x !== undefined || guides.y !== undefined ? "axis" : "none";
  return { point: { x, y }, kind, guides };
}

/** Area-weighted centroid in map space; falls back to the vertex mean for a zero-area shape. */
export function centroid(poly: readonly MapPoint[]): MapPoint {
  let a2 = 0;
  let cx = 0;
  let cy = 0;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const p = poly[j] as MapPoint;
    const q = poly[i] as MapPoint;
    const f = p[0] * q[1] - q[0] * p[1];
    a2 += f;
    cx += (p[0] + q[0]) * f;
    cy += (p[1] + q[1]) * f;
  }
  if (a2 === 0) {
    const n = poly.length || 1;
    return [poly.reduce((s, p) => s + p[0], 0) / n, poly.reduce((s, p) => s + p[1], 0) / n];
  }
  return [cx / (3 * a2), cy / (3 * a2)];
}

/** Axis-aligned bounds of a polygon in map space. */
export function bounds(poly: readonly MapPoint[]): { minX: number; minY: number; maxX: number; maxY: number } {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const [x, y] of poly) {
    minX = Math.min(minX, x);
    minY = Math.min(minY, y);
    maxX = Math.max(maxX, x);
    maxY = Math.max(maxY, y);
  }
  return { minX, minY, maxX, maxY };
}
