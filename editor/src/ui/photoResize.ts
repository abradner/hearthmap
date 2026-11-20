// Browser-side photo work: decode an attached image, cut the centre square (lib/photos.ts
// decides which pixels) and encode it as a PHOTO_SIZE_PX square PNG for the export.

import { PHOTO_SIZE_PX, squareCrop } from "../lib/photos";

/**
 * A photo that went through toDevicePhoto: freshly encoded by the browser from decoded pixels,
 * so it holds no bytes of the original file (no EXIF, GPS or other metadata, nothing outside
 * the crop). The store accepts only this type, which is what makes "every exported photo was
 * re-encoded" structural rather than a convention each import path has to remember.
 */
export type DevicePhoto = Uint8Array & { readonly __devicePhoto: unique symbol };

/** Decode any image the browser can read and re-encode it; rejects anything that is not one. */
export async function toDevicePhoto(blob: Blob): Promise<DevicePhoto> {
  let bitmap: ImageBitmap;
  try {
    bitmap = await createImageBitmap(blob);
  } catch {
    throw new Error("not an image this browser can decode");
  }
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
    return new Uint8Array(await png.arrayBuffer()) as DevicePhoto;
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
