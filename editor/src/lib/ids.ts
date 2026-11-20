// Id generation that always satisfies the schema's patterns:
//   room/pin id: ^[a-z][a-z0-9_]{0,47}$
//   photo id:    ^[a-z0-9][a-z0-9_-]{0,47}$

export const ID_PATTERN = /^[a-z][a-z0-9_]{0,47}$/;
export const PHOTO_ID_PATTERN = /^[a-z0-9][a-z0-9_-]{0,47}$/;
export const ENTITY_ID_PATTERN = /^[a-z0-9_]+\.[a-z0-9_]+$/;

const MAX_ID = 48;

function words(name: string): string[] {
  return name
    .normalize("NFKD")
    .replace(/[̀-ͯ]/g, "")
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((w) => w.length > 0);
}

/** A room/pin id from a display name; `fallback` when nothing usable is left. */
export function slugId(name: string, fallback: string): string {
  let s = words(name).join("_");
  if (!/^[a-z]/.test(s)) s = s ? `${fallback}_${s}` : fallback;
  return s.slice(0, MAX_ID).replace(/_+$/, "") || fallback;
}

/** A photo id from a display name (hyphenated, as in the example: "fiddle-leaf-fig"). */
export function slugPhotoId(name: string, fallback: string): string {
  const s = words(name).join("-").slice(0, MAX_ID).replace(/-+$/, "");
  return s || fallback;
}

/** `base`, or `base_2`, `base_3`, ... : the first not in `taken`, never longer than 48. */
export function uniqueId(base: string, taken: Iterable<string>, sep = "_"): string {
  const used = new Set(taken);
  if (!used.has(base)) return base;
  for (let n = 2; ; n++) {
    const suffix = `${sep}${n}`;
    const candidate = base.slice(0, MAX_ID - suffix.length) + suffix;
    if (!used.has(candidate)) return candidate;
  }
}
