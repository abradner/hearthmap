// What the live preview draws, computed from (config, readings, layer). This is an
// APPROXIMATION of core's view model for judging placement in the editor; it is not the
// device's logic and the UI says so. The firmware and the host renderer use core (C++), which
// is the only tested source of truth for severity, thirst, the heat field and the HUD
// (AGENTS.md principle 2). Rulings followed: R2 (HUD dry side), R10 (outdoor rooms get no
// heat colour), D-004/D-006/D-008/D-009/D-012.

import { inViewFrame } from "../lib/rotation";
import { DEFAULT_HEAT, DEFAULT_HUD_MAX_ITEMS, DEFAULT_STALE_AFTER_S, PRIMARY_ROLE, defaultMapArea } from "../lib/defaults";
import { pointInPolygon } from "../lib/geometry";
import { effectiveBand, fedLayer, layerEnabled } from "../lib/validate";
import type { Band } from "../lib/defaults";
import type { Calibration, CanvasRect, Config, LayerName, MapPoint, Pin, Role } from "../lib/types";

export type Severity = "ok" | "stale" | "warn" | "danger";
const RANK: Record<Severity, number> = { ok: 0, stale: 1, warn: 2, danger: 3 };

export interface Reading {
  value: number | "unavailable" | "unknown";
  /** Milliseconds since the epoch. */
  lastUpdated: number;
}

export interface ReadingSet {
  /** Milliseconds since the epoch. */
  now: number;
  readings: ReadonlyMap<string, Reading>;
}

export interface ScenePin {
  index: number;
  x: number;
  y: number;
  kind: Pin["kind"];
  name: string;
  severity: Severity;
  /** Value text shown next to a dot pin, or "no data". */
  label: string;
  /** Plant/soil: moisture fraction 0..1 for the ring; undefined when stale. */
  moisture?: number;
}

export interface SceneRoom {
  name: string;
  outdoor: boolean;
  points: { x: number; y: number }[];
  pulse?: "warn" | "danger";
}

export interface HudItem {
  pin: number;
  name: string;
  room: string;
  percent: number;
  severity: "warn" | "danger";
}

export interface Scene {
  mapArea: CanvasRect;
  canvas: { width: number; height: number };
  layer: LayerName;
  enabledLayers: LayerName[];
  rooms: SceneRoom[];
  pins: ScenePin[];
  heat: { cols: number; rows: number; cells: (number | null | undefined)[]; band: Band };
  hud: { items: HudItem[]; overflow: number; total: number };
}

export function thirstOf(cal: Calibration, raw: number): number | undefined {
  if (!Number.isFinite(raw) || Math.abs(cal.wet - cal.dry) < 1e-9) return undefined;
  const f = Math.min(1, Math.max(0, (raw - cal.dry) / (cal.wet - cal.dry)));
  return 1 - f;
}

/** Core's effective_soil_band(), approximately (calibration.h). */
export function soilBand(layer: Band, cal: Calibration): Band {
  const b = { ...layer };
  if (Math.abs(cal.wet - cal.dry) < 1e-9) return b;
  if (cal.happy_min !== undefined) {
    const t = thirstOf(cal, cal.happy_min) as number;
    b.comfort_max = t;
    b.warn_above = t;
    b.danger_above = layer.danger_above === null ? null : Math.max(layer.danger_above, t);
  }
  if (cal.happy_max !== undefined) {
    const t = thirstOf(cal, cal.happy_max) as number;
    b.comfort_min = t;
    b.warn_below = t;
    b.danger_below = layer.danger_below === null ? null : Math.min(layer.danger_below, t);
  }
  if ((b.comfort_min as number) > (b.comfort_max as number)) {
    if (cal.happy_min !== undefined && cal.happy_max === undefined) b.comfort_min = b.comfort_max;
    else b.comfort_max = b.comfort_min;
  }
  return b;
}

