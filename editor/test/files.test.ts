import { describe, expect, it } from "vitest";
import { strFromU8, strToU8, unzipSync, zipSync } from "fflate";
import { exportFiles, openFiles, photoIdFromName, zipFiles } from "../src/lib/files";
import { example } from "./helpers";

const png = (n: number) => new Uint8Array([0x89, 0x50, 0x4e, 0x47, n]);

describe("exportFiles", () => {
  it("writes config.json and only the photos pins reference; lists referenced ids without images", () => {
    const photos = new Map([
      ["basil", png(1)],
      ["monstera", png(2)],
      ["orphan", png(3)],
    ]);
    const { files, missing } = exportFiles(example(), photos);
    expect(files.map((f) => f.name)).toEqual(["config.json", "photos/basil.png", "photos/monstera.png"]);
    expect(missing).toEqual(["fiddle-leaf-fig", "snake-plant", "tomato"]);
    expect(JSON.parse(strFromU8(files[0]!.bytes))).toEqual(example());
  });
});

describe("zipFiles", () => {
  it("is deterministic and unzips to the same files", () => {
    const { files } = exportFiles(example(), new Map([["basil", png(1)]]));
    const a = zipFiles(files);
    expect(zipFiles(files)).toEqual(a);
    const back = unzipSync(a);
    expect(Object.keys(back).sort()).toEqual(["config.json", "photos/basil.png"]);
    expect(back["photos/basil.png"]).toEqual(png(1));
  });
});

describe("openFiles", () => {
  it("opens a config with loose images (raw, keyed by file name as photo id)", () => {
    const o = openFiles([
      { name: "config.json", bytes: strToU8(JSON.stringify(example())) },
      { name: "basil.png", bytes: png(1) },
      { name: "Bad Name.png", bytes: png(2) },
      { name: "notes.txt", bytes: strToU8("x") },
    ]);
    expect(o.config).toEqual(example());
    expect([...o.images.keys()]).toEqual(["basil"]);
    expect(o.ignored).toEqual(["Bad Name.png"]);
  });
  it("opens a zip, preferring config.json over other JSON and skipping folders and __MACOSX", () => {
    const zip = zipSync({
      "flat/readings.json": strToU8('{"other": true}'),
      "flat/config.json": strToU8(JSON.stringify(example())),
      "flat/photos/": new Uint8Array(),
      "flat/photos/tomato.jpg": png(4),
      "__MACOSX/flat/photos/._tomato.jpg": png(5),
    });
    const o = openFiles([{ name: "flat.zip", bytes: zip }]);
    expect(o.config).toEqual(example());
    expect(o.configName).toBe("flat/config.json");
    expect([...o.images.keys()]).toEqual(["tomato"]);
  });
  it("throws on a config that is not JSON, and returns no config when there is none", () => {
    expect(() => openFiles([{ name: "config.json", bytes: strToU8("{") }])).toThrow(/not valid JSON/);
    expect(openFiles([{ name: "a.png", bytes: png(1) }]).config).toBeUndefined();
  });
  it("photoIdFromName", () => {
    expect(photoIdFromName("photos/fiddle-leaf-fig.png")).toBe("fiddle-leaf-fig");
    expect(photoIdFromName("x/Basil.PNG")).toBeUndefined();
    expect(photoIdFromName("basil.gif")).toBeUndefined();
  });
});
