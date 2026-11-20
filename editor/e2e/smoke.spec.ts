import { expect, test, type Page } from "@playwright/test";
import { readFileSync } from "node:fs";
import { crc32, deflateSync } from "node:zlib";
import { mkdirSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { strFromU8, unzipSync } from "fflate";
import { pngSize } from "../src/lib/photos";
import { Ajv2020 } from "ajv/dist/2020.js";
import type { Config } from "../src/lib/types";

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, "..", "..");

// The real contract, read from disk (Node's ESM loader will not import JSON without attributes).
const validateSchema = new Ajv2020({ allErrors: true, strict: false }).compile(
  JSON.parse(readFileSync(resolve(repo, "schema/hearthmap.schema.json"), "utf8")) as object,
);

// A real 300x200 PNG (solid green), built here so the photo path truly decodes, crops to a
// square and re-encodes an image.
function makePng(width: number, height: number): Buffer {
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr.set([8, 2, 0, 0, 0], 8); // 8-bit RGB
  const row = Buffer.concat([Buffer.from([0]), Buffer.alloc(width * 3, Buffer.from([0x30, 0xa4, 0x6c]))]);
  const raw = Buffer.concat(Array.from({ length: height }, () => row));
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}

async function fresh(page: Page) {
  await page.goto("/");
  await page.evaluate(() => localStorage.clear());
  await page.reload();
}

async function canvasBox(page: Page) {
  const box = await page.locator("canvas.map-canvas").boundingBox();
  if (!box) throw new Error("no canvas");
  return box;
}

