import { describe, expect, it } from "vitest";
import { deviceClassesIn, filterEntities, findDuplicateBindings, parseStatesExport } from "../src/lib/entities";
import { example } from "./helpers";

const states = [
  { entity_id: "sensor.example_living_temperature", state: "21.4", attributes: { device_class: "temperature", unit_of_measurement: "°C", friendly_name: "Living temperature" } },
  { entity_id: "sensor.example_basil_moisture", state: "0", attributes: { device_class: "moisture", friendly_name: "Basil soil" } },
  { entity_id: "sensor.example_living_co2", state: 640, attributes: { device_class: "carbon_dioxide" } },
  { entity_id: "light.example_lamp", state: "on", attributes: {} },
  { entity_id: "Not An Id", state: "1" },
  { state: "x" },
  "junk",
  { entity_id: "sensor.example_basil_moisture", state: "99" },
];

describe("parseStatesExport", () => {
  it("parses /api/states output, skipping unusable entries and keeping the first duplicate", () => {
    const { entities, skipped } = parseStatesExport(JSON.stringify(states));
    expect(skipped).toBe(3);
    expect(entities.map((e) => e.entity_id)).toEqual([
      "light.example_lamp",
      "sensor.example_basil_moisture",
      "sensor.example_living_co2",
      "sensor.example_living_temperature",
    ]);
    expect(entities[1]).toEqual({ entity_id: "sensor.example_basil_moisture", name: "Basil soil", device_class: "moisture", state: "0" });
    expect(entities[2]?.state).toBe("640");
  });
  it("accepts {states: [...]}", () => {
    expect(parseStatesExport(JSON.stringify({ states })).entities).toHaveLength(4);
  });
  it("refuses non-JSON and other shapes with a readable message", () => {
    expect(() => parseStatesExport("nope")).toThrow("not valid JSON");
    expect(() => parseStatesExport('{"a": 1}')).toThrow(/JSON array/);
  });
});

describe("filterEntities", () => {
  const { entities } = parseStatesExport(JSON.stringify(states));
  it("filters by device class, 'any' and 'none'", () => {
    expect(filterEntities(entities, { deviceClass: "moisture", search: "" }).map((e) => e.entity_id)).toEqual(["sensor.example_basil_moisture"]);
    expect(filterEntities(entities, { deviceClass: "", search: "" })).toHaveLength(4);
    expect(filterEntities(entities, { deviceClass: "none", search: "" }).map((e) => e.entity_id)).toEqual(["light.example_lamp"]);
  });
  it("searches id and friendly name, every term must match, case-insensitive", () => {
    expect(filterEntities(entities, { deviceClass: "", search: "LIVING temp" }).map((e) => e.entity_id)).toEqual(["sensor.example_living_temperature"]);
    expect(filterEntities(entities, { deviceClass: "", search: "basil soil" })).toHaveLength(1);
    expect(filterEntities(entities, { deviceClass: "", search: "basil lamp" })).toHaveLength(0);
  });
  it("lists device classes present", () => {
    expect(deviceClassesIn(entities)).toEqual(["carbon_dioxide", "moisture", "temperature"]);
  });
});

describe("findDuplicateBindings", () => {
  it("finds none in the example", () => {
    expect(findDuplicateBindings(example()).size).toBe(0);
  });
  it("finds an entity bound on two pins and on two roles of one pin", () => {
    const c = example();
    c.pins[1]!.entities.battery = c.pins[0]!.entities.temperature;
    c.pins[2]!.entities.battery = c.pins[2]!.entities.humidity;
    const d = findDuplicateBindings(c);
    expect(d.get("sensor.example_living_temperature")).toEqual([
      { pin: 0, role: "temperature" },
      { pin: 1, role: "battery" },
    ]);
    expect(d.get("sensor.example_study_humidity")).toHaveLength(2);
    expect(d.size).toBe(2);
  });
});
