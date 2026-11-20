import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import type { Config } from "../src/lib/types";

export const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

export function repoText(path: string): string {
  return readFileSync(resolve(REPO, path), "utf8");
}

export function exampleText(): string {
  return repoText("examples/flat/config.json");
}

export function example(): Config {
  return JSON.parse(exampleText()) as Config;
}

/** Photo ids the example references (it ships no image files). */
export function examplePhotoIds(): Set<string> {
  return new Set(example().pins.map((p) => p.photo).filter((x): x is string => x !== undefined));
}

export function clone<T>(v: T): T {
  return JSON.parse(JSON.stringify(v)) as T;
}
