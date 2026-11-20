// Entry point: builds the toolbar, map view, sidebar and preview, and wires export/open.

import exampleConfig from "../../examples/flat/config.json";
import { loadConfig } from "./lib/autosave";
import { exportFiles, openFiles, staleExportedPhotos, zipFiles } from "./lib/files";
import * as ops from "./lib/ops";
import { canRedo, canUndo } from "./lib/history";
import { checkImportedConfig, hasErrors, validateConfig } from "./lib/validate";
import { KINDS, LAYERS, type Config, type Kind } from "./lib/types";
import { buildScene, parseReadings, sampleReadings } from "./preview/scene";
import { download, h, readFileBytes, renderInto } from "./ui/dom";
import { MapView, isTyping } from "./ui/mapView";
import { Sidebar } from "./ui/panels";
import { decodePhoto, toDevicePhoto, type DevicePhoto } from "./ui/photoResize";
import { drawPreview } from "./ui/previewDraw";
import { Store, type Tool } from "./ui/store";
import "./styles.css";

const example = (): Config => JSON.parse(JSON.stringify(exampleConfig)) as Config;

const store = new Store(loadConfig(localStorage) ?? ops.blankConfig());
const app = document.getElementById("app") as HTMLElement;

const toolbar = h("header", { class: "toolbar" });
const mapHost = h("main", { class: "map-host" });
// Drawing controls float over the map so the toolbar never changes height mid-gesture (a
// reflow would move the canvas under a finger between two taps).
const drawBar = h("div", { class: "draw-bar" });
const side = h("aside", { class: "sidebar" });
const status = h("div", { class: "status", role: "status", "aria-live": "polite" });
app.append(toolbar, h("div", { class: "workspace" }, mapHost, side), status);

const map = new MapView(mapHost, store);
// A reload or tab close inside the autosave debounce must not lose the last edit.
window.addEventListener("pagehide", () => store.flush());
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "hidden") store.flush();
});
mapHost.append(drawBar);
new Sidebar(side, store);

// ---- project actions ----------------------------------------------------------------------------

function hasContent(): boolean {
  return store.config.rooms.length > 0 || store.config.pins.length > 0;
}

function replaceProject(config: Config, photos = new Map<string, DevicePhoto>()): void {
  store.load(config, photos);
  map.fit();
}

/**
 * Re-encode every opened image to the export shape. Original bytes never reach the store, so
 * they can never reach an export; a file that does not decode is reported and dropped.
 */
async function normaliseImages(images: Map<string, Uint8Array>): Promise<{ photos: Map<string, DevicePhoto>; rejected: string[] }> {
  const photos = new Map<string, DevicePhoto>();
  const rejected: string[] = [];
  for (const [id, bytes] of images) {
    try {
      photos.set(id, await toDevicePhoto(new Blob([bytes as BlobPart])));
    } catch {
      rejected.push(id);
    }
  }
  return { photos, rejected };
}

async function openProject(files: FileList | null): Promise<void> {
  if (!files?.length) return;
  try {
    const named = await Promise.all([...files].map(async (f) => ({ name: f.webkitRelativePath || f.name, bytes: await readFileBytes(f) })));
    const opened = openFiles(named);
    // Check the config before anything changes: a rejected file leaves the project and its
    // autosave exactly as they were.
    const check = opened.config === undefined ? undefined : checkImportedConfig(opened.config);
    if (check && !check.ok) throw new Error(`${opened.configName ?? "config"} ${check.message}`);
    const { photos, rejected } = await normaliseImages(opened.images);
    const notes = [
      rejected.length ? `not images, ignored: ${rejected.join(", ")}` : "",
      opened.ignored.length ? `ignored ${opened.ignored.join(", ")} (file names must be photo ids)` : "",
    ].filter((x) => x);
    const tail = notes.length ? `; ${notes.join("; ")}` : "";
    if (!check) {
      for (const [id, png] of photos) store.setPhoto(id, png);
      store.set({ message: { text: `${photos.size} photo(s) added${tail}`, kind: rejected.length ? "error" : "info" } });
      return;
    }
    replaceProject(check.config, photos);
    const n = store.state.issues.filter((i) => i.severity === "error").length;
    store.set({
      message: { text: `Opened ${opened.configName} with ${photos.size} photo(s)${n ? `, ${n} error(s) to fix` : ""}${tail}`, kind: n || rejected.length ? "error" : "info" },
    });
  } catch (e) {
    store.set({ message: { text: `Could not open: ${(e as Error).message}`, kind: "error" } });
  }
}

