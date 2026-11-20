import { describe, expect, it } from "vitest";
import { loadConfig, saveConfig } from "../src/lib/autosave";
import { exportFiles, openFiles, zipFiles } from "../src/lib/files";
import { normaliseJsonText, serialiseConfig } from "../src/lib/serialise";
import { schemaIssues, validateConfig, hasErrors } from "../src/lib/validate";
import { example, examplePhotoIds, exampleText } from "./helpers";

class MemoryStore {
  data = new Map<string, string>();
  getItem(k: string) {
    return this.data.get(k) ?? null;
  }
  setItem(k: string, v: string) {
    this.data.set(k, v);
  }
  removeItem(k: string) {
    this.data.delete(k);
  }
}

describe("round trip of the fictional flat", () => {
  it("exports byte-equivalent to the example after normalisation", () => {
    expect(serialiseConfig(example())).toBe(normaliseJsonText(exampleText()));
  });

  it("validates against the real schema with no errors", () => {
    expect(schemaIssues(example())).toEqual([]);
    const issues = validateConfig(example(), examplePhotoIds());
    expect(issues.filter((i) => i.severity === "error")).toEqual([]);
  });

  it("survives export to a zip and opening it again", () => {
    const { files } = exportFiles(example(), new Map());
    const opened = openFiles([{ name: "flat.zip", bytes: zipFiles(files) }]);
    expect(serialiseConfig(opened.config as never)).toBe(normaliseJsonText(exampleText()));
    expect(hasErrors(validateConfig(opened.config))).toBe(false);
  });

  it("survives autosave and reload", () => {
    const store = new MemoryStore();
    expect(saveConfig(store, example())).toBe(true);
    const back = loadConfig(store);
    expect(back && serialiseConfig(back)).toBe(normaliseJsonText(exampleText()));
  });
});
