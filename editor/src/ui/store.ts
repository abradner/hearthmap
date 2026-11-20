// Application state and the actions that change it. Config edits go through the pure
// functions in lib/ops.ts and are recorded in lib/history.ts; everything else here is view
// state that nobody undoes. The Home Assistant token is deliberately absent: it lives only in
// its password field and is read at the moment of a fetch (ui/panels.ts).

import { saveConfig } from "../lib/autosave";
import type { SensorOutcome } from "../lib/calibrationImport";
import type { EntityInfo } from "../lib/entities";
import * as H from "../lib/history";
import { validateConfig, type Issue } from "../lib/validate";
import type { Config, Kind, LayerName } from "../lib/types";
import { sampleReadings, type ReadingSet } from "../preview/scene";
import type { DevicePhoto } from "./photoResize";

export type Tool = "select" | "room" | "pin" | "pan";

export type Selection =
  | { type: "room"; index: number; vertex?: number }
  | { type: "pin"; index: number }
  | undefined;

export interface Backdrop {
  image: HTMLImageElement;
  url: string;
  width: number;
  height: number;
  opacity: number;
}

export interface View {
  scale: number;
  ox: number;
  oy: number;
}

export interface State {
  history: H.History<Config>;
  selection: Selection;
  tool: Tool;
  pinKind: Kind;
  /** Vertices of the room being drawn, in map space. */
  draft: [number, number][];
  /** Only re-encoded photos (DevicePhoto) can enter: see ui/photoResize.ts. */
  photos: Map<string, DevicePhoto>;
  entities: EntityInfo[];
  entityFilter: { deviceClass: string; search: string; role?: string };
  backdrop?: Backdrop;
  view: View;
  snapping: boolean;
  previewLayer: LayerName;
  readings: { source: "sample" | "pasted"; set: ReadingSet };
  sidebarTab: "item" | "rooms" | "pins" | "entities" | "layers" | "issues";
  message?: { text: string; kind: "info" | "error" };
  issues: Issue[];
  /** Result of the last calibration import, shown in the Pins tab. */
  calibrationReport?: { source: string; outcomes: SensorOutcome[] };
}

type Listener = (s: State) => void;

export class Store {
  private s: State;
  private listeners: Listener[] = [];
  private saveTimer: ReturnType<typeof setTimeout> | undefined;

  constructor(config: Config) {
    this.s = {
      history: H.createHistory(config),
      selection: undefined,
      tool: "select",
      pinKind: "plant",
      draft: [],
      photos: new Map(),
      entities: [],
      entityFilter: { deviceClass: "", search: "" },
      view: { scale: 1, ox: 0, oy: 0 },
      snapping: true,
      previewLayer: config.layers?.default ?? "temperature",
      readings: { source: "sample", set: sampleReadings(config) },
      sidebarTab: "item",
      issues: [],
    };
    this.s.issues = this.computeIssues();
  }

  get state(): State {
    return this.s;
  }

  get config(): Config {
    return this.s.history.present;
  }

  /** Returns a function that removes the listener. */
  subscribe(l: Listener): () => void {
    this.listeners.push(l);
    return () => {
      this.listeners = this.listeners.filter((x) => x !== l);
    };
  }

  /** Change view state (not undoable). */
  set(patch: Partial<State>): void {
    this.s = { ...this.s, ...patch };
    this.emit();
  }

  /** Apply a config edit as one undo step. Errors from ops become a message, not a crash. */
  edit(f: (c: Config) => Config, patch: Partial<State> = {}): boolean {
    let next: Config;
    try {
      next = f(this.config);
    } catch (e) {
      this.set({ message: { text: (e as Error).message, kind: "error" } });
      return false;
    }
    this.s = { ...this.s, ...patch, history: H.push(this.s.history, next) };
    this.configChanged();
    return true;
  }

  /** Start a gesture (a drag): later live() calls are one undo step until endGesture(). */
  beginGesture(): void {
    this.s = { ...this.s, history: H.begin(this.s.history) };
  }

  live(f: (c: Config) => Config): void {
    this.s = { ...this.s, history: H.replace(this.s.history, f(this.config)) };
    this.emit();
  }

  endGesture(): void {
    this.s = { ...this.s, history: H.commit(this.s.history) };
    this.configChanged();
  }

  undo(): void {
    this.s = { ...this.s, history: H.undo(this.s.history), selection: undefined, draft: [] };
    this.configChanged();
  }

  redo(): void {
    this.s = { ...this.s, history: H.redo(this.s.history), selection: undefined, draft: [] };
    this.configChanged();
  }

  /** Replace the whole project (new, open, load example). Clears history. */
  load(config: Config, photos: Map<string, DevicePhoto> = new Map()): void {
    this.s = {
      ...this.s,
      history: H.createHistory(config),
      photos,
      selection: undefined,
      draft: [],
      previewLayer: config.layers?.default ?? "temperature",
      readings: { source: "sample", set: sampleReadings(config) },
    };
    this.configChanged();
  }

  setPhoto(id: string, png: DevicePhoto): void {
    const photos = new Map(this.s.photos);
    photos.set(id, png);
    this.s = { ...this.s, photos };
    this.configChanged();
  }

  private computeIssues(): Issue[] {
    return validateConfig(this.config, new Set(this.s.photos.keys()));
  }

  private configChanged(): void {
    const readings = this.s.readings.source === "sample" ? { source: "sample" as const, set: sampleReadings(this.config) } : this.s.readings;
    this.s = { ...this.s, issues: this.computeIssues(), readings };
    this.scheduleSave();
    this.emit();
  }

  private scheduleSave(): void {
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this.flush(), 300);
  }

  /** Write a pending autosave now (called when the page is hidden or unloaded). */
  flush(): void {
    if (this.saveTimer === undefined) return;
    clearTimeout(this.saveTimer);
    this.saveTimer = undefined;
    if (!saveConfig(localStorage, this.config)) {
      this.set({ message: { text: "Autosave failed: browser storage is full or disabled", kind: "error" } });
    }
  }

  private emit(): void {
    for (const l of this.listeners) l(this.s);
  }
}