function checkedExport(): ReturnType<typeof exportFiles> | undefined {
  const issues = validateConfig(store.config, new Set(store.state.photos.keys()));
  if (hasErrors(issues)) {
    store.set({ sidebarTab: "issues", message: { text: "Export refused: fix the errors listed in Issues first", kind: "error" } });
    return undefined;
  }
  return exportFiles(store.config, store.state.photos);
}

function exportedMessage(where: string, missing: string[]): void {
  const tail = missing.length ? `. No image loaded for: ${missing.join(", ")}` : "";
  store.set({ message: { text: `Exported ${where}${tail}`, kind: missing.length ? "error" : "info" } });
}

function exportZip(): void {
  const out = checkedExport();
  if (!out) return;
  download("hearthmap-config.zip", zipFiles(out.files), "application/zip");
  exportedMessage("hearthmap-config.zip", out.missing);
}

interface DirHandle {
  getDirectoryHandle(name: string, o: { create: boolean }): Promise<DirHandle>;
  keys(): AsyncIterable<string>;
  removeEntry(name: string): Promise<void>;
  getFileHandle(name: string, o: { create: boolean }): Promise<{ createWritable(): Promise<{ write(d: BlobPart): Promise<void>; close(): Promise<void> }> }>;
}
const picker = (window as unknown as { showDirectoryPicker?: (o?: object) => Promise<DirHandle> }).showDirectoryPicker;

async function exportToFolder(): Promise<void> {
  const out = checkedExport();
  if (!out || !picker) return;
  let dir: DirHandle;
  try {
    dir = await picker({ mode: "readwrite" });
  } catch {
    return; // cancelled
  }
  try {
    for (const f of out.files) {
      const parts = f.name.split("/");
      let d = dir;
      for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
      const w = await (await d.getFileHandle(parts[parts.length - 1] as string, { create: true })).createWritable();
      await w.write(f.bytes as BlobPart);
      await w.close();
    }
    // Photos an earlier export left behind that this config no longer uses: only names the
    // editor itself writes (photos/<photo-id>.png), only inside photos/, and only on a yes.
    const photosDir = await dir.getDirectoryHandle("photos", { create: true });
    const existing: string[] = [];
    for await (const name of photosDir.keys()) existing.push(name);
    const stale = staleExportedPhotos(existing, out.files);
    let removed: string[] = [];
    if (stale.length && confirm(`Remove photos from an earlier export that this config no longer uses?\n\n${stale.map((n) => `photos/${n}`).join("\n")}`)) {
      for (const n of stale) await photosDir.removeEntry(n);
      removed = stale;
    }
    const kept = stale.filter((n) => !removed.includes(n));
    exportedMessage(
      `to the chosen folder${removed.length ? `; removed ${removed.map((n) => `photos/${n}`).join(", ")}` : ""}${kept.length ? `; left unused ${kept.map((n) => `photos/${n}`).join(", ")}` : ""}`,
      out.missing,
    );
  } catch (e) {
    store.set({ message: { text: `Could not write to that folder: ${(e as Error).message}. Use Download zip instead.`, kind: "error" } });
  }
}

