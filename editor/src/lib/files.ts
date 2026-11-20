// Opening and exporting projects as files. Pure byte-level logic; the UI supplies the bytes
// (file inputs, the File System Access API) and does the image resizing (photos.ts).
//
// Export layout (what the firmware build and the host renderer read):
//   config.json            the config (serialise.ts)
//   photos/<photo-id>.png  one square PNG per photo id a plant pin uses, PHOTO_SIZE_PX a side

import { unzipSync, zipSync, strToU8, strFromU8 } from "fflate";
import { serialiseConfig } from "./serialise";
import { PHOTO_ID_PATTERN } from "./ids";
import type { Config, PhotoStore } from "./types";

export interface NamedBytes {
  name: string;
  bytes: Uint8Array;
}

/**
 * The files of an export. Only photos some pin references are included, so a detached photo
 * does not leak into the output; referenced photo ids with no image are listed in `missing`.
 */
export function exportFiles(config: Config, photos: PhotoStore): { files: NamedBytes[]; missing: string[] } {
  const files: NamedBytes[] = [{ name: "config.json", bytes: strToU8(serialiseConfig(config)) }];
  const missing: string[] = [];
  const used = [...new Set(config.pins.map((p) => p.photo).filter((x): x is string => x !== undefined))].sort();
  for (const id of used) {
    const png = photos.get(id);
    if (png) files.push({ name: `photos/${id}.png`, bytes: png });
    else missing.push(id);
  }
  return { files, missing };
}

/**
 * A zip of `files`. Timestamps are fixed so the same project always zips to the same bytes.
 * PNGs are stored (already compressed); text is deflated.
 */
export function zipFiles(files: readonly NamedBytes[]): Uint8Array {
  const mtime = new Date(Date.UTC(2020, 0, 1));
  const tree: Record<string, [Uint8Array, { level: 0 | 6; mtime: Date }]> = {};
  for (const f of files) tree[f.name] = [f.bytes, { level: f.name.endsWith(".png") ? 0 : 6, mtime }];
  return zipSync(tree);
}

export interface OpenedProject {
  config?: unknown;
  configName?: string;
  photos: Map<string, Uint8Array>;
  /** Images whose name is not a valid photo id, or that are not PNG/JPEG. */
  ignored: string[];
}

const IMAGE = /\.(png|jpe?g)$/i;

function baseName(path: string): string {
  return path.split("/").pop() ?? path;
}

/** The photo id a file name stands for: "photos/basil.png" -> "basil". */
export function photoIdFromName(name: string): string | undefined {
  const stem = baseName(name).replace(IMAGE, "");
  return IMAGE.test(name) && PHOTO_ID_PATTERN.test(stem) ? stem : undefined;
}

/**
 * Sort a mixed bag of opened files: a zip is unpacked and treated as its contents; the first
 * `*.json` is the config (parsed, not validated: the caller validates); images become photos
 * by file name. Throws a user-facing Error when a JSON file does not parse.
 */
export function openFiles(files: readonly NamedBytes[]): OpenedProject {
  const flat: NamedBytes[] = [];
  for (const f of files) {
    if (/\.zip$/i.test(f.name)) {
      const entries = unzipSync(f.bytes);
      for (const [name, bytes] of Object.entries(entries)) {
        if (!name.endsWith("/") && !name.startsWith("__MACOSX/")) flat.push({ name, bytes });
      }
    } else {
      flat.push(f);
    }
  }
  const out: OpenedProject = { photos: new Map(), ignored: [] };
  const jsons = flat.filter((f) => /\.json$/i.test(f.name));
  const configFile = jsons.find((f) => baseName(f.name) === "config.json") ?? jsons[0];
  if (configFile) {
    try {
      out.config = JSON.parse(strFromU8(configFile.bytes)) as unknown;
    } catch {
      throw new Error(`${configFile.name} is not valid JSON`);
    }
    out.configName = configFile.name;
  }
  for (const f of flat) {
    if (!IMAGE.test(f.name)) continue;
    const id = photoIdFromName(f.name);
    if (id) out.photos.set(id, f.bytes);
    else out.ignored.push(f.name);
  }
  return out;
}
