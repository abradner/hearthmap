// Home Assistant entity lists: parsing a states export, filtering, and finding entities bound
// more than once. The list is only a picker's source; the config stores bare entity ids.

import { ENTITY_ID_PATTERN } from "./ids";
import { ROLES, type Config, type Role } from "./types";

export interface EntityInfo {
  entity_id: string;
  name?: string;
  device_class?: string;
  unit?: string;
  state?: string;
}

/** Device classes HA uses for each role; the first is the picker's default filter. */
export const ROLE_DEVICE_CLASSES: Readonly<Record<Role, readonly string[]>> = {
  temperature: ["temperature"],
  humidity: ["humidity"],
  moisture: ["moisture"],
  co2: ["carbon_dioxide"],
  pm25: ["pm25"],
  voc: ["volatile_organic_compounds", "volatile_organic_compounds_parts"],
  battery: ["battery"],
  predicted_dry_at: ["timestamp"],
};

export interface ParseResult {
  entities: EntityInfo[];
  /** Entries dropped because they had no usable entity id. */
  skipped: number;
}

function str(v: unknown): string | undefined {
  return typeof v === "string" && v.length > 0 ? v : undefined;
}

function toInfo(raw: unknown): EntityInfo | undefined {
  if (typeof raw !== "object" || raw === null) return undefined;
  const r = raw as Record<string, unknown>;
  const id = str(r.entity_id);
  if (!id || !ENTITY_ID_PATTERN.test(id)) return undefined;
  const a = (typeof r.attributes === "object" && r.attributes !== null ? r.attributes : {}) as Record<string, unknown>;
  const info: EntityInfo = { entity_id: id };
  const name = str(a.friendly_name);
  const dc = str(a.device_class);
  const unit = str(a.unit_of_measurement);
  const state = typeof r.state === "string" || typeof r.state === "number" || typeof r.state === "boolean" ? String(r.state) : undefined;
  if (name) info.name = name;
  if (dc) info.device_class = dc;
  if (unit) info.unit = unit;
  if (state !== undefined) info.state = state;
  return info;
}

/**
 * Parse a Home Assistant states export: the JSON array `GET /api/states` returns, or an object
 * wrapping it as `{"states": [...]}`. Entries without a valid entity id are skipped and
 * counted; duplicates keep the first. Throws on text that is not JSON or not one of the two
 * shapes, with a message for the user.
 */
export function parseStatesExport(text: string): ParseResult {
  let data: unknown;
  try {
    data = JSON.parse(text);
  } catch {
    throw new Error("not valid JSON");
  }
  const list = Array.isArray(data)
    ? data
    : typeof data === "object" && data !== null && Array.isArray((data as { states?: unknown }).states)
      ? ((data as { states: unknown[] }).states)
      : undefined;
  if (!list) throw new Error('expected a JSON array of states (as /api/states returns) or {"states": [...]}');
  return fromStates(list);
}

export function fromStates(list: readonly unknown[]): ParseResult {
  const seen = new Set<string>();
  const entities: EntityInfo[] = [];
  let skipped = 0;
  for (const raw of list) {
    const info = toInfo(raw);
    if (!info) {
      skipped++;
      continue;
    }
    if (seen.has(info.entity_id)) continue;
    seen.add(info.entity_id);
    entities.push(info);
  }
  entities.sort((a, b) => a.entity_id.localeCompare(b.entity_id));
  return { entities, skipped };
}

export interface EntityFilter {
  /** A device class, "" for "any", or "none" for entities without one. */
  deviceClass: string;
  search: string;
}

export function filterEntities(list: readonly EntityInfo[], f: EntityFilter): EntityInfo[] {
  const terms = f.search.toLowerCase().split(/\s+/).filter((t) => t.length > 0);
  return list.filter((e) => {
    if (f.deviceClass === "none" ? e.device_class !== undefined : f.deviceClass !== "" && e.device_class !== f.deviceClass) {
      return false;
    }
    const hay = `${e.entity_id} ${e.name ?? ""}`.toLowerCase();
    return terms.every((t) => hay.includes(t));
  });
}

/** Device classes present in a list, sorted, for the filter menu. */
export function deviceClassesIn(list: readonly EntityInfo[]): string[] {
  return [...new Set(list.map((e) => e.device_class).filter((d): d is string => d !== undefined))].sort();
}

export interface BindingUse {
  pin: number;
  role: Role;
}

/** Entities bound in more than one place (across pins or roles), with every place. */
export function findDuplicateBindings(config: Pick<Config, "pins">): Map<string, BindingUse[]> {
  const uses = new Map<string, BindingUse[]>();
  config.pins.forEach((p, pin) => {
    for (const role of ROLES) {
      const id = p.entities[role];
      if (id === undefined) continue;
      const list = uses.get(id) ?? [];
      list.push({ pin, role });
      uses.set(id, list);
    }
  });
  for (const [id, list] of uses) if (list.length < 2) uses.delete(id);
  return uses;
}
