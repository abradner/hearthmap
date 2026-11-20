// Autosave of the config to localStorage. Only the config is written: never the Home
// Assistant token (which never enters the config or any store; haClient.ts), never photos
// (they are large; the UI says they must be re-attached after a reload), never the floor-plan
// backdrop (not part of the export at all).

import { serialiseConfig } from "./serialise";
import { structuralIssue } from "./validate";
import type { Config } from "./types";

export const AUTOSAVE_KEY = "hearthmap-editor:config:v1";

export type KeyValueStore = Pick<Storage, "getItem" | "setItem" | "removeItem">;

/** Write the config. Returns false when storage refused (quota, private mode). */
export function saveConfig(store: KeyValueStore, config: Config): boolean {
  try {
    store.setItem(AUTOSAVE_KEY, serialiseConfig(config));
    return true;
  } catch {
    return false;
  }
}

/** The saved config, or undefined when there is none or the editor could not render it. */
export function loadConfig(store: KeyValueStore): Config | undefined {
  let text: string | null;
  try {
    text = store.getItem(AUTOSAVE_KEY);
  } catch {
    return undefined;
  }
  if (text === null) return undefined;
  try {
    const data = JSON.parse(text) as unknown;
    return structuralIssue(data) === undefined ? (data as Config) : undefined;
  } catch {
    return undefined;
  }
}

export function clearConfig(store: KeyValueStore): void {
  try {
    store.removeItem(AUTOSAVE_KEY);
  } catch {
    // Nothing to do: storage is unavailable.
  }
}