async function loadBackdrop(file: File | undefined): Promise<void> {
  if (!file) return;
  const url = URL.createObjectURL(file);
  const image = new Image();
  image.src = url;
  try {
    await image.decode();
  } catch {
    store.set({ message: { text: "That file is not an image the browser can show", kind: "error" } });
    return;
  }
  const old = store.state.backdrop;
  if (old) URL.revokeObjectURL(old.url);
  store.set({ backdrop: { image, url, width: image.naturalWidth, height: image.naturalHeight, opacity: old?.opacity ?? 0.6 } });
}

// ---- toolbar ------------------------------------------------------------------------------------

const TOOLS: [Tool, string, string][] = [
  ["select", "Select", "V"],
  ["room", "Draw room", "R"],
  ["pin", "Add pin", "P"],
  ["pan", "Pan", "H"],
];

function renderToolbar(): void {
  const st = store.state;
  const errors = st.issues.filter((i) => i.severity === "error").length;
  const warnings = st.issues.length - errors;
  renderInto(toolbar, [
    h("span", { class: "brand" }, "hearthmap editor"),
    h(
      "div",
      { class: "group" },
      h(
        "button",
        {
          "data-testid": "new",
          onclick: () => {
            if (!hasContent() || confirm("Start a blank project? The current one is replaced (export it first if you want to keep it).")) replaceProject(ops.blankConfig());
          },
        },
        "New",
      ),
      h(
        "label",
        { class: "button", title: "A config.json with its photos, or an exported zip" },
        "Open...",
        h("input", {
          type: "file",
          multiple: true,
          accept: ".json,.zip,.png,.jpg,.jpeg,application/json,application/zip,image/png,image/jpeg",
          hidden: true,
          "data-testid": "open-files",
          onchange: (e: Event) => void openProject((e.target as HTMLInputElement).files),
        }),
      ),
      h(
        "button",
        {
          "data-testid": "load-example",
          onclick: () => {
            if (!hasContent() || confirm("Load the fictional example flat? The current project is replaced.")) replaceProject(example());
          },
        },
        "Example flat",
      ),
    ),
    h(
      "div",
      { class: "group" },
      h("button", { disabled: !canUndo(st.history), onclick: () => store.undo(), title: "Undo (Ctrl/Cmd+Z)", "data-testid": "undo" }, "Undo"),
      h("button", { disabled: !canRedo(st.history), onclick: () => store.redo(), title: "Redo (Shift+Ctrl/Cmd+Z)", "data-testid": "redo" }, "Redo"),
    ),
    h(
      "div",
      { class: "group", role: "radiogroup", "aria-label": "Tool" },
      TOOLS.map(([id, label, key]) =>
        h(
          "button",
          {
            class: st.tool === id ? "active" : "",
            "aria-pressed": st.tool === id ? "true" : "false",
            title: `${label} (${key})`,
            "data-testid": `tool-${id}`,
            onclick: () => store.set({ tool: id, draft: id === "room" ? st.draft : [] }),
          },
          label,
        ),
      ),
      h(
        "select",
        { title: "Kind of pin to add", "data-testid": "pin-kind-tool", onchange: (e: Event) => store.set({ pinKind: (e.target as HTMLSelectElement).value as Kind, tool: "pin" }) },
        KINDS.map((k) => h("option", { value: k, selected: st.pinKind === k }, k.replace("_", " "))),
      ),
      h(
        "label",
        { class: "check", title: "Snap to corners and horizontal/vertical (Alt suspends)" },
        h("input", { type: "checkbox", checked: st.snapping, onchange: (e: Event) => store.set({ snapping: (e.target as HTMLInputElement).checked }) }),
        " Snap",
      ),
    ),
    h(
      "div",
      { class: "group" },
      h(
        "label",
        { class: "button", title: "A floor-plan image to trace over. It is never exported." },
        "Floor plan...",
        h("input", { type: "file", accept: "image/*", hidden: true, "data-testid": "backdrop-file", onchange: (e: Event) => void loadBackdrop((e.target as HTMLInputElement).files?.[0]) }),
      ),
      st.backdrop
        ? [
            h("input", {
              type: "range",
              min: 0,
              max: 1,
              step: 0.05,
              value: st.backdrop.opacity,
              title: "Floor plan opacity",
              oninput: (e: Event) => st.backdrop && store.set({ backdrop: { ...st.backdrop, opacity: Number((e.target as HTMLInputElement).value) } }),
            }),
            h(
              "button",
              {
                title: "Make the map area the floor plan's shape, so it is not stretched",
                onclick: () => {
                  const b = store.state.backdrop;
                  if (b && store.edit((c) => ops.fitMapAreaToAspect(c, b.width, b.height))) map.fit();
                },
              },
              "Match plan shape",
            ),
            h(
              "button",
              {
                onclick: () => {
                  const b = store.state.backdrop;
                  if (b) URL.revokeObjectURL(b.url);
                  store.set({ backdrop: undefined });
                },
              },
              "Remove plan",
            ),
          ]
        : null,
    ),
    h(
      "div",
      { class: "group" },
      h("button", { title: "Zoom out", onclick: () => map.zoomBy(1 / 1.25) }, "-"),
      h("button", { title: "Fit to window", onclick: () => map.fit() }, "Fit"),
      h("button", { title: "Zoom in", onclick: () => map.zoomBy(1.25) }, "+"),
    ),
    h(
      "div",
      { class: "group right" },
      h("button", { "data-testid": "open-preview", onclick: () => openPreview() }, "Preview"),
      h(
        "button",
        { class: errors ? "badge err" : warnings ? "badge warn" : "badge ok", "data-testid": "issue-badge", onclick: () => store.set({ sidebarTab: "issues" }) },
        errors ? `${errors} error${errors > 1 ? "s" : ""}` : warnings ? `${warnings} warning${warnings > 1 ? "s" : ""}` : "Valid",
      ),
      h("button", { class: "primary", "data-testid": "export-zip", onclick: () => exportZip() }, "Download zip"),
      picker ? h("button", { "data-testid": "export-folder", onclick: () => void exportToFolder() }, "Save to folder...") : null,
    ),
  ]);
  renderInto(drawBar, st.tool === "room" && st.draft.length ? [
    h("span", { class: "muted" }, `${st.draft.length} corner${st.draft.length > 1 ? "s" : ""}`),
    h("button", { "data-testid": "finish-room", disabled: st.draft.length < 3, onclick: () => map.finishDraft() }, "Finish room"),
    h("button", { onclick: () => store.set({ draft: st.draft.slice(0, -1) }) }, "Undo corner"),
    h("button", { onclick: () => store.set({ draft: [] }) }, "Cancel"),
  ] : []);
  renderInto(status, st.message ? [h("span", { class: st.message.kind, "data-testid": "message" }, st.message.text)] : [h("span", { class: "muted" }, "Autosaves the config in this browser (not photos, not the floor plan, never a token).")]);
}