/** Strictly beyond a threshold is that severity; exactly on it is the milder side. */
export function classify(value: number, band: Band): "ok" | "warn" | "danger" {
  const beyond = (t: number | null, dir: 1 | -1) => t !== null && (dir === 1 ? value > t : value < t);
  if (beyond(band.danger_above, 1) || beyond(band.danger_below, -1)) return "danger";
  if (beyond(band.warn_above, 1) || beyond(band.warn_below, -1)) return "warn";
  return "ok";
}

function fresh(r: Reading | undefined, now: number, staleAfterS: number): r is Reading & { value: number } {
  if (!r || typeof r.value !== "number") return false;
  const age = Math.max(0, now - r.lastUpdated);
  return age <= staleAfterS * 1000;
}

interface Assessment {
  severity: Severity;
  value?: number;
  layerValue?: number;
}

function assess(config: Config, pin: Pin, role: Role, rs: ReadingSet, outdoor: boolean): Assessment {
  const id = pin.entities[role];
  const r = id === undefined ? undefined : rs.readings.get(id);
  const staleAfter = pin.stale_after_s ?? config.stale_after_s ?? DEFAULT_STALE_AFTER_S;
  if (!fresh(r, rs.now, staleAfter)) return { severity: "stale" };
  const layer = fedLayer(pin.kind, role);
  if (role === "moisture") {
    if (!pin.calibration) return { severity: "stale" };
    const t = thirstOf(pin.calibration, r.value);
    if (t === undefined) return { severity: "stale" };
    const judged = layerEnabled(config, "soil");
    const sev = judged ? classify(t, soilBand(effectiveBand(config, "soil"), pin.calibration)) : "ok";
    return { severity: sev, value: r.value, layerValue: t };
  }
  if (!layer || !layerEnabled(config, layer) || outdoor) return { severity: "ok", value: r.value, layerValue: r.value };
  return { severity: classify(r.value, effectiveBand(config, layer)), value: r.value, layerValue: r.value };
}

function format(role: Role, value: number, unit: "C" | "F"): string {
  switch (role) {
    case "temperature":
      return `${value.toFixed(1)}°${unit}`;
    case "humidity":
      return `${Math.round(value)}% RH`;
    case "co2":
      return `${Math.round(value)} ppm`;
    default:
      return `${Math.round(value)}`;
  }
}

function roleForLayer(layer: LayerName): Role {
  return ({ temperature: "temperature", humidity: "humidity", soil: "moisture", air: "co2" } as const)[layer];
}

