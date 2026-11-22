// Edits to a config. Every function is pure: it returns a new Config and leaves its input
// untouched, which is what makes undo/redo a list of snapshots (history.ts). Coordinates are
// rounded to the stored precision when an edit writes them.

import {
  NEW_CALIBRATION,
  PRIMARY_ROLE,
  allowsMoisture,
  allowsPhoto,
  needsCalibration,
  DEFAULT_CANVAS,
  defaultMapArea,
  MAX_PINS,
  MAX_ROOMS,
  MAX_VERTICES,
  PANEL_MAP_AREA,
  SCHEMA_URL,
} from "./defaults";
import { roomIndexAt, roundCoord, clamp01 } from "./geometry";
import { mapRotationOf, swapsAxes } from "./rotation";
import { slugId, slugPhotoId, uniqueId } from "./ids";
import type { BandField, Calibration, Config, Kind, LayerBand, LayerName, MapPoint, MapRotation, Pin, Role, Room } from "./types";

const round = (p: MapPoint): MapPoint => [roundCoord(clamp01(p[0])), roundCoord(clamp01(p[1]))];

export function blankConfig(): Config {
  return {
    $schema: SCHEMA_URL,
    version: 1,
    canvas: { ...DEFAULT_CANVAS, map_area: { ...PANEL_MAP_AREA } },
    rooms: [],
    pins: [],
  };
}

function setAt<T>(list: readonly T[], i: number, value: T): T[] {
  const out = list.slice();
  out[i] = value;
  return out;
}

function room(config: Config, i: number): Room {
  const r = config.rooms[i];
  if (!r) throw new RangeError(`no room ${i}`);
  return r;
}

function pin(config: Config, i: number): Pin {
  const p = config.pins[i];
  if (!p) throw new RangeError(`no pin ${i}`);
  return p;
}

// ---- Rooms ------------------------------------------------------------------------------------

export function addRoom(config: Config, polygon: readonly MapPoint[], name?: string): Config {
  if (config.rooms.length >= MAX_ROOMS) throw new Error(`a config holds at most ${MAX_ROOMS} rooms`);
  const n = config.rooms.length + 1;
  const display = name?.trim() || `Room ${n}`;
  const id = uniqueId(slugId(display, "room"), config.rooms.map((r) => r.id));
  return { ...config, rooms: [...config.rooms, { id, name: display, polygon: polygon.map(round) }] };
}

/**
 * Rename a room. The id follows the name only while it still matches the name's slug (so a
 * hand-set id is kept), and pins pointing at the old id follow it.
 */
export function renameRoom(config: Config, i: number, name: string): Config {
  const r = room(config, i);
  const others = config.rooms.filter((_, k) => k !== i).map((x) => x.id);
  const autoId = uniqueId(slugId(r.name, "room"), others) === r.id;
  const id = autoId ? uniqueId(slugId(name, "room"), others) : r.id;
  const rooms = setAt(config.rooms, i, { ...r, name, id });
  const pins = id === r.id ? config.pins : config.pins.map((p) => (p.room === r.id ? { ...p, room: id } : p));
  return { ...config, rooms, pins };
}

/** Set a room id explicitly; pins follow. The caller checks the pattern and uniqueness. */
export function setRoomId(config: Config, i: number, id: string): Config {
  const r = room(config, i);
  return {
    ...config,
    rooms: setAt(config.rooms, i, { ...r, id }),
    pins: config.pins.map((p) => (p.room === r.id ? { ...p, room: id } : p)),
  };
}

export function setOutdoor(config: Config, i: number, outdoor: boolean): Config {
  const { outdoor: _drop, ...rest } = room(config, i);
  return { ...config, rooms: setAt(config.rooms, i, outdoor ? { ...rest, outdoor: true } : rest) };
}

/**
 * Delete a room. Pins that were in it are re-homed into whichever remaining room contains
 * them; a pin no room contains keeps the dangling id, which validation then reports next to it.
 */
