// TypeScript mirror of schema/hearthmap.schema.json. The schema is the contract; these types
// only describe it for the compiler. Optional fields stay optional: the editor never fills a
// schema default into the exported config (defaults.ts shows them in the UI instead), so a
// config round-trips without growing.

export type MapPoint = [number, number];

export const KINDS = ["temperature", "humidity", "soil", "air_quality", "plant"] as const;
export type Kind = (typeof KINDS)[number];

export const ROLES = [
  "temperature",
  "humidity",
  "moisture",
  "co2",
  "pm25",
  "voc",
  "battery",
  "predicted_dry_at",
] as const;
export type Role = (typeof ROLES)[number];

export const LAYERS = ["temperature", "humidity", "soil", "air"] as const;
export type LayerName = (typeof LAYERS)[number];

export const BAND_FIELDS = [
  "comfort_min",
  "comfort_max",
  "warn_below",
  "warn_above",
  "danger_below",
  "danger_above",
  "scale_min",
  "scale_max",
] as const;
export type BandField = (typeof BAND_FIELDS)[number];

/** Threshold fields may be null in the config, meaning "no threshold on that side". */
export const THRESHOLD_FIELDS: readonly BandField[] = [
  "warn_below",
  "warn_above",
  "danger_below",
  "danger_above",
];

export interface CanvasRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface Canvas {
  width: number;
  height: number;
  orientation: "landscape" | "portrait";
  map_area?: CanvasRect;
}

export interface Room {
  id: string;
  name: string;
  polygon: MapPoint[];
  outdoor?: boolean;
}

export type Entities = Partial<Record<Role, string>>;

export interface Calibration {
  dry: number;
  wet: number;
  happy_min?: number;
  happy_max?: number;
}

export interface Pin {
  id: string;
  kind: Kind;
  name: string;
  position: MapPoint;
  room: string;
  entities: Entities;
  photo?: string;
  calibration?: Calibration;
  stale_after_s?: number;
}

export interface LayerBand {
  enabled?: boolean;
  comfort_min?: number;
  comfort_max?: number;
  warn_below?: number | null;
  warn_above?: number | null;
  danger_below?: number | null;
  danger_above?: number | null;
  scale_min?: number;
  scale_max?: number;
}

export interface Layers {
  default?: LayerName;
  temperature?: LayerBand;
  humidity?: LayerBand;
  soil?: LayerBand;
  air?: LayerBand;
}

export interface Config {
  $schema?: string;
  version: 1;
  canvas: Canvas;
  temperature_unit?: "C" | "F";
  rooms: Room[];
  pins: Pin[];
  layers?: Layers;
  stale_after_s?: number;
  hud?: { max_items?: number };
  heat?: { grid_cols?: number; grid_rows?: number; idw_power?: number };
}

/** Photo bytes by photo id. Always PNG, already square and sized for the device (photos.ts). */
export type PhotoStore = ReadonlyMap<string, Uint8Array>;
