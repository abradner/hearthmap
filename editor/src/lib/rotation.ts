// canvas.map_rotation (docs/decisions.md D-080). The config stores rooms and pins in the floor
// plan's own frame; the panel shows them turned clockwise by map_rotation inside map_area. Core
// turns the config once as it loads (apply_map_rotation(), config.h); the editor turns only what
// it draws and turns pointer positions back, so it never writes a turned coordinate.
//
// Three frames here: plan (what the config stores, 0..1), view (0..1 over the map area as the
// panel shows it), and world (view times the map area's pixel size, geometry.ts).

import { clamp01, roundCoord, type Size, type Vec } from "./geometry";
import type { Config, MapPoint, MapRotation } from "./types";

/** The config's rotation; a config without the field is turned 0 degrees (the schema default). */
export function mapRotationOf(config: Config): MapRotation {
  return config.canvas.map_rotation ?? 0;
}

/**
 * Core's rotate_map_point() (geometry.h): `quarterTurns` x 90 degrees clockwise as seen on the
 * panel, y down. One turn takes (x, y) to (1 - y, x); turns are taken modulo 4. The same double
 * arithmetic as core, so the same values come out.
 */
export function rotatePoint(p: MapPoint, quarterTurns: number): MapPoint {
  switch ((((quarterTurns % 4) + 4) % 4) as 0 | 1 | 2 | 3) {
    case 1:
      return [1 - p[1], p[0]];
    case 2:
      return [1 - p[0], 1 - p[1]];
    case 3:
      return [p[1], 1 - p[0]];
    default:
      return [p[0], p[1]];
  }
}

export function planToView(p: MapPoint, rotation: MapRotation): MapPoint {
  return rotatePoint(p, rotation / 90);
}

export function viewToPlan(p: MapPoint, rotation: MapRotation): MapPoint {
  return rotatePoint(p, -rotation / 90);
}

/** A stored point as world pixels on the turned map area. */
export function planToWorld(p: MapPoint, size: Size, rotation: MapRotation): Vec {
  const v = planToView(p, rotation);
  return { x: v[0] * size.width, y: v[1] * size.height };
}

/**
 * World pixels on the turned map area -> the stored (plan-frame) point under them: clamped onto
 * the map in the view frame (a quarter turn maps the unit square onto itself), turned back, then
 * rounded to the stored precision. Every editing gesture writes through this.
 */
export function worldToPlan(w: Vec, size: Size, rotation: MapRotation): MapPoint {
  const p = viewToPlan([clamp01(w.x / size.width), clamp01(w.y / size.height)], rotation);
  return [roundCoord(clamp01(p[0])), roundCoord(clamp01(p[1]))];
}

/** Whether a rotation swaps the plan's width and height on screen. */
export function swapsAxes(rotation: MapRotation): boolean {
  return rotation === 90 || rotation === 270;
}

/**
 * The config as the panel draws it: core's apply_map_rotation() for the preview. Rooms and pins
 * turned, the field dropped, so applying it twice is applying it once. Never exported.
 */
export function inViewFrame(config: Config): Config {
  const rotation = mapRotationOf(config);
  if (rotation === 0) return config;
  const { map_rotation: _drop, ...canvas } = config.canvas;
  return {
    ...config,
    canvas,
    rooms: config.rooms.map((r) => ({ ...r, polygon: r.polygon.map((p) => planToView(p, rotation)) })),
    pins: config.pins.map((p) => ({ ...p, position: planToView(p.position, rotation) })),
  };
}
