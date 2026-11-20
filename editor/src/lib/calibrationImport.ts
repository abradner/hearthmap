// Import of calibration proposals written by tools/calibrate, in the interchange format that
// tool defines (schema/calibration.schema.json; example: tools/calibrate/examples/). A sensor
// with status "ok" carries a calibration with all four fields; any other status is a sensor
// the tool could not calibrate, with a message, and is skipped.

import { Ajv2020 } from "ajv/dist/2020.js";
import calibrationSchema from "../../../schema/calibration.schema.json";
import configSchema from "../../../schema/hearthmap.schema.json";
import type { Calibration, Config } from "./types";

export type Confidence = "high" | "medium" | "low" | "none";

export interface SensorProposal {
  entity_id: string;
  status: string;
  confidence: Confidence;
  message?: string;
  calibration?: Required<Calibration>;
  happy_is_default?: boolean;
  warnings: { code: string; message: string }[];
}

export interface CalibrationFile {
  generator: string;
  generated_at: string;
  sensors: SensorProposal[];
}

// The calibration schema $refs the config schema's calibration definition by relative URL, so
// both are registered. Formats ("date-time") are not checked: the date is informational.
const ajv = new Ajv2020({ allErrors: true, strict: false, validateFormats: false });
ajv.addSchema(configSchema);
const validateFile = ajv.compile(calibrationSchema);

/** Parse and schema-check a calibration file. Throws a user-facing Error when it is not one. */
export function parseCalibrationFile(text: string): CalibrationFile {
  let data: unknown;
  try {
    data = JSON.parse(text);
  } catch {
    throw new Error("not valid JSON");
  }
  if (!validateFile(data)) {
    const first = validateFile.errors?.find((e) => e.keyword !== "if") ?? validateFile.errors?.[0];
    throw new Error(`not a calibration file: ${first?.instancePath || "/"} ${first?.message ?? ""}`.trim());
  }
  return data as unknown as CalibrationFile;
}

export interface SensorOutcome {
  entity_id: string;
  confidence: Confidence;
  /** applied: copied into these pins; skipped: the tool could not calibrate; unmatched: no pin binds it. */
  result: "applied" | "skipped" | "unmatched" | "invalid";
  pins: string[];
  message?: string;
  happy_is_default?: boolean;
  warnings: string[];
}

export interface ApplyResult {
  config: Config;
  outcomes: SensorOutcome[];
}

/**
 * Copy each usable proposal into every soil/plant pin whose moisture entity matches, replacing
 * the pin's calibration as a whole. Sensors the tool could not calibrate are skipped with
 * their message; a zero-width range is refused (core would reject it). Pure.
 */
export function applyCalibrations(config: Config, file: Pick<CalibrationFile, "sensors">): ApplyResult {
  const outcomes: SensorOutcome[] = [];
  const usable = new Map<string, Required<Calibration>>();
  for (const s of file.sensors) {
    const base = { entity_id: s.entity_id, confidence: s.confidence, pins: [] as string[], warnings: s.warnings.map((w) => w.message) };
    if (s.status !== "ok" || !s.calibration) {
      outcomes.push({ ...base, result: "skipped", message: s.message ?? s.status });
      continue;
    }
    if (Math.abs(s.calibration.wet - s.calibration.dry) < 1e-9) {
      outcomes.push({ ...base, result: "invalid", message: "dry and wet are equal" });
      continue;
    }
    usable.set(s.entity_id, s.calibration);
    const outcome: SensorOutcome = { ...base, result: "unmatched" };
    if (s.happy_is_default !== undefined) outcome.happy_is_default = s.happy_is_default;
    outcomes.push(outcome);
  }
  const byEntity = new Map(outcomes.map((o) => [o.entity_id, o]));
  const pins = config.pins.map((pin) => {
    if (pin.kind !== "soil" && pin.kind !== "plant") return pin;
    const m = pin.entities.moisture;
    const cal = m === undefined ? undefined : usable.get(m);
    if (!cal || m === undefined) return pin;
    const o = byEntity.get(m) as SensorOutcome;
    o.result = "applied";
    o.pins.push(pin.id);
    const calibration: Calibration = { dry: cal.dry, wet: cal.wet, happy_min: cal.happy_min, happy_max: cal.happy_max };
    return { ...pin, calibration };
  });
  return { config: { ...config, pins }, outcomes };
}
