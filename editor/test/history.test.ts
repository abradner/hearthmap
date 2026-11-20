import { describe, expect, it } from "vitest";
import { HISTORY_LIMIT, begin, canRedo, canUndo, commit, createHistory, push, redo, replace, undo } from "../src/lib/history";

describe("history", () => {
  it("undoes and redoes pushes; a push clears redo", () => {
    let h = push(push(createHistory("a"), "b"), "c");
    h = undo(h);
    expect(h.present).toBe("b");
    expect(canRedo(h)).toBe(true);
    h = redo(h);
    expect(h.present).toBe("c");
    h = push(undo(h), "d");
    expect([h.present, canRedo(h)]).toEqual(["d", false]);
  });
  it("ignores undo/redo at the ends and a push of the same object", () => {
    const h = createHistory({ n: 1 });
    expect(undo(h).present).toBe(h.present);
    expect(redo(h).present).toBe(h.present);
    expect(push(h, h.present)).toBe(h);
    expect(canUndo(h)).toBe(false);
  });
  it("records a whole gesture as one step, and nothing if it changed nothing", () => {
    let h = begin(createHistory("start"));
    h = replace(h, "drag1");
    h = replace(h, "drag2");
    h = begin(h); // nested begin keeps the original start
    h = commit(h);
    expect(h.past).toEqual(["start"]);
    expect(undo(h).present).toBe("start");
    const idle = commit(begin(createHistory("x")));
    expect(idle.past).toEqual([]);
    expect(idle.pending).toBeUndefined();
  });
  it("undo during an open gesture closes it first", () => {
    let h = replace(begin(createHistory("a")), "b");
    h = undo(h);
    expect([h.present, h.future]).toEqual(["a", ["b"]]);
  });
  it("keeps at most HISTORY_LIMIT steps", () => {
    let h = createHistory(0);
    for (let i = 1; i <= HISTORY_LIMIT + 50; i++) h = push(h, i);
    expect(h.past).toHaveLength(HISTORY_LIMIT);
    expect(h.past[0]).toBe(50);
  });
});