export function deleteRoom(config: Config, i: number): Config {
  const gone = room(config, i);
  const rooms = config.rooms.filter((_, k) => k !== i);
  const pins = config.pins.map((p) => {
    if (p.room !== gone.id) return p;
    const k = roomIndexAt(rooms, p.position);
    return k >= 0 ? { ...p, room: (rooms[k] as Room).id } : p;
  });
  return { ...config, rooms, pins };
}

export function moveVertex(config: Config, i: number, v: number, to: MapPoint): Config {
  const r = room(config, i);
  if (v < 0 || v >= r.polygon.length) throw new RangeError(`no vertex ${v}`);
  return { ...config, rooms: setAt(config.rooms, i, { ...r, polygon: setAt(r.polygon, v, round(to)) }) };
}

/** Insert a vertex before index `at` (so `at = edge + 1` splits edge `edge`). Max 64. */
export function insertVertex(config: Config, i: number, at: number, p: MapPoint): Config {
  const r = room(config, i);
  if (r.polygon.length >= MAX_VERTICES) return config;
  const polygon = [...r.polygon.slice(0, at), round(p), ...r.polygon.slice(at)];
  return { ...config, rooms: setAt(config.rooms, i, { ...r, polygon }) };
}

/** Delete a vertex; a room keeps at least three, so deleting from a triangle does nothing. */
export function deleteVertex(config: Config, i: number, v: number): Config {
  const r = room(config, i);
  if (r.polygon.length <= 3) return config;
  return { ...config, rooms: setAt(config.rooms, i, { ...r, polygon: r.polygon.filter((_, k) => k !== v) }) };
}

/** Move a room up or down the list; earlier rooms own overlaps. */
export function reorderRoom(config: Config, i: number, delta: -1 | 1): Config {
  const j = i + delta;
  if (j < 0 || j >= config.rooms.length) return config;
  const rooms = config.rooms.slice();
  [rooms[i], rooms[j]] = [rooms[j] as Room, rooms[i] as Room];
  return { ...config, rooms };
}

// ---- Pins -------------------------------------------------------------------------------------

const KIND_NAMES: Record<Kind, string> = {
  temperature: "Temperature",
  humidity: "Humidity",
  soil: "Soil probe",
  air_quality: "Air quality",
  plant: "Plant",
};

/** The room a position suggests: the first room containing it, or undefined. */
export function suggestRoom(config: Config, position: MapPoint): string | undefined {
  const k = roomIndexAt(config.rooms, position);
  return k >= 0 ? (config.rooms[k] as Room).id : undefined;
}

/**
 * Drop a pin. Its room is the one under it, or the first room when it lands outside all of
 * them (validation then warns that it is outside its room). Needs at least one room.
 */
export function addPin(config: Config, kind: Kind, position: MapPoint): Config {
  const first = config.rooms[0];
  if (!first) throw new Error("draw a room before dropping pins");
  if (config.pins.length >= MAX_PINS) throw new Error(`a config holds at most ${MAX_PINS} pins`);
  const pos = round(position);
  const roomId = suggestRoom(config, pos) ?? first.id;
  const roomName = config.rooms.find((r) => r.id === roomId)?.name ?? "";
  const name = kind === "plant" ? "Plant" : `${roomName} ${KIND_NAMES[kind].toLowerCase()}`.trim();
  const id = uniqueId(slugId(`${kind === "plant" ? "plant" : kind} ${roomId}`, "pin"), config.pins.map((p) => p.id));
  const p: Pin = { id, kind, name, position: pos, room: roomId, entities: {} };
  if (needsCalibration(kind)) p.calibration = { ...NEW_CALIBRATION };
  return { ...config, pins: [...config.pins, p] };
}

/**
 * Move a pin. When the pin leaves its room for another, the room follows (the suggestion is
 * automatic); when it lands outside every room the room is left as it was.
 */
export function movePin(config: Config, i: number, to: MapPoint): Config {
  const p = pin(config, i);
  const position = round(to);
  const suggested = suggestRoom(config, position);
  return { ...config, pins: setAt(config.pins, i, { ...p, position, room: suggested ?? p.room }) };
}