export function buildScene(planConfig: Config, rs: ReadingSet, requested: LayerName): Scene {
  // The panel's frame, as core hands it over (apply_map_rotation(), D-080): everything below
  // draws the turned rooms and pins into map_area and needs no rotation of its own.
  const config = inViewFrame(planConfig);
  const mapArea = config.canvas.map_area ?? defaultMapArea(config.canvas);
  const toPx = (p: MapPoint) => ({ x: mapArea.x + p[0] * mapArea.width, y: mapArea.y + p[1] * mapArea.height });
  const enabledLayers = (["temperature", "humidity", "soil", "air"] as const).filter((l) => layerEnabled(config, l));
  const fallback = config.layers?.default ?? "temperature";
  const layer: LayerName = layerEnabled(config, requested) ? requested : fallback;
  const unit = config.temperature_unit ?? "C";
  const roomIndex = new Map(config.rooms.map((r, i) => [r.id, i]));
  const layerRole = roleForLayer(layer);

  const pins: ScenePin[] = [];
  const pulse: (Severity | undefined)[] = config.rooms.map(() => undefined);
  const samples: { room: number; x: number; y: number; v: number }[] = [];
  const hudCandidates: (HudItem & { thirst: number; order: number })[] = [];

  config.pins.forEach((pin, index) => {
    const ri = roomIndex.get(pin.room);
    const room = ri === undefined ? undefined : config.rooms[ri];
    const outdoor = room?.outdoor === true;
    const primary = assess(config, pin, PRIMARY_ROLE[pin.kind], rs, outdoor);
    const px = toPx(pin.position);
    // On a layer the pin feeds, the label shows that layer's reading; else its primary role.
    const shownRole: Role = fedLayer(pin.kind, layerRole) === layer && pin.entities[layerRole] ? layerRole : PRIMARY_ROLE[pin.kind];
    const shown = shownRole === PRIMARY_ROLE[pin.kind] ? primary : assess(config, pin, shownRole, rs, outdoor);
    const sp: ScenePin = {
      index,
      x: px.x,
      y: px.y,
      kind: pin.kind,
      name: pin.name,
      severity: shown.severity,
      label: shown.value === undefined ? "no data" : format(shownRole, shown.value, unit),
    };
    if (pin.kind === "plant" || pin.kind === "soil") {
      if (primary.layerValue !== undefined) sp.moisture = 1 - primary.layerValue;
      sp.severity = primary.severity;
      sp.label = primary.layerValue === undefined ? "no data" : `${Math.round((1 - primary.layerValue) * 100)}%`;
    }
    pins.push(sp);

    // Active-layer assessment: heat sample and room pulse (stale never pulses).
    const a = pin.entities[layerRole] !== undefined && fedLayer(pin.kind, layerRole) === layer ? assess(config, pin, layerRole, rs, outdoor) : undefined;
    if (a && ri !== undefined) {
      if (a.layerValue !== undefined && !outdoor) samples.push({ room: ri, ...toPx(pin.position), v: a.layerValue });
      if (a.severity === "warn" || a.severity === "danger") {
        const cur = pulse[ri];
        if (cur === undefined || RANK[a.severity] > RANK[cur]) pulse[ri] = a.severity;
      }
    }

    // HUD (D-009 with R2): fresh plants, thirst above the effective comfort_max, warn/danger.
    if (pin.kind === "plant" && pin.calibration && primary.layerValue !== undefined && layerEnabled(config, "soil")) {
      const band = soilBand(effectiveBand(config, "soil"), pin.calibration);
      if (primary.layerValue > (band.comfort_max as number) && (primary.severity === "warn" || primary.severity === "danger")) {
        hudCandidates.push({
          pin: index,
          name: pin.name,
          room: room?.name ?? pin.room,
          percent: Math.round((1 - primary.layerValue) * 100),
          severity: primary.severity,
          thirst: primary.layerValue,
          order: index,
        });
      }
    }
  });

  hudCandidates.sort((a, b) => RANK[b.severity] - RANK[a.severity] || b.thirst - a.thirst || a.order - b.order);
  const max = config.hud?.max_items ?? DEFAULT_HUD_MAX_ITEMS;
  const items = hudCandidates.slice(0, max).map(({ thirst: _t, order: _o, ...h }) => h);

  // Heat: room-local IDW on the configured grid, outdoor rooms excluded (R10).
  const cols = config.heat?.grid_cols ?? DEFAULT_HEAT.grid_cols;
  const rows = config.heat?.grid_rows ?? DEFAULT_HEAT.grid_rows;
  const power = config.heat?.idw_power ?? DEFAULT_HEAT.idw_power;
  const cells: (number | null | undefined)[] = Array.from<number | null | undefined>({ length: cols * rows });
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const m: MapPoint = [(c + 0.5) / cols, (r + 0.5) / rows];
      let ri = -1;
      for (let k = 0; k < config.rooms.length; k++) {
        if (pointInPolygon((config.rooms[k] as { polygon: MapPoint[] }).polygon, m)) {
          ri = k;
          break;
        }
      }
      if (ri < 0 || config.rooms[ri]?.outdoor) {
        cells[r * cols + c] = undefined; // outside
        continue;
      }
      const p = toPx(m);
      let sw = 0;
      let sv = 0;
      let exact: number | undefined;
      for (const s of samples) {
        if (s.room !== ri) continue;
        const d = Math.hypot(s.x - p.x, s.y - p.y);
        if (d < 0.5) {
          exact = s.v;
          break;
        }
        const w = 1 / d ** power;
        sw += w;
        sv += w * s.v;
      }
      cells[r * cols + c] = exact ?? (sw > 0 ? sv / sw : null); // null: no data
    }
  }

  const rooms: SceneRoom[] = config.rooms.map((room, i) => {
    const sr: SceneRoom = { name: room.name, outdoor: room.outdoor === true, points: room.polygon.map(toPx) };
    const p = pulse[i];
    if (p === "warn" || p === "danger") sr.pulse = p;
    return sr;
  });

  return {
    mapArea,
    canvas: { width: config.canvas.width, height: config.canvas.height },
    layer,
    enabledLayers,
    rooms,
    pins,
    heat: { cols, rows, cells, band: effectiveBand(config, layer) },
    hud: { items, overflow: hudCandidates.length - items.length, total: hudCandidates.length },
  };
}