test.describe("editor smoke", () => {
  test("draws a room, drops a pin, exports a zip that validates", async ({ page }) => {
    await fresh(page);
    const b = await canvasBox(page);
    const at = (fx: number, fy: number) => ({ x: b.x + b.width * fx, y: b.y + b.height * fy });

    await page.getByTestId("tool-room").click();
    for (const [fx, fy] of [
      [0.3, 0.3],
      [0.6, 0.3],
      [0.6, 0.65],
      [0.3, 0.65],
    ] as const) {
      const p = at(fx, fy);
      await page.mouse.click(p.x, p.y);
    }
    await page.getByTestId("finish-room").click();
    await page.getByTestId("room-name").fill("Lounge");
    await page.getByTestId("room-name").press("Tab");

    await page.getByTestId("pin-kind-tool").selectOption("plant");
    const pinAt = at(0.45, 0.47);
    await page.mouse.click(pinAt.x, pinAt.y);
    await page.getByTestId("pin-name").fill("Boston fern");
    await page.getByTestId("pin-name").press("Tab");
    await expect(page.getByTestId("pin-room")).toHaveValue("lounge");
    await page.getByTestId("bind-moisture").fill("sensor.example_fern_moisture");
    await page.getByTestId("bind-moisture").press("Tab");
    await page.getByTestId("cal-dry").fill("12");
    await page.getByTestId("cal-dry").press("Tab");
    await page.getByTestId("photo-file").setInputFiles({ name: "fern.png", mimeType: "image/png", buffer: makePng(300, 200) });
    await expect(page.getByText("photos/boston-fern.png")).toBeVisible();

    // Undo and redo the calibration edit through the toolbar.
    const cfg = () => page.evaluate(() => (window as unknown as { __hearthmap: { config: () => Config } }).__hearthmap.config());
    await page.getByTestId("undo").click();
    expect((await cfg()).pins[0]?.photo).toBeUndefined();
    await page.getByTestId("redo").click();
    expect((await cfg()).pins[0]?.photo).toBe("boston-fern");

    await expect(page.getByTestId("issue-badge")).not.toHaveText(/error/);
    const [download] = await Promise.all([page.waitForEvent("download"), page.getByTestId("export-zip").click()]);
    const zip = unzipSync(new Uint8Array(readFileSync(await download.path())));
    expect(Object.keys(zip).sort()).toEqual(["config.json", "photos/boston-fern.png"]);
    const config = JSON.parse(strFromU8(zip["config.json"]!)) as Config;
    expect(validateSchema(config), JSON.stringify(validateSchema.errors)).toBe(true);
    expect(config.rooms).toHaveLength(1);
    expect(config.rooms[0]).toMatchObject({ id: "lounge", name: "Lounge" });
    expect(config.pins[0]).toMatchObject({
      kind: "plant",
      name: "Boston fern",
      room: "lounge",
      entities: { moisture: "sensor.example_fern_moisture" },
      calibration: { dry: 12, wet: 100 },
      photo: "boston-fern",
    });
    expect(pngSize(zip["photos/boston-fern.png"]!)).toEqual({ width: 128, height: 128 });

    // Autosave keeps the config across a reload (photos are not autosaved, by design).
    await page.reload();
    expect((await cfg()).pins[0]?.name).toBe("Boston fern");
  });

  test("refuses to export while there are errors, and points at them", async ({ page }) => {
    await fresh(page);
    await page.getByTestId("export-zip").click();
    await expect(page.getByTestId("message")).toContainText("Export refused");
    await expect(page.getByTestId("issue-list")).toContainText("draw at least one room");
  });

  test("never stores or shows a typed Home Assistant token", async ({ page }) => {
    const token = "planted-e2e-token-7d1b";
    await fresh(page);
    await page.getByTestId("load-example").click();
    await page.getByTestId("tab-entities").click();
    await page.getByText("Fetch live from Home Assistant").click();
    await page.getByPlaceholder("http://homeassistant.local:8123").fill("http://127.0.0.1:9");
    await page.getByPlaceholder("Long-lived access token").fill(token);
    const consoleText: string[] = [];
    page.on("console", (m) => consoleText.push(m.text()));
    await page.getByRole("button", { name: "Fetch entities" }).click();
    await expect(page.getByTestId("message")).toContainText("could not reach");
    await page.waitForTimeout(500); // past the autosave debounce
    const stored = await page.evaluate(() => JSON.stringify({ ...localStorage }));
    expect(stored).toContain("sensor.example_");
    expect(stored).not.toContain(token);
    expect(await page.getByTestId("message").textContent()).not.toContain(token);
    expect(consoleText.join("\n")).not.toContain(token);
  });

  test("loads entities from a states export and filters them", async ({ page }) => {
    await fresh(page);
    await page.getByTestId("load-example").click();
    await page.getByTestId("tab-entities").click();
    await page.getByText("Paste a states export").click();
    await page.getByTestId("states-paste").fill(
      JSON.stringify([
        { entity_id: "sensor.example_a_temperature", state: "21", attributes: { device_class: "temperature" } },
        { entity_id: "sensor.example_living_temperature", state: "22", attributes: { device_class: "temperature" } },
        { entity_id: "sensor.example_b_moisture", state: "40", attributes: { device_class: "moisture" } },
      ]),
    );
    await page.getByTestId("states-paste-use").click();
    await page.getByTestId("entity-class").selectOption("temperature");
    await expect(page.getByTestId("entity-list").locator("li")).toHaveCount(2);
    await page.getByTestId("entity-search").fill("living");
    await expect(page.getByTestId("entity-list").locator("li")).toHaveCount(1);
    await expect(page.getByTestId("entity-list")).toContainText("Bound:");
  });

  test("screenshots of the fictional flat", async ({ page }) => {
    test.skip(!process.env.SCREENSHOTS, "set SCREENSHOTS=1 to regenerate docs/img");
    const out = resolve(repo, "docs", "img");
    mkdirSync(out, { recursive: true });
    await fresh(page);
    await page.getByTestId("load-example").click();
    await page.getByTestId("tab-rooms").click();
    await page.getByRole("button", { name: "Hallway" }).click();
    await page.screenshot({ path: resolve(out, "editor-flat.png") });
    await page.getByTestId("open-preview").click();
    await page.getByTestId("preview-layer-soil").click();
    await page.locator(".preview-controls input[type=file]").setInputFiles(resolve(repo, "examples/flat/readings/thirsty-many.json"));
    await page.waitForTimeout(300);
    await page.getByTestId("preview-canvas").screenshot({ path: resolve(out, "editor-preview.png") });
  });
});

test("draws a room by touch @touch", async ({ page }) => {
  await fresh(page);
  const b = await canvasBox(page);
  await page.getByTestId("tool-room").tap();
  for (const [fx, fy] of [
    [0.3, 0.3],
    [0.6, 0.3],
    [0.6, 0.6],
  ] as const) {
    await page.touchscreen.tap(b.x + b.width * fx, b.y + b.height * fy);
  }
  // Tap the first corner again to close the room.
  await page.touchscreen.tap(b.x + b.width * 0.3, b.y + b.height * 0.3);
  const rooms = await page.evaluate(() => (window as unknown as { __hearthmap: { config: () => Config } }).__hearthmap.config().rooms);
  expect(rooms).toHaveLength(1);
  expect(rooms[0]?.polygon).toHaveLength(3);
});