export function updatePin(config: Config, i: number, patch: Partial<Pick<Pin, "name" | "room" | "id">>): Config {
  return { ...config, pins: setAt(config.pins, i, { ...pin(config, i), ...patch }) };
}

/** Change kind, adding a calibration where the new kind needs one and dropping fields it forbids. */
export function setPinKind(config: Config, i: number, kind: Kind): Config {
  const { calibration, photo, ...rest } = pin(config, i);
  const next: Pin = { ...rest, kind };
  if (!allowsMoisture(kind) && next.entities.moisture !== undefined) {
    // Ruling R1: moisture binds only on soil and plant pins. Undo brings it back.
    const { moisture: _m, ...entities } = next.entities;
    next.entities = entities;
  }
  if (photo !== undefined && allowsPhoto(kind)) next.photo = photo;
  if (needsCalibration(kind)) next.calibration = calibration ?? { ...NEW_CALIBRATION };
  return { ...config, pins: setAt(config.pins, i, next) };
}

/** Bind or (with undefined/empty) unbind an entity for a role. */
export function bindEntity(config: Config, i: number, role: Role, entityId: string | undefined): Config {
  const p = pin(config, i);
  if (role === "moisture" && entityId && !allowsMoisture(p.kind)) throw new Error("only soil and plant pins can bind moisture");
  const entities = { ...p.entities };
  if (entityId === undefined || entityId.trim() === "") delete entities[role];
  else entities[role] = entityId.trim();
  return { ...config, pins: setAt(config.pins, i, { ...p, entities }) };
}

export function setCalibration(config: Config, i: number, calibration: Calibration): Config {
  const c: Calibration = { dry: calibration.dry, wet: calibration.wet };
  if (calibration.happy_min !== undefined) c.happy_min = calibration.happy_min;
  if (calibration.happy_max !== undefined) c.happy_max = calibration.happy_max;
  return { ...config, pins: setAt(config.pins, i, { ...pin(config, i), calibration: c }) };
}

export function setPinStaleAfter(config: Config, i: number, seconds: number | undefined): Config {
  const { stale_after_s: _drop, ...rest } = pin(config, i);
  return { ...config, pins: setAt(config.pins, i, seconds === undefined ? rest : { ...rest, stale_after_s: seconds }) };
}

/** Attach (or with undefined, detach) a photo id. Only plant pins may hold one. */
export function setPinPhoto(config: Config, i: number, photoId: string | undefined): Config {
  const { photo: _drop, ...rest } = pin(config, i);
  if (photoId !== undefined && !allowsPhoto(rest.kind)) throw new Error("only plant pins can have a photo");
  return { ...config, pins: setAt(config.pins, i, photoId === undefined ? rest : { ...rest, photo: photoId }) };
}

/** A photo id for a pin, unique among the config's other pins' photos. */
export function photoIdFor(config: Config, i: number): string {
  const p = pin(config, i);
  const taken = config.pins.filter((_, k) => k !== i).map((x) => x.photo).filter((x): x is string => x !== undefined);
  return uniqueId(slugPhotoId(p.name, p.id.replace(/_/g, "-")), taken, "-");
}

export function deletePin(config: Config, i: number): Config {
  pin(config, i);
  return { ...config, pins: config.pins.filter((_, k) => k !== i) };
}

export function primaryRole(kind: Kind): Role {
  return PRIMARY_ROLE[kind];
}

// ---- Layers and project settings --------------------------------------------------------------

/**
 * Set one layer field. `undefined` removes it (the default applies); `null` is only meaningful
 * for thresholds and means "no threshold on this side". An emptied layer object is removed,
 * and so is an emptied `layers`.
 */
export function setLayerField(
  config: Config,
  layer: LayerName,
  field: BandField | "enabled",
  value: number | boolean | null | undefined,
): Config {
  const layers = { ...(config.layers ?? {}) };
  const band: LayerBand = { ...(layers[layer] ?? {}) };
  if (value === undefined) delete band[field];
  else (band as Record<string, unknown>)[field] = value;
  if (Object.keys(band).length) layers[layer] = band;
  else delete layers[layer];
  return withLayers(config, layers);
}