/** Diverging comfort-quiet colour (UI spec): neutral in band, to blue below, to red above. */
export function heatColour(value: number, band: Band): [number, number, number] {
  const neutral: [number, number, number] = [30, 35, 41];
  const cold: [number, number, number] = [42, 120, 214];
  const hot: [number, number, number] = [227, 73, 72];
  const lerp = (a: number[], b: number[], t: number) =>
    a.map((x, i) => Math.round(x + ((b[i] as number) - x) * Math.min(1, Math.max(0, t)))) as [number, number, number];
  const cmin = band.comfort_min as number;
  const cmax = band.comfort_max as number;
  if (value < cmin) return cmin <= (band.scale_min as number) ? neutral : lerp(neutral, cold, (cmin - value) / (cmin - (band.scale_min as number)));
  if (value > cmax) return (band.scale_max as number) <= cmax ? neutral : lerp(neutral, hot, (value - cmax) / ((band.scale_max as number) - cmax));
  return neutral;
}

/**
 * Deterministic sample readings so a fresh project previews something: every pin's entities
 * read a value that varies by pin, all fresh. Plants alternate between thirsty and fine.
 */
export function sampleReadings(config: Config, now = Date.UTC(2026, 0, 15, 9)): ReadingSet {
  const readings = new Map<string, Reading>();
  config.pins.forEach((pin, i) => {
    const wobble = ((i * 37) % 11) / 10; // 0..1, stable per pin
    for (const [role, id] of Object.entries(pin.entities) as [Role, string][]) {
      let value: number;
      switch (role) {
        case "temperature":
          value = 19.5 + wobble * 5;
          break;
        case "humidity":
          value = 42 + wobble * 22;
          break;
        case "co2":
          value = 520 + wobble * 900;
          break;
        case "moisture": {
          const cal = pin.calibration ?? { dry: 0, wet: 100 };
          const frac = i % 3 === 0 ? 0.05 + wobble * 0.1 : 0.45 + wobble * 0.3;
          value = Math.round(cal.dry + frac * (cal.wet - cal.dry));
          break;
        }
        case "battery":
          value = 60 + wobble * 40;
          break;
        default:
          value = Math.round(wobble * 100);
      }
      readings.set(id, { value, lastUpdated: now - 5 * 60_000 });
    }
  });
  return { now, readings };
}

/** Parse a readings fixture (schema/readings.schema.json) into a ReadingSet. */
export function parseReadings(text: string): ReadingSet {
  let data: unknown;
  try {
    data = JSON.parse(text);
  } catch {
    throw new Error("not valid JSON");
  }
  const d = data as { now?: unknown; readings?: unknown };
  const now = typeof d.now === "string" ? Date.parse(d.now) : NaN;
  if (!Number.isFinite(now) || typeof d.readings !== "object" || d.readings === null) {
    throw new Error('expected a readings fixture: {"now": "...", "readings": {...}}');
  }
  const readings = new Map<string, Reading>();
  for (const [id, raw] of Object.entries(d.readings as Record<string, unknown>)) {
    const r = raw as { value?: unknown; last_updated?: unknown };
    const lu = typeof r.last_updated === "string" ? Date.parse(r.last_updated) : NaN;
    const value = typeof r.value === "number" ? r.value : r.value === "unavailable" || r.value === "unknown" ? r.value : undefined;
    if (value === undefined || !Number.isFinite(lu)) throw new Error(`${id}: needs a value and last_updated`);
    readings.set(id, { value, lastUpdated: lu });
  }
  return { now, readings };
}
