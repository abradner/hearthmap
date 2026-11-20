import { afterEach, describe, expect, it, vi } from "vitest";
import { AUTOSAVE_KEY, saveConfig } from "../src/lib/autosave";
import { exportFiles } from "../src/lib/files";
import { fetchStates, statesUrl, type FetchLike } from "../src/lib/haClient";
import { strFromU8 } from "fflate";
import { example } from "./helpers";

// A planted secret: if it shows up anywhere it must not, a test below goes red.
const TOKEN = "planted-token-3f9c1e7a-do-not-store";

function respond(status: number, body: unknown): FetchLike {
  return vi.fn(() => Promise.resolve({ ok: status >= 200 && status < 300, status, json: () => Promise.resolve(body) }));
}

afterEach(() => vi.restoreAllMocks());

describe("statesUrl", () => {
  it("appends /api/states, keeping a path prefix and dropping query and hash", () => {
    expect(statesUrl("http://ha.example.test:8123")).toBe("http://ha.example.test:8123/api/states");
    expect(statesUrl(" https://ha.example.test/prefix/ ")).toBe("https://ha.example.test/prefix/api/states");
    expect(statesUrl("http://ha.example.test/?x=1#y")).toBe("http://ha.example.test/api/states");
  });
  it("refuses other schemes, garbage and credentials in the URL", () => {
    expect(() => statesUrl("ftp://ha.example.test")).toThrow(/http/);
    expect(() => statesUrl("not a url")).toThrow(/address/);
    expect(() => statesUrl("http://u:p@ha.example.test")).toThrow(/credentials/);
  });
});

describe("fetchStates", () => {
  it("sends the token as a bearer header, without cookies or referrer, and parses the states", async () => {
    const f = respond(200, [{ entity_id: "sensor.example_x", state: "1", attributes: { device_class: "temperature" } }]);
    const r = await fetchStates("http://ha.example.test", ` ${TOKEN} `, f);
    expect(r.entities).toEqual([{ entity_id: "sensor.example_x", device_class: "temperature", state: "1" }]);
    const [url, init] = (f as ReturnType<typeof vi.fn>).mock.calls[0] as [string, RequestInit];
    expect(url).toBe("http://ha.example.test/api/states");
    expect(init.headers).toEqual({ Authorization: `Bearer ${TOKEN}` });
    expect(init.credentials).toBe("omit");
    expect(init.referrerPolicy).toBe("no-referrer");
  });

  it("needs a token", async () => {
    await expect(fetchStates("http://ha.example.test", "  ", respond(200, []))).rejects.toThrow(/token/);
  });

  it.each([
    [401, /refused the token/],
    [403, /refused the token/],
    [500, /HTTP 500/],
  ])("reports HTTP %i without the token", async (status, msg) => {
    const err = await fetchStates("http://ha.example.test", TOKEN, respond(status, {})).catch((e: Error) => e.message);
    expect(err).toMatch(msg);
    expect(err).not.toContain(TOKEN);
  });

  it("reports a network/CORS failure and a non-list body without the token", async () => {
    const boom: FetchLike = () => Promise.reject(new TypeError(`Failed to fetch with ${TOKEN}`));
    const e1 = await fetchStates("http://ha.example.test", TOKEN, boom).catch((e: Error) => e.message);
    expect(e1).toMatch(/cors_allowed_origins/);
    expect(e1).not.toContain(TOKEN);
    const e2 = await fetchStates("http://ha.example.test", TOKEN, respond(200, { message: "x" })).catch((e: Error) => e.message);
    expect(e2).toMatch(/not a list/);
  });

  it("leaves no trace of the token in logs, autosave or the export", async () => {
    const logs: string[] = [];
    for (const m of ["log", "info", "warn", "error", "debug"] as const) {
      vi.spyOn(console, m).mockImplementation((...a: unknown[]) => void logs.push(a.map(String).join(" ")));
    }
    const store = new Map<string, string>();
    const storage = { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => void store.set(k, v), removeItem: (k: string) => void store.delete(k) };
    await fetchStates("http://ha.example.test", TOKEN, respond(200, []));
    await fetchStates("http://ha.example.test", TOKEN, respond(401, [])).catch(() => undefined);
    saveConfig(storage, example());
    const exported = exportFiles(example(), new Map()).files.map((f) => strFromU8(f.bytes)).join("\n");
    expect(logs.join("\n")).not.toContain(TOKEN);
    expect(store.get(AUTOSAVE_KEY)).toBeDefined();
    expect([...store.values()].join("\n")).not.toContain(TOKEN);
    expect(exported).not.toContain(TOKEN);
  });
});