store.subscribe(renderToolbar);
renderToolbar();

// ---- preview ------------------------------------------------------------------------------------

let previewRaf = 0;
function openPreview(): void {
  const canvas = h("canvas", { width: 1280, height: 800, class: "preview-canvas", "data-testid": "preview-canvas" });
  const ctx = canvas.getContext("2d") as CanvasRenderingContext2D;
  const photoImgs = new Map<number, CanvasImageSource>();
  const loadPhotos = () => {
    photoImgs.clear();
    store.config.pins.forEach((p, i) => {
      const bytes = p.photo ? store.state.photos.get(p.photo) : undefined;
      if (bytes) void decodePhoto(bytes).then((b) => photoImgs.set(i, b));
    });
  };
  loadPhotos();
  const controls = h("div", { class: "preview-controls" });
  const renderControls = () => {
    const st = store.state;
    renderInto(controls, [
      h("strong", {}, "Preview at 1280 x 800"),
      h("span", { class: "muted" }, " Approximate: the panel draws with LVGL and core's logic; this is for judging placement, not pixels."),
      h(
        "div",
        { class: "row" },
        LAYERS.map((l) =>
          h("button", { class: st.previewLayer === l ? "active" : "", "data-testid": `preview-layer-${l}`, onclick: () => store.set({ previewLayer: l }) }, l),
        ),
        h(
          "button",
          { class: st.readings.source === "sample" ? "active" : "", onclick: () => store.set({ readings: { source: "sample", set: sampleReadings(store.config) } }) },
          "Sample readings",
        ),
        h(
          "label",
          { class: "button" },
          "Readings file...",
          h("input", {
            type: "file",
            accept: ".json",
            hidden: true,
            onchange: async (e: Event) => {
              const f = (e.target as HTMLInputElement).files?.[0];
              if (!f) return;
              try {
                store.set({ readings: { source: "pasted", set: parseReadings(await f.text()) } });
              } catch (err) {
                store.set({ message: { text: `Readings: ${(err as Error).message}`, kind: "error" } });
              }
            },
          }),
        ),
        h("button", { "data-testid": "close-preview", onclick: () => close() }, "Close"),
      ),
    ]);
  };
  const dialog = h("div", { class: "preview-dialog", role: "dialog", "aria-modal": "true", "aria-label": "Preview" }, controls, h("div", { class: "preview-frame" }, canvas));
  document.body.append(dialog);
  const start = performance.now();
  const frame = () => {
    const st = store.state;
    drawPreview(ctx, buildScene(store.config, st.readings.set, st.previewLayer), photoImgs, (performance.now() - start) / 1000);
    previewRaf = requestAnimationFrame(frame);
  };
  const unsubscribe = store.subscribe(() => renderControls());
  const close = () => {
    cancelAnimationFrame(previewRaf);
    dialog.remove();
    unsubscribe();
  };
  renderControls();
  frame();
}

