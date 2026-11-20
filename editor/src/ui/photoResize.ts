// Browser-side photo work: decode an attached image, cut the centre square (lib/photos.ts
// decides which pixels) and encode it as a PHOTO_SIZE_PX square PNG for the export.

import { PHOTO_SIZE_PX, squareCrop } from "../lib/photos";

export async function toDevicePhoto(blob: Blob): Promise<Uint8Array> {
  const bitmap = await createImageBitmap(blob);
  try {
    const crop = squareCrop(bitmap.width, bitmap.height);
    const canvas = document.createElement("canvas");
    canvas.width = PHOTO_SIZE_PX;
    canvas.height = PHOTO_SIZE_PX;
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("no 2D canvas");
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(bitmap, crop.sx, crop.sy, crop.size, crop.size, 0, 0, PHOTO_SIZE_PX, PHOTO_SIZE_PX);
    const png = await new Promise<Blob>((resolve, reject) =>
      canvas.toBlob((b) => (b ? resolve(b) : reject(new Error("could not encode PNG"))), "image/png"),
    );
    return new Uint8Array(await png.arrayBuffer());
  } finally {
    bitmap.close();
  }
}

/** Decode stored PNG bytes into something a canvas can draw. Cached by byte array identity. */
const cache = new WeakMap<Uint8Array, Promise<ImageBitmap>>();
export function decodePhoto(bytes: Uint8Array): Promise<ImageBitmap> {
  let p = cache.get(bytes);
  if (!p) {
    p = createImageBitmap(new Blob([bytes as BlobPart], { type: "image/png" }));
    cache.set(bytes, p);
  }
  return p;
}
