// Validation before export, in two parts:
//   1. the real schema (schema/hearthmap.schema.json) through a pinned JSON Schema validator
//      (Ajv, draft 2020-12). This is the contract check: an export that fails it is refused.
//   2. editor checks for rules the schema cannot express (unique ids, room references, polygon
//      simplicity, calibration width, band ordering, ...). Core's validate() (config.h) is the
//      enforcement for these; the editor repeats them only so an author sees the problem next
//      to the item before the firmware build does (docs/decisions.md D-040). Deleting them
//      would change politeness, not possibility.

import { Ajv2020, type ErrorObject } from "ajv/dist/2020.js";
import schema from "../../../schema/hearthmap.schema.json";
import { DEFAULT_BANDS, DEFAULT_LAYER, MAX_PINS, MAX_ROOMS, allowsMoisture, defaultMapArea, needsCalibration } from "./defaults";
import { findDuplicateBindings } from "./entities";
import { isSimplePolygon, pointInPolygon } from "./geometry";
import { BAND_FIELDS, LAYERS, type BandField, type Config, type Kind, type LayerName, type Role } from "./types";

export type IssueSeverity = "error" | "warning";

export interface Issue {
  severity: IssueSeverity;
  /** JSON pointer into the config, as in core's ConfigIssue ("/pins/3/room"). */
  path: string;
  message: string;
  source: "schema" | "editor";
}

// strict: false because Ajv's strict mode is a lint of Ajv-specific style, and it rejects
// standard 2020-12 idioms the contract uses (a "required" under "not" naming a property the
// subschema does not declare). The schema itself is still checked against the meta-schema.
const ajv = new Ajv2020({ allErrors: true, strict: false });
const validateSchema = ajv.compile(schema);

function describe(e: ErrorObject): string {
  switch (e.keyword) {
    case "required":
      return `missing "${String(e.params.missingProperty)}"`;
    case "additionalProperties":
      return `unknown field "${String(e.params.additionalProperty)}"`;
    case "minItems":
      return e.instancePath === "/rooms" ? "draw at least one room" : (e.message ?? e.keyword);
    case "not":
      return "this kind of pin cannot have that field (calibration or photo)";
    case "pattern":
      return `"${String(e.data)}" does not match ${String(e.params.pattern)}`;
    default:
      return e.message ?? e.keyword;
  }
}

/** Schema errors only. `if` failures are bookkeeping for the `then` errors and are dropped. */
export function schemaIssues(config: unknown): Issue[] {
  if (validateSchema(config)) return [];
  const seen = new Set<string>();
  const out: Issue[] = [];
  for (const e of validateSchema.errors ?? []) {
    if (e.keyword === "if") continue;
    const path = e.keyword === "required" ? `${e.instancePath}/${String(e.params.missingProperty)}` : e.instancePath;
    const message = describe(e);
    const key = `${path}|${message}`;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ severity: "error", path: path || "/", message, source: "schema" });
  }
  return out;
}

/** Layer values with defaults filled in; null means "no threshold". */
export function effectiveBand(config: Config, layer: LayerName): Record<BandField, number | null> {
  const set = config.layers?.[layer] ?? {};
  const out = { ...DEFAULT_BANDS[layer] };
  for (const f of BAND_FIELDS) {
    const v = set[f];
    if (v !== undefined) out[f] = v;
  }
  return out;
}

export function layerEnabled(config: Config, layer: LayerName): boolean {
  return config.layers?.[layer]?.enabled ?? true;
}

const ORDER: readonly BandField[] = [
  "scale_min",
  "danger_below",
  "warn_below",
  "comfort_min",
  "comfort_max",
  "warn_above",
  "danger_above",
  "scale_max",
];

function bandOrderIssue(band: Record<BandField, number | null>): string | undefined {
  const present = ORDER.filter((f) => band[f] !== null).map((f) => [f, band[f] as number] as const);
  for (let i = 1; i < present.length; i++) {
    const [pf, pv] = present[i - 1] as readonly [BandField, number];
    const [f, value] = present[i] as readonly [BandField, number];
    if (value < pv) return `${f} (${value}) is below ${pf} (${pv})`;
  }
  if ((band.scale_min as number) >= (band.scale_max as number)) return "scale_min must be below scale_max";
  return undefined;
}

/** Which layer a role on a pin of `kind` feeds, following core's feeds_layer() (D-004). */
export function fedLayer(kind: Kind, role: Role): LayerName | undefined {
  const layer: Partial<Record<Role, LayerName>> = {
    temperature: "temperature",
    humidity: "humidity",
    moisture: "soil",
    co2: "air",
  };
  const l = layer[role];
  if (!l) return undefined;
  if ((kind === "soil" || kind === "plant") && (role === "temperature" || role === "humidity")) return undefined;
  if (role === "moisture" && !allowsMoisture(kind)) return undefined;
  return l;
}

