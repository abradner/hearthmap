import { describe, expect, it } from "vitest";
import { PHOTO_SIZE_PX, isExportShaped, pngSize, squareCrop } from "../src/lib/photos";

function pngHeader(w: number, h: number): Uint8Array {
  const b = new Uint8Array(24);
  b.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  const dv = new DataView(b.buffer);
  dv.setUint32(16, w);
  dv.setUint32(20, h);
  return b;
}

describe("squareCrop", () => {
  it("centres the largest square in landscape and portrait images", () => {
    expect(squareCrop(400, 300)).toEqual({ sx: 50, sy: 0, size: 300 });
    expect(squareCrop(300, 400)).toEqual({ sx: 0, sy: 50, size: 300 });
    expect(squareCrop(128, 128)).toEqual({ sx: 0, sy: 0, size: 128 });
  });
  it("moves a chosen centre inward so the square stays inside", () => {
    expect(squareCrop(400, 300, 0, 0.5)).toEqual({ sx: 0, sy: 0, size: 300 });
    expect(squareCrop(400, 300, 1, 0.5)).toEqual({ sx: 100, sy: 0, size: 300 });
    expect(squareCrop(400, 300, 0.6, 0.5)).toEqual({ sx: 90, sy: 0, size: 300 });
  });
  it("refuses an empty image", () => {
    expect(() => squareCrop(0, 10)).toThrow(RangeError);
  });
});

describe("pngSize / isExportShaped", () => {
  it("reads IHDR and recognises the export shape", () => {
    expect(pngSize(pngHeader(640, 480))).toEqual({ width: 640, height: 480 });
    expect(isExportShaped(pngHeader(PHOTO_SIZE_PX, PHOTO_SIZE_PX))).toBe(true);
    expect(isExportShaped(pngHeader(PHOTO_SIZE_PX, PHOTO_SIZE_PX + 1))).toBe(false);
    expect(pngSize(new Uint8Array(24))).toBeUndefined();
    expect(pngSize(new Uint8Array(4))).toBeUndefined();
  });
});
