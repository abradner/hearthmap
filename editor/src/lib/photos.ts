// Plant photo geometry. The export holds one square PNG per photo, PHOTO_SIZE_PX a side, cut
// from the centre of whatever the author attached (or from a point they choose). The pixel
// work happens in the browser (ui/photoResize.ts); this module decides which pixels.

import { PHOTO_SIZE_PX } from "./defaults";

export interface CropRect {
  sx: number;
  sy: number;
  size: number;
}

/**
 * The largest square inside a `width` x `height` image, centred on (cx, cy) given as 0..1
 * fractions of the image (default: the centre), moved inward as needed to stay inside.
 */
export function squareCrop(width: number, height: number, cx = 0.5, cy = 0.5): CropRect {
  if (!(width > 0 && height > 0)) throw new RangeError("image has no pixels");
  const size = Math.min(width, height);
  const clamp = (v: number, max: number) => Math.min(Math.max(v, 0), max);
  const sx = clamp(Math.round(cx * width - size / 2), width - size);
  const sy = clamp(Math.round(cy * height - size / 2), height - size);
  return { sx, sy, size };
}

export { PHOTO_SIZE_PX };

const PNG_SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];

/** Width and height from a PNG's IHDR, or undefined when the bytes are not a PNG. */
export function pngSize(bytes: Uint8Array): { width: number; height: number } | undefined {
  if (bytes.length < 24 || PNG_SIGNATURE.some((b, i) => bytes[i] !== b)) return undefined;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return { width: dv.getUint32(16), height: dv.getUint32(20) };
}

/** True when a photo is already in the export shape: a PNG, square, PHOTO_SIZE_PX a side. */
export function isExportShaped(bytes: Uint8Array): boolean {
  const s = pngSize(bytes);
  return s !== undefined && s.width === PHOTO_SIZE_PX && s.height === PHOTO_SIZE_PX;
}