/**
 * Rules the schema cannot express. Assumes the config is schema-shaped enough to walk; call it
 * only when schemaIssues() found nothing structural (validateConfig does this).
 */
export function editorIssues(config: Config, photoIds: ReadonlySet<string> = new Set()): Issue[] {
  const out: Issue[] = [];
  const err = (path: string, message: string) => out.push({ severity: "error", path, message, source: "editor" });
  const warn = (path: string, message: string) => out.push({ severity: "warning", path, message, source: "editor" });

  // Canvas.
  const c = config.canvas;
  if (c.orientation === "landscape" && !(c.width > c.height)) err("/canvas", "landscape needs width > height");
  if (c.orientation === "portrait" && !(c.height > c.width)) err("/canvas", "portrait needs height > width");
  const ma = c.map_area ?? defaultMapArea(c);
  if (ma.x + ma.width > c.width || ma.y + ma.height > c.height) err("/canvas/map_area", "map area must lie inside the canvas");

  if (config.rooms.length === 0) err("/rooms", "draw at least one room");
  if (config.rooms.length > MAX_ROOMS) err("/rooms", `at most ${MAX_ROOMS} rooms`);
  if (config.pins.length > MAX_PINS) err("/pins", `at most ${MAX_PINS} pins`);

  // Rooms.
  const roomIds = new Map<string, number>();
  config.rooms.forEach((r, i) => {
    if (roomIds.has(r.id)) err(`/rooms/${i}/id`, `duplicate room id "${r.id}"`);
    else roomIds.set(r.id, i);
    if (!isSimplePolygon(r.polygon)) err(`/rooms/${i}/polygon`, "outline crosses itself or repeats a vertex");
  });

  // Pins.
  const pinIds = new Set<string>();
  config.pins.forEach((p, i) => {
    if (pinIds.has(p.id)) err(`/pins/${i}/id`, `duplicate pin id "${p.id}"`);
    pinIds.add(p.id);
    const roomIndex = roomIds.get(p.room);
    if (roomIndex === undefined) err(`/pins/${i}/room`, `unknown room "${p.room}"`);
    else if (!pointInPolygon(config.rooms[roomIndex]?.polygon ?? [], p.position)) {
      warn(`/pins/${i}/position`, `pin is outside its room "${p.room}"`);
    }
    if (p.entities.moisture !== undefined && !allowsMoisture(p.kind)) {
      err(`/pins/${i}/entities/moisture`, "only soil and plant pins can bind moisture");
    }
    const cal = p.calibration;
    if (cal && needsCalibration(p.kind)) {
      if (Math.abs(cal.wet - cal.dry) < 1e-9) err(`/pins/${i}/calibration`, "dry and wet must differ");
      else {
        const lo = Math.min(cal.dry, cal.wet);
        const hi = Math.max(cal.dry, cal.wet);
        for (const f of ["happy_min", "happy_max"] as const) {
          const hv = cal[f];
          if (hv !== undefined && (hv < lo || hv > hi)) err(`/pins/${i}/calibration/${f}`, `${f} must lie between dry and wet`);
        }
        if (cal.happy_min !== undefined && cal.happy_max !== undefined) {
          // "Wetter" is towards `wet`, whichever way the range runs.
          const wetness = (x: number) => (x - cal.dry) / (cal.wet - cal.dry);
          if (wetness(cal.happy_min) > wetness(cal.happy_max)) {
            err(`/pins/${i}/calibration/happy_min`, "happy_min is wetter than happy_max");
          }
        }
      }
    }
    if (p.photo !== undefined && !photoIds.has(p.photo)) {
      warn(`/pins/${i}/photo`, `no image loaded for photo "${p.photo}": the export will not include photos/${p.photo}.png`);
    }
  });

  for (const [entity, uses] of findDuplicateBindings(config)) {
    for (const u of uses) {
      warn(`/pins/${u.pin}/entities/${u.role}`, `${entity} is bound ${uses.length} times`);
    }
  }

  // Layers.
  for (const layer of LAYERS) {
    const msg = bandOrderIssue(effectiveBand(config, layer));
    if (msg) err(`/layers/${layer}`, msg);
  }
  const def = config.layers?.default ?? DEFAULT_LAYER;
  if (!layerEnabled(config, def)) err("/layers/default", `default layer "${def}" is disabled`);
  if (config.temperature_unit === "F") {
    const set = config.layers?.temperature ?? {};
    const missing = BAND_FIELDS.filter((f) => set[f] === undefined);
    if (missing.length) {
      err("/layers/temperature", `with °F every temperature band field must be set (missing: ${missing.join(", ")})`);
    }
  }
  for (const layer of LAYERS) {
    if (!layerEnabled(config, layer)) continue;
    const fed = config.pins.some((p) =>
      (Object.keys(p.entities) as Role[]).some((role) => fedLayer(p.kind, role) === layer),
    );
    if (!fed) warn(`/layers/${layer}`, `no pin feeds the ${layer} layer`);
  }
  return out;
}