export function setDefaultLayer(config: Config, layer: LayerName | undefined): Config {
  const layers = { ...(config.layers ?? {}) };
  if (layer === undefined) delete layers.default;
  else layers.default = layer;
  return withLayers(config, layers);
}

function withLayers(config: Config, layers: NonNullable<Config["layers"]>): Config {
  const { layers: _drop, ...rest } = config;
  return Object.keys(layers).length ? { ...rest, layers } : rest;
}

export function setStaleAfter(config: Config, seconds: number | undefined): Config {
  const { stale_after_s: _drop, ...rest } = config;
  return seconds === undefined ? rest : { ...rest, stale_after_s: seconds };
}

export function setHudMax(config: Config, max: number | undefined): Config {
  const { hud: _drop, ...rest } = config;
  return max === undefined ? rest : { ...rest, hud: { ...(config.hud ?? {}), max_items: max } };
}

export function setTemperatureUnit(config: Config, unit: "C" | "F" | undefined): Config {
  const { temperature_unit: _drop, ...rest } = config;
  return unit === undefined ? rest : { ...rest, temperature_unit: unit };
}

/**
 * Size the map area to a floor plan's proportions, as large as fits in the space the panel
 * leaves for the map (the default map area) and inside the canvas. Keeps the area anchored at
 * the default origin.
 */
export function fitMapAreaToAspect(config: Config, imageWidth: number, imageHeight: number): Config {
  if (!(imageWidth > 0 && imageHeight > 0)) return config;
  // The panel's region, cut to the canvas, so the area always lies inside it (core's
  // validate() refuses one that does not). A canvas too small for the region's origin uses its
  // own corner.
  const { width: cw, height: ch } = config.canvas;
  const fits = PANEL_MAP_AREA.x < cw && PANEL_MAP_AREA.y < ch;
  const x = fits ? PANEL_MAP_AREA.x : 0;
  const y = fits ? PANEL_MAP_AREA.y : 0;
  const maxW = fits ? Math.min(PANEL_MAP_AREA.width, cw - x) : cw;
  const maxH = fits ? Math.min(PANEL_MAP_AREA.height, ch - y) : ch;
  const aspect = imageWidth / imageHeight;
  let width = maxW;
  let height = Math.round(maxW / aspect);
  if (height > maxH) {
    height = maxH;
    width = Math.round(maxH * aspect);
  }
  width = Math.min(maxW, Math.max(1, width));
  height = Math.min(maxH, Math.max(1, height));
  return { ...config, canvas: { ...config.canvas, map_area: { x, y, width, height } } };
}


/**
 * Match plan shape, for a plan turned by canvas.map_rotation: `planWidth` x `planHeight` are the
 * floor plan's own proportions (its image), swapped on a quarter turn, because map_area is the
 * on-screen rectangle (schema/README.md, Coordinates).
 */
export function fitMapAreaToPlan(config: Config, planWidth: number, planHeight: number): Config {
  return swapsAxes(mapRotationOf(config))
    ? fitMapAreaToAspect(config, planHeight, planWidth)
    : fitMapAreaToAspect(config, planWidth, planHeight);
}

/**
 * Turn the whole plan a quarter turn right (90) or left (-90) on the panel (D-080): sets
 * canvas.map_rotation (removed again at 0, the default) and fits map_area to the turned
 * proportions, from the floor plan image when there is one, else by swapping the current map
 * area's. Rooms and pins are not touched: they stay in the plan's frame. One edit, one undo step.
 */
export function rotatePlan(config: Config, delta: 90 | -90, plan?: { width: number; height: number }): Config {
  const next = ((((mapRotationOf(config) + delta) % 360) + 360) % 360) as MapRotation;
  const { map_rotation: _drop, ...canvas } = config.canvas;
  const turned: Config = { ...config, canvas: next === 0 ? canvas : { ...canvas, map_rotation: next } };
  if (plan) return fitMapAreaToPlan(turned, plan.width, plan.height);
  const area = config.canvas.map_area ?? defaultMapArea(config.canvas);
  return fitMapAreaToAspect(turned, area.height, area.width);
}
