// Regression tests for the first review round on the editor PR.
import { expect, test, type Page } from "@playwright/test";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { strFromU8, unzipSync } from "fflate";
import { pngSize } from "../src/lib/photos";
import type { Config } from "../src/lib/types";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const exampleText = readFileSync(resolve(repo, "examples/flat/config.json"), "utf8");
const GPS_MARKER = "GPS-MARKER-51.5007N-0.1246W";

const cfg = (page: Page) => page.evaluate(() => (window as unknown as { __hearthmap: { config: () => Config } }).__hearthmap.config());

async function fresh(page: Page) {
  await page.goto("/");
  await page.evaluate(() => localStorage.clear());
  await page.reload();
}

/**
 * A real JPEG (encoded by the browser) with an APP1 Exif segment spliced in after SOI. The
 * segment is a valid TIFF block whose IFD0 points at a GPS IFD holding GPSLatitudeRef and an
 * ASCII tag carrying GPS_MARKER, so "the metadata survived" is a byte search for the marker.
 */
async function jpegWithExif(page: Page): Promise<Buffer> {
  const b64 = await page.evaluate(async () => {
    const c = document.createElement("canvas");
    c.width = 240;
    c.height = 160;
    const x = c.getContext("2d")!;
    x.fillStyle = "#3fa56b";
    x.fillRect(0, 0, 240, 160);
    const blob = await new Promise<Blob>((r) => c.toBlob((b) => r(b!), "image/jpeg", 0.9));
    const bytes = new Uint8Array(await blob.arrayBuffer());
    let s = "";
    for (const v of bytes) s += String.fromCharCode(v);
    return btoa(s);
  });
  const jpeg = Buffer.from(b64, "base64");
  const marker = Buffer.from(`${GPS_MARKER}\0`, "ascii");
  // TIFF, big endian. IFD0 at 8: 1 entry (GPSInfo 0x8825 -> offset of GPS IFD).
  const ifd0 = 8;
  const gpsIfd = ifd0 + 2 + 12 + 4;
  const markerOff = gpsIfd + 2 + 2 * 12 + 4;
  const tiff = Buffer.alloc(markerOff + marker.length);
  tiff.write("MM", 0, "ascii");
  tiff.writeUInt16BE(42, 2);
  tiff.writeUInt32BE(ifd0, 4);
  tiff.writeUInt16BE(1, ifd0);
  tiff.writeUInt16BE(0x8825, ifd0 + 2);
  tiff.writeUInt16BE(4, ifd0 + 4); // LONG
  tiff.writeUInt32BE(1, ifd0 + 6);
  tiff.writeUInt32BE(gpsIfd, ifd0 + 10);
  tiff.writeUInt32BE(0, ifd0 + 14);
  tiff.writeUInt16BE(2, gpsIfd);
  // GPSLatitudeRef "N"
  tiff.writeUInt16BE(1, gpsIfd + 2);
  tiff.writeUInt16BE(2, gpsIfd + 4);
  tiff.writeUInt32BE(2, gpsIfd + 6);
  tiff.write("N\0", gpsIfd + 10, "ascii");
  // GPSMapDatum (0x0012), ASCII, holding the marker
  tiff.writeUInt16BE(0x0012, gpsIfd + 14);
  tiff.writeUInt16BE(2, gpsIfd + 16);
  tiff.writeUInt32BE(marker.length, gpsIfd + 18);
  tiff.writeUInt32BE(markerOff, gpsIfd + 22);
  tiff.writeUInt32BE(0, gpsIfd + 26);
  marker.copy(tiff, markerOff);
  const payload = Buffer.concat([Buffer.from("Exif\0\0", "ascii"), tiff]);
  const app1 = Buffer.alloc(4);
  app1.writeUInt16BE(0xffe1, 0);
  app1.writeUInt16BE(payload.length + 2, 2);
  return Buffer.concat([jpeg.subarray(0, 2), app1, payload, jpeg.subarray(2)]);
}

test("photos opened with a project are re-encoded; metadata and non-images never reach the export", async ({ page }) => {
  await fresh(page);
  const jpeg = await jpegWithExif(page);
  expect(jpeg.includes(GPS_MARKER)).toBe(true); // positive control: the marker is in the input
  await page.getByTestId("open-files").setInputFiles([
    { name: "config.json", mimeType: "application/json", buffer: Buffer.from(exampleText) },
    { name: "basil.jpg", mimeType: "image/jpeg", buffer: jpeg },
    { name: "monstera.png", mimeType: "image/png", buffer: Buffer.from("this is not an image at all") },
  ]);
  await expect(page.getByTestId("message")).toContainText("monstera");
  const [download] = await Promise.all([page.waitForEvent("download"), page.getByTestId("export-zip").click()]);
  const zip = unzipSync(new Uint8Array(readFileSync(await download.path())));
  expect(Object.keys(zip).sort()).toEqual(["config.json", "photos/basil.png"]);
  const basil = Buffer.from(zip["photos/basil.png"]!);
  expect(pngSize(basil)).toEqual({ width: 128, height: 128 });
  expect(basil.includes(GPS_MARKER)).toBe(false);
  expect(basil.includes("Exif")).toBe(false);
  expect(JSON.parse(strFromU8(zip["config.json"]!))).toEqual(JSON.parse(exampleText));
});

test("a structurally broken config is rejected and the current project and autosave are untouched", async ({ page }) => {
  await fresh(page);
  await page.getByTestId("load-example").click();
  await page.waitForTimeout(500); // past the autosave debounce
  const before = await page.evaluate(() => JSON.stringify({ ...localStorage }));
  await page.getByTestId("open-files").setInputFiles([
    { name: "config.json", mimeType: "application/json", buffer: Buffer.from('{"version":1,"canvas":null,"rooms":[],"pins":[]}') },
  ]);
  await expect(page.getByTestId("message")).toContainText("Could not open");
  await page.waitForTimeout(500);
  expect(await page.evaluate(() => JSON.stringify({ ...localStorage }))).toBe(before);
  expect((await cfg(page)).rooms).toHaveLength(7);
  await page.reload();
  expect((await cfg(page)).rooms).toHaveLength(7);
});

test("names count Unicode code points, as the schema does: 48 astral characters fit", async ({ page }) => {
  await fresh(page);
  await page.getByTestId("load-example").click();
  await page.getByTestId("tab-rooms").click();
  await page.getByRole("button", { name: "Kitchen" }).click();
  const name = "\u{1F33F}".repeat(48); // 48 code points, 96 UTF-16 units
  await page.getByTestId("room-name").fill(name);
  await page.getByTestId("room-name").press("Tab");
  const saved = (await cfg(page)).rooms[1]!.name;
  expect([...saved].length).toBe(48);
  // One more is refused with a message, and the name stays as it was.
  await page.getByTestId("room-name").fill(`${name}x`);
  await page.getByTestId("room-name").press("Tab");
  await expect(page.getByTestId("message")).toContainText("48");
  expect((await cfg(page)).rooms[1]!.name).toBe(saved);
});