// ---- keyboard -----------------------------------------------------------------------------------

window.addEventListener("keydown", (e) => {
  if (isTyping(e)) return;
  const mod = e.metaKey || e.ctrlKey;
  const st = store.state;
  if (mod && e.key.toLowerCase() === "z") {
    e.preventDefault();
    if (e.shiftKey) store.redo();
    else store.undo();
    return;
  }
  if (mod && e.key.toLowerCase() === "y") {
    e.preventDefault();
    store.redo();
    return;
  }
  if (mod) return;
  const sel = st.selection;
  switch (e.key) {
    case "Escape":
      store.set(st.draft.length ? { draft: [] } : { selection: undefined, tool: "select" });
      break;
    case "Enter":
      if (st.tool === "room") map.finishDraft();
      break;
    case "Delete":
    case "Backspace":
      e.preventDefault();
      if (st.tool === "room" && st.draft.length) store.set({ draft: st.draft.slice(0, -1) });
      else if (sel?.type === "room" && sel.vertex !== undefined) store.edit((c) => ops.deleteVertex(c, sel.index, sel.vertex as number), { selection: { type: "room", index: sel.index } });
      else if (sel?.type === "room") store.edit((c) => ops.deleteRoom(c, sel.index), { selection: undefined });
      else if (sel?.type === "pin") store.edit((c) => ops.deletePin(c, sel.index), { selection: undefined });
      break;
    case "v":
      store.set({ tool: "select", draft: [] });
      break;
    case "r":
      store.set({ tool: "room" });
      break;
    case "p":
      store.set({ tool: "pin", draft: [] });
      break;
    case "h":
      store.set({ tool: "pan", draft: [] });
      break;
  }
});

// Test and debugging hook: read-only snapshot of the config for the e2e smoke test.
(window as unknown as { __hearthmap?: { config: () => Config } }).__hearthmap = { config: () => store.config };
