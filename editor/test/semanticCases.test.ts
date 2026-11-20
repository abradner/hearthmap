// The cases in schema/semantic-cases.json, through the editor's checks. Core's tests read the
// same file (core/tests/semantic_cases_test.cpp, the enforcement), so a config the editor
// exports is one the panel accepts, case by case (docs/decisions.md D-040, D-048).
import { describe, expect, it } from "vitest";
import type { Config } from "../src/lib/types";
import { validateConfig } from "../src/lib/validate";
import { clone, repoText } from "./helpers";

interface SemanticCase {
  name: string;
  polygon?: [number, number][];
  map_area?: { x: number; y: number; width: number; height: number };
  valid: boolean;
  path?: string;
}

const doc = JSON.parse(repoText("schema/semantic-cases.json")) as { base: Config; cases: SemanticCase[] };

function configFor(c: SemanticCase): Config {
  const config = clone(doc.base);
  if (c.polygon) config.rooms[0]!.polygon = c.polygon;
  if (c.map_area) config.canvas.map_area = c.map_area;
  return config;
}

describe("schema/semantic-cases.json (shared with core's validate())", () => {
  it("exercises both outcomes", () => {
    expect(doc.cases.some((c) => c.valid)).toBe(true);
    expect(doc.cases.some((c) => !c.valid)).toBe(true);
  });

  for (const c of doc.cases) {
    it(`${c.valid ? "accepts" : "refuses"}: ${c.name}`, () => {
      const errors = validateConfig(configFor(c)).filter((i) => i.severity === "error");
      if (c.valid) expect(errors).toEqual([]);
      else expect(errors.map((i) => i.path)).toContain(c.path);
    });
  }
});
