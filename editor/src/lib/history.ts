// Undo/redo over immutable snapshots. A drag is one step: begin() remembers the state before
// it, replace() updates the present without recording, commit() records one entry for the
// whole gesture (or none if nothing changed).

export interface History<T> {
  readonly past: readonly T[];
  readonly present: T;
  readonly future: readonly T[];
  /** The state a gesture started from, while one is open. */
  readonly pending?: T;
}

export const HISTORY_LIMIT = 200;

export function createHistory<T>(present: T): History<T> {
  return { past: [], present, future: [] };
}

/** Record `next` as a new step. Clears redo. A no-op when `next` is the present object. */
export function push<T>(h: History<T>, next: T): History<T> {
  if (next === h.present) return h;
  const past = [...h.past, h.present].slice(-HISTORY_LIMIT);
  return { past, present: next, future: [] };
}

export function begin<T>(h: History<T>): History<T> {
  return h.pending !== undefined ? h : { ...h, pending: h.present };
}

/** Change the present without recording (inside a gesture, or for state nobody undoes). */
export function replace<T>(h: History<T>, next: T): History<T> {
  return { ...h, present: next };
}

export function commit<T>(h: History<T>): History<T> {
  if (h.pending === undefined) return h;
  const { pending, ...rest } = h;
  if (pending === h.present) return rest;
  return { past: [...h.past, pending].slice(-HISTORY_LIMIT), present: h.present, future: [] };
}

export function canUndo<T>(h: History<T>): boolean {
  return h.past.length > 0;
}

export function canRedo<T>(h: History<T>): boolean {
  return h.future.length > 0;
}

export function undo<T>(h: History<T>): History<T> {
  const closed = commit(h);
  const prev = closed.past[closed.past.length - 1];
  if (prev === undefined) return closed;
  return { past: closed.past.slice(0, -1), present: prev, future: [closed.present, ...closed.future] };
}

export function redo<T>(h: History<T>): History<T> {
  const closed = commit(h);
  const [next, ...future] = closed.future;
  if (next === undefined) return closed;
  return { past: [...closed.past, closed.present], present: next, future };
}