/** Schema first; editor checks only on a schema-valid config (they assume its shape). */
export function validateConfig(config: unknown, photoIds?: ReadonlySet<string>): Issue[] {
  const s = schemaIssues(config);
  if (s.length) return s;
  return editorIssues(config as Config, photoIds);
}

export function hasErrors(issues: readonly Issue[]): boolean {
  return issues.some((i) => i.severity === "error");
}

export type IssueTarget =
  | { kind: "room"; index: number }
  | { kind: "pin"; index: number }
  | { kind: "layer"; layer: LayerName | "default" }
  | { kind: "project" };

/** Which editor item an issue belongs to, so the UI can show it next to that item. */
export function issueTarget(path: string): IssueTarget {
  const parts = path.split("/").filter((s) => s.length > 0);
  const [head, second] = parts;
  if ((head === "rooms" || head === "pins") && second !== undefined && /^\d+$/.test(second)) {
    return { kind: head === "rooms" ? "room" : "pin", index: Number(second) };
  }
  if (head === "layers" && second !== undefined && (LAYERS as readonly string[]).concat("default").includes(second)) {
    return { kind: "layer", layer: second as LayerName | "default" };
  }
  return { kind: "project" };
}

const isNum = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);
const isObj = (v: unknown): v is Record<string, unknown> => typeof v === "object" && v !== null && !Array.isArray(v);
const isPoint = (v: unknown): boolean => Array.isArray(v) && v.length === 2 && isNum(v[0]) && isNum(v[1]);

/**
 * The shape the editor needs to render and edit a config at all, or a message saying what is
 * missing. Weaker than the schema on purpose: a blank project (no rooms yet) is fine here, so
 * autosave can restore work in progress. Anything this accepts can be drawn without throwing.
 */
export function structuralIssue(data: unknown): string | undefined {
  if (!isObj(data)) return "not a JSON object";
  if (data.version !== 1) return "version must be 1";
  const c = data.canvas;
  if (!isObj(c) || !isNum(c.width) || !isNum(c.height) || c.width <= 0 || c.height <= 0) return "canvas needs a width and height";
  if (c.map_area !== undefined) {
    const m = c.map_area;
    if (!isObj(m) || !isNum(m.x) || !isNum(m.y) || !isNum(m.width) || !isNum(m.height) || m.width <= 0 || m.height <= 0) return "canvas.map_area is malformed";
  }
  if (!Array.isArray(data.rooms)) return "rooms must be a list";
  for (const [i, r] of data.rooms.entries()) {
    if (!isObj(r) || typeof r.id !== "string" || typeof r.name !== "string" || !Array.isArray(r.polygon) || !r.polygon.every(isPoint)) {
      return `room ${i} is malformed`;
    }
  }
  if (!Array.isArray(data.pins)) return "pins must be a list";
  for (const [i, p] of data.pins.entries()) {
    if (!isObj(p) || typeof p.id !== "string" || typeof p.name !== "string" || typeof p.kind !== "string" || !isPoint(p.position) || !isObj(p.entities)) {
      return `pin ${i} is malformed`;
    }
  }
  return undefined;
}

export type ImportCheck = { ok: true; config: Config } | { ok: false; message: string };

/**
 * Whether an opened config may replace the current project. It must be renderable and valid
 * against the schema; semantic errors the schema cannot see (duplicate ids, a self-crossing
 * room) are allowed through, because they are fixable in the editor and are listed there.
 * Call this before touching application state or autosave.
 */
export function checkImportedConfig(data: unknown): ImportCheck {
  const structural = structuralIssue(data);
  if (structural) return { ok: false, message: structural };
  const issues = schemaIssues(data);
  if (issues.length) {
    const first = issues.slice(0, 3).map((i) => `${i.path} ${i.message}`).join("; ");
    return { ok: false, message: `does not match the schema: ${first}${issues.length > 3 ? ` (+${issues.length - 3} more)` : ""}` };
  }
  return { ok: true, config: data as Config };
}
