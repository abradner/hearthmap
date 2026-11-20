// Config <-> JSON text. Export order follows the schema's own property order (read from the
// schema, so it cannot drift), except inside `entities`, whose order is the author's. Values
// are never changed here: no defaults filled, no rounding (edits round when they write).

import schema from "../../../schema/hearthmap.schema.json";
import type { Config } from "./types";

type Json = null | boolean | number | string | Json[] | { [k: string]: Json };

interface SchemaNode {
  properties?: Record<string, unknown>;
  $ref?: string;
}

const defs = (schema as { $defs: Record<string, SchemaNode> }).$defs;

function keysOf(node: SchemaNode | undefined): string[] {
  return node?.properties ? Object.keys(node.properties) : [];
}

/** Property order per object kind, straight from the schema. */
export const KEY_ORDER = {
  top: keysOf(schema),
  canvas: keysOf(defs.canvas),
  canvas_rect: keysOf(defs.canvas_rect),
  room: keysOf(defs.room),
  pin: keysOf(defs.pin),
  calibration: keysOf(defs.calibration),
  layers: keysOf(defs.layers),
  layer: keysOf(defs.layer),
  hud: keysOf(defs.hud),
  heat: keysOf(defs.heat),
} as const;

/** Copy `obj` with known keys first in `order`, then any others in their original order. */
function ordered(obj: Record<string, Json>, order: readonly string[], child?: (k: string, v: Json) => Json) {
  const out: Record<string, Json> = {};
  const map = (k: string, v: Json) => (child ? child(k, v) : v);
  for (const k of order) if (k in obj && obj[k] !== undefined) out[k] = map(k, obj[k]);
  for (const k of Object.keys(obj)) if (!(k in out) && obj[k] !== undefined) out[k] = map(k, obj[k]);
  return out;
}

const isObj = (v: Json): v is Record<string, Json> => typeof v === "object" && v !== null && !Array.isArray(v);

/** The config with every object's keys in canonical order. Pure; the input is not mutated. */
export function canonicalise(config: Config): Config {
  const c = JSON.parse(JSON.stringify(config)) as Record<string, Json>;
  const out = ordered(c, KEY_ORDER.top, (k, v) => {
    if (!isObj(v) && !Array.isArray(v)) return v;
    switch (k) {
      case "canvas":
        return isObj(v)
          ? ordered(v, KEY_ORDER.canvas, (ck, cv) => (ck === "map_area" && isObj(cv) ? ordered(cv, KEY_ORDER.canvas_rect) : cv))
          : v;
      case "rooms":
        return Array.isArray(v) ? v.map((r) => (isObj(r) ? ordered(r, KEY_ORDER.room) : r)) : v;
      case "pins":
        return Array.isArray(v)
          ? v.map((p) =>
              isObj(p)
                ? ordered(p, KEY_ORDER.pin, (pk, pv) => (pk === "calibration" && isObj(pv) ? ordered(pv, KEY_ORDER.calibration) : pv))
                : p,
            )
          : v;
      case "layers":
        return isObj(v) ? ordered(v, KEY_ORDER.layers, (_lk, lv) => (isObj(lv) ? ordered(lv, KEY_ORDER.layer) : lv)) : v;
      case "hud":
        return isObj(v) ? ordered(v, KEY_ORDER.hud) : v;
      case "heat":
        return isObj(v) ? ordered(v, KEY_ORDER.heat) : v;
      default:
        return v;
    }
  });
  return out as unknown as Config;
}

/** The exported config.json text: canonical key order, two-space indent, trailing newline. */
export function serialiseConfig(config: Config): string {
  return `${JSON.stringify(canonicalise(config), null, 2)}\n`;
}

/**
 * Normal form for comparing two config files: parse and re-print with two-space indent. It
 * removes formatting differences only ("0.0" and "0" are the same number); key order and
 * values are kept, so it is the yardstick for "byte-equivalent after normalisation".
 */
export function normaliseJsonText(text: string): string {
  return `${JSON.stringify(JSON.parse(text), null, 2)}\n`;
}
