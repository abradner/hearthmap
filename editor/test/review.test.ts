// Unit-level regressions for the first review round (import guard, stale folder photos, names).
import { describe, expect, it } from "vitest";
import { loadConfig, AUTOSAVE_KEY } from "../src/lib/autosave";
import { staleExportedPhotos } from "../src/lib/files";
import { NAME_MAX, codePointLength } from "../src/lib/ids";
import { checkImportedConfig, structuralIssue, schemaIssues } from "../src/lib/validate";
import { example } from "./helpers";

describe("checkImportedConfig", () => {
  it("accepts the example", () => {
    expect(checkImportedConfig(example())).toMatchObject({ ok: true });
  });
  it.each([
    ["null canvas", { version: 1, canvas: null, rooms: [], pins: [] }],
    ["not an object", [1, 2]],
    ["rooms not an array", { ...example(), rooms: {} }],
    ["polygon point not a pair", { ...example(), rooms: [{ id: "a", name: "A", polygon: [[0, 0], [1], [1, 1]] }] }],
    ["pin position missing", { ...example(), pins: [{ ...example().pins[0], position: undefined }] }],
    ["wrong version", { ...example(), version: 2 }],
    ["schema-invalid but structurally fine (unknown field)", { ...example(), extra: true }],
    ["schema-invalid but structurally fine (bad room id)", { ...example(), rooms: [{ ...example().rooms[0], id: "Bad Id" }] }],
  ])("rejects %s", (_label, data) => {
    const r = checkImportedConfig(data);
    expect(r.ok).toBe(false);
  });
  it("allows editable semantic errors the schema cannot see (duplicate ids)", () => {
    const c = example();
    c.rooms[1]!.id = c.rooms[0]!.id;
    expect(checkImportedConfig(c)).toMatchObject({ ok: true });
  });
});

describe("autosave load uses the structural guard", () => {
  const store = (text: string) => ({ getItem: (k: string) => (k === AUTOSAVE_KEY ? text : null), setItem: () => {}, removeItem: () => {} });
  it("refuses a saved config the editor cannot render, keeps a blank project", () => {
    expect(loadConfig(store('{"version":1,"canvas":null,"rooms":[],"pins":[]}'))).toBeUndefined();
    expect(structuralIssue({ version: 1, canvas: { width: 1280, height: 800, orientation: "landscape" }, rooms: [], pins: [] })).toBeUndefined();
    expect(loadConfig(store('{"version":1,"canvas":{"width":1280,"height":800,"orientation":"landscape"},"rooms":[],"pins":[]}'))).toBeDefined();
  });
});

describe("staleExportedPhotos", () => {
  const files = [{ name: "config.json", bytes: new Uint8Array() }, { name: "photos/basil.png", bytes: new Uint8Array() }];
  it("lists photos the editor would have written that the new export does not contain", () => {
    expect(staleExportedPhotos(["basil.png", "monstera.png", "old-fern.png"], files, ["basil"])).toEqual(["monstera.png", "old-fern.png"]);
  });
  it("never lists files outside the editor's own name pattern", () => {
    expect(staleExportedPhotos(["Holiday.png", "notes.txt", "basil.jpg", ".DS_Store", "a/b.png", "x.png.bak", "-x.png"], files, ["basil"])).toEqual([]);
  });
  it("never lists a photo the config still references, even when its image is not in memory", () => {
    // After a reload the config is restored but photos are not, so the export omits monstera.png;
    // it is still the config's photo and must stay on disk.
    const referenced = ["basil", "monstera"];
    expect(staleExportedPhotos(["basil.png", "monstera.png", "old-fern.png"], files, referenced)).toEqual(["old-fern.png"]);
  });
  it("lists nothing when the export holds every photo there", () => {
    expect(staleExportedPhotos(["basil.png"], files, ["basil"])).toEqual([]);
  });
});

describe("names count Unicode code points (schema maxLength and core agree)", () => {
  const astral = "\u{1F33F}";
  it("counts an astral character as one", () => {
    expect(codePointLength(astral.repeat(48))).toBe(48);
    expect(astral.repeat(48).length).toBe(96); // what a UTF-16 count would say
    expect(NAME_MAX).toBe(48);
  });
  it("the schema accepts 48 astral code points and refuses 49", () => {
    const ok = example();
    ok.rooms[0]!.name = astral.repeat(48);
    expect(schemaIssues(ok)).toEqual([]);
    const tooLong = example();
    tooLong.rooms[0]!.name = astral.repeat(49);
    expect(schemaIssues(tooLong).map((i) => i.path)).toContain("/rooms/0/name");
  });
});
