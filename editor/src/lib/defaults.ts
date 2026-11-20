// Schema defaults the editor shows next to empty fields. These are copies; the originals are
// the schema's "default" keywords and core's default_band() (core/include/hearthmap/config.h).
// test/defaults.test.ts reads both and fails if a copy here drifts.

import type { BandField, CanvasRect, Kind, LayerName, Role } from "./types";

export type Band = Record<BandField, number | null>;

export const DEFAULT_BANDS: Readonly<Record<LayerName, Readonly<Band>>> = {
  temperature: {
    comfort_min: 19,
    comfort_max: 24,
    warn_below: 16,
    warn_above: 27,
    danger_below: 12,
    danger_above: 32,
    scale_min: 10,
    scale_max: 34,
  },
  humidity: {
    comfort_min: 40,
    comfort_max: 60,
    warn_below: 30,
    warn_above: 65,
    danger_below: 20,
    danger_above: 75,
    scale_min: 15,
    scale_max: 90,
  },
  soil: {
    comfort_min: 0.1,
    comfort_max: 0.6,
    warn_below: null,
    warn_above: 0.7,
    danger_below: null,
    danger_above: 0.9,
    scale_min: 0,
    scale_max: 1,
  },
  air: {
    comfort_min: 400,
    comfort_max: 800,
    warn_below: null,
    warn_above: 1200,
    danger_below: null,
    danger_above: 2000,
    scale_min: 350,
    scale_max: 2500,
  },
};

export const DEFAULT_STALE_AFTER_S = 10800;
export const DEFAULT_HUD_MAX_ITEMS = 6;
export const DEFAULT_HEAT = { grid_cols: 48, grid_rows: 40, idw_power: 2 } as const;
/**
 * The map area a config gets when it omits canvas.map_area: the whole canvas (ruling R14).
 * The editor's new projects set it explicitly to PANEL_MAP_AREA, leaving room for the HUD.
 */
export function defaultMapArea(canvas: { width: number; height: number }): CanvasRect {
  return { x: 0, y: 0, width: canvas.width, height: canvas.height };
}

/** The map area of the panel layout in the UI spec: left of the HUD column on 1280x800. */
export const PANEL_MAP_AREA: Readonly<CanvasRect> = { x: 0, y: 0, width: 960, height: 800 };

/** Schema maxItems (ruling R3). */
export const MAX_ROOMS = 64;
export const MAX_PINS = 256;
export const MAX_VERTICES = 64;

/** Kinds that may bind the moisture role (ruling R1). */
export function allowsMoisture(kind: Kind): boolean {
  return kind === "soil" || kind === "plant";
}
export const DEFAULT_LAYER: LayerName = "temperature";
export const DEFAULT_CANVAS = { width: 1280, height: 800, orientation: "landscape" } as const;

/** The role each kind must bind (core's primary_role()). */
export const PRIMARY_ROLE: Readonly<Record<Kind, Role>> = {
  temperature: "temperature",
  humidity: "humidity",
  soil: "moisture",
  air_quality: "co2",
  plant: "moisture",
};

/** Kinds that need a calibration (schema: required for soil and plant, forbidden otherwise). */
export function needsCalibration(kind: Kind): boolean {
  return kind === "soil" || kind === "plant";
}

/** Only plant pins may carry a photo. */
export function allowsPhoto(kind: Kind): boolean {
  return kind === "plant";
}

/** Units for display, by layer. Temperature follows the config's temperature_unit. */
export const LAYER_UNITS: Readonly<Record<LayerName, string>> = {
  temperature: "°",
  humidity: "% RH",
  soil: "thirst 0..1",
  air: "ppm CO2",
};

/** The calibration a new soil or plant pin starts with: a 0..100 percentage sensor. */
export const NEW_CALIBRATION = { dry: 0, wet: 100 } as const;

/**
 * Side of the square plant photo in the export, in pixels. The panel draws the photo at
 * diameter 44 on the map, 52 in the HUD and 128 in the detail overlay (UI spec), so 128 is
 * the largest size it is ever shown at; the ESPHome component converts it to RGB565.
 */
export const PHOTO_SIZE_PX = 128;

/** Where exported configs point "$schema" when the project did not already set it. */
export const SCHEMA_URL =
  "https://raw.githubusercontent.com/abradner/hearthmap/main/schema/hearthmap.schema.json";
