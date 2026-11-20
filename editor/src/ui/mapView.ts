// The map editor canvas: backdrop, rooms, pins, and the pointer gestures that edit them.
// Pointer Events cover mouse, trackpad, pen and touch alike. Two fingers pan and pinch-zoom;
// a trackpad pinch arrives as a ctrl+wheel event and zooms; a plain wheel/two-finger scroll pans.

import { MAX_VERTICES, defaultMapArea } from "../lib/defaults";
import { bounds, dist, mapToWorld, nearestEdge, pointInPolygon, snap, worldToMap, type Size, type Vec } from "../lib/geometry";
import * as ops from "../lib/ops";
import { issueTarget } from "../lib/validate";
import type { Config, MapPoint } from "../lib/types";
import type { Store } from "./store";

const KIND_COLOUR: Record<string, string> = {
  temperature: "#e5484d",
  humidity: "#3b82f6",
  soil: "#a16207",
  air_quality: "#a855f7",
  plant: "#30a46c",
};
const KIND_LETTER: Record<string, string> = { temperature: "T", humidity: "H", soil: "S", air_quality: "A", plant: "P" };

type Drag =
  | { kind: "vertex"; room: number; vertex: number }
  | { kind: "pin"; pin: number; offset: Vec }
  | { kind: "pan"; start: Vec; origin: { ox: number; oy: number } }
  | { kind: "pinch"; startDist: number; startScale: number; startMid: Vec; origin: { ox: number; oy: number } };

export function worldSize(config: Config): Size {
  const ma = config.canvas.map_area ?? defaultMapArea(config.canvas);
  return { width: ma.width, height: ma.height };
}

export class MapView {
  readonly canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private pointers = new Map<number, Vec>();
  private drag: Drag | undefined;
  private hover: Vec | undefined;
  private guides: { x?: number; y?: number } = {};
  private spaceHeld = false;
  private frame = 0;
  private fitted = false;

  constructor(
    private host: HTMLElement,
    private store: Store,
  ) {
    this.canvas = document.createElement("canvas");
    this.canvas.className = "map-canvas";
    this.canvas.setAttribute("aria-label", "Floor plan editor");
    this.canvas.tabIndex = 0;
    host.append(this.canvas);
    const ctx = this.canvas.getContext("2d");
    if (!ctx) throw new Error("no 2D canvas");
    this.ctx = ctx;
    new ResizeObserver(() => this.resize()).observe(host);
    this.canvas.addEventListener("pointerdown", (e) => this.onDown(e));
    this.canvas.addEventListener("pointermove", (e) => this.onMove(e));
    this.canvas.addEventListener("pointerup", (e) => this.onUp(e));
    this.canvas.addEventListener("pointercancel", (e) => this.onUp(e));
    this.canvas.addEventListener("pointerleave", () => {
      this.hover = undefined;
      this.request();
    });
    this.canvas.addEventListener("dblclick", (e) => this.onDouble(e));
    this.canvas.addEventListener("wheel", (e) => this.onWheel(e), { passive: false });
    window.addEventListener("keydown", (e) => {
      if (e.code === "Space" && !isTyping(e)) this.spaceHeld = true;
    });
    window.addEventListener("keyup", (e) => {
      if (e.code === "Space") this.spaceHeld = false;
    });
    store.subscribe(() => this.request());
    this.resize();
  }

  // ---- view transform ---------------------------------------------------------------------------

  private get size(): Size {
    return worldSize(this.store.config);
  }

  private toScreen(w: Vec): Vec {
    const v = this.store.state.view;
    return { x: w.x * v.scale + v.ox, y: w.y * v.scale + v.oy };
  }

  private toWorld(s: Vec): Vec {
    const v = this.store.state.view;
    return { x: (s.x - v.ox) / v.scale, y: (s.y - v.oy) / v.scale };
  }

  private local(e: PointerEvent | MouseEvent): Vec {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  fit(): void {
    const { width, height } = this.size;
    const cw = this.canvas.clientWidth || 800;
    const ch = this.canvas.clientHeight || 600;
    const scale = Math.min((cw - 48) / width, (ch - 48) / height);
    this.store.set({ view: { scale, ox: (cw - width * scale) / 2, oy: (ch - height * scale) / 2 } });
  }

  zoomBy(factor: number, at?: Vec): void {
    const v = this.store.state.view;
    const p = at ?? { x: this.canvas.clientWidth / 2, y: this.canvas.clientHeight / 2 };
    const scale = Math.min(40, Math.max(0.05, v.scale * factor));
    const k = scale / v.scale;
    this.store.set({ view: { scale, ox: p.x - (p.x - v.ox) * k, oy: p.y - (p.y - v.oy) * k } });
  }

  private resize(): void {
    const dpr = window.devicePixelRatio || 1;
    const w = this.host.clientWidth;
    const h = this.host.clientHeight;
    this.canvas.style.width = `${w}px`;
    this.canvas.style.height = `${h}px`;
    this.canvas.width = Math.max(1, Math.round(w * dpr));
    this.canvas.height = Math.max(1, Math.round(h * dpr));
    if (!this.fitted && w > 0 && h > 0) {
      this.fitted = true;
      this.fit();
    }
    this.request();
  }

  private request(): void {
    if (this.frame) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.draw();
    });
  }

  // ---- hit testing ------------------------------------------------------------------------------

  private tolerance(e?: PointerEvent): number {
    return e?.pointerType === "touch" ? 20 : 10;
  }

  private worldOf(p: MapPoint): Vec {
    return mapToWorld(p, this.size);
  }

  private hitPin(s: Vec, tol: number): number {
    const pins = this.store.config.pins;
    for (let i = pins.length - 1; i >= 0; i--) {
      if (dist(this.toScreen(this.worldOf((pins[i] as { position: MapPoint }).position)), s) <= tol + 6) return i;
    }
    return -1;
  }

  private hitVertex(room: number, s: Vec, tol: number): number {
    const r = this.store.config.rooms[room];
    if (!r) return -1;
    let best = -1;
    let bestD = tol;
    r.polygon.forEach((p, i) => {
      const d = dist(this.toScreen(this.worldOf(p)), s);
      if (d <= bestD) {
        best = i;
        bestD = d;
      }
    });
    return best;
  }

  /** Edge midpoint handles of the selected room: index of the edge whose midpoint was hit. */
  private hitMidpoint(room: number, s: Vec, tol: number): number {
    const r = this.store.config.rooms[room];
    if (!r) return -1;
    for (let i = 0; i < r.polygon.length; i++) {
      const a = this.worldOf(r.polygon[i] as MapPoint);
      const b = this.worldOf(r.polygon[(i + 1) % r.polygon.length] as MapPoint);
      if (dist(this.toScreen({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 }), s) <= tol) return i;
    }
    return -1;
  }

  private hitRoom(w: Vec): number {
    const p = worldToMap(w, this.size);
    const rooms = this.store.config.rooms;
    // Topmost drawn (last) first, matching what the eye sees under the pointer.
    for (let i = rooms.length - 1; i >= 0; i--) {
      if (pointInPolygon((rooms[i] as { polygon: MapPoint[] }).polygon, p)) return i;
    }
    return -1;
  }

  private snapped(w: Vec, exclude?: { room: number; vertex: number }, e?: PointerEvent): Vec {
    this.guides = {};
    if (!this.store.state.snapping || e?.altKey) return w;
    const tol = this.tolerance(e) / this.store.state.view.scale;
    const vertices: Vec[] = [];
    this.store.config.rooms.forEach((r, ri) =>
      r.polygon.forEach((p, vi) => {
        if (!(exclude && exclude.room === ri && exclude.vertex === vi)) vertices.push(this.worldOf(p));
      }),
    );
    const axis: Vec[] = [];
    const draft = this.store.state.draft;
    if (exclude) {
      const poly = this.store.config.rooms[exclude.room]?.polygon ?? [];
      const n = poly.length;
      axis.push(this.worldOf(poly[(exclude.vertex + n - 1) % n] as MapPoint), this.worldOf(poly[(exclude.vertex + 1) % n] as MapPoint));
    } else if (draft.length) {
      axis.push(this.worldOf(draft[draft.length - 1] as MapPoint), this.worldOf(draft[0] as MapPoint));
    }
    for (const d of draft) vertices.push(this.worldOf(d));
    const r = snap(w, vertices, axis, tol);
    this.guides = r.guides;
    return r.point;
  }

  // ---- gestures ---------------------------------------------------------------------------------

  private onDown(e: PointerEvent): void {
    this.canvas.focus();
    this.canvas.setPointerCapture(e.pointerId);
    const s = this.local(e);
    this.pointers.set(e.pointerId, s);
    if (this.pointers.size === 2) {
      // A second finger turns any gesture into pan + pinch. Close an edit gesture first.
      if (this.drag && (this.drag.kind === "vertex" || this.drag.kind === "pin")) this.store.endGesture();
      const [a, b] = [...this.pointers.values()] as [Vec, Vec];
      const v = this.store.state.view;
      this.drag = { kind: "pinch", startDist: dist(a, b), startScale: v.scale, startMid: mid(a, b), origin: { ox: v.ox, oy: v.oy } };
      return;
    }
    if (this.pointers.size > 2) return;

    const st = this.store.state;
    const w = this.toWorld(s);
    const tol = this.tolerance(e);
    if (e.button === 1 || st.tool === "pan" || this.spaceHeld) {
      this.startPan(s);
      return;
    }
    if (st.tool === "room") {
      this.addDraftVertex(w, e, tol);
      return;
    }
    if (st.tool === "pin") {
      const p = worldToMap(w, this.size);
      if (this.store.edit((c) => ops.addPin(c, st.pinKind, p))) {
        this.store.set({ selection: { type: "pin", index: this.store.config.pins.length - 1 }, tool: "select", sidebarTab: "item" });
      }
      return;
    }
    // Select tool.
    const sel = st.selection;
    if (sel?.type === "room") {
      const v = this.hitVertex(sel.index, s, tol);
      if (v >= 0) {
        this.store.set({ selection: { type: "room", index: sel.index, vertex: v } });
        this.store.beginGesture();
        this.drag = { kind: "vertex", room: sel.index, vertex: v };
        return;
      }
      const m = this.hitMidpoint(sel.index, s, tol);
      if (m >= 0) {
        const room = this.store.config.rooms[sel.index];
        if (room && room.polygon.length < MAX_VERTICES) {
          const a = room.polygon[m] as MapPoint;
          const b = room.polygon[(m + 1) % room.polygon.length] as MapPoint;
          this.store.beginGesture();
          this.store.live((c) => ops.insertVertex(c, sel.index, m + 1, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]));
          this.store.set({ selection: { type: "room", index: sel.index, vertex: m + 1 } });
          this.drag = { kind: "vertex", room: sel.index, vertex: m + 1 };
          return;
        }
      }
    }
    const pin = this.hitPin(s, tol);
    if (pin >= 0) {
      const pw = this.worldOf((this.store.config.pins[pin] as { position: MapPoint }).position);
      this.store.set({ selection: { type: "pin", index: pin }, sidebarTab: "item" });
      this.store.beginGesture();
      this.drag = { kind: "pin", pin, offset: { x: pw.x - w.x, y: pw.y - w.y } };
      return;
    }
    const room = this.hitRoom(w);
    if (room >= 0) {
      if (!(sel?.type === "room" && sel.index === room)) this.store.set({ selection: { type: "room", index: room }, sidebarTab: "item" });
      this.startPan(s);
      return;
    }
    this.store.set({ selection: undefined });
    this.startPan(s);
  }

  private startPan(s: Vec): void {
    const v = this.store.state.view;
    this.drag = { kind: "pan", start: s, origin: { ox: v.ox, oy: v.oy } };
  }

  private addDraftVertex(w: Vec, e: PointerEvent, tol: number): void {
    const draft = this.store.state.draft;
    const first = draft[0];
    if (first && draft.length >= 3 && dist(this.toScreen(this.worldOf(first)), this.local(e)) <= tol) {
      this.finishDraft();
      return;
    }
    if (draft.length >= MAX_VERTICES) return;
    const p = worldToMap(this.snapped(w, undefined, e), this.size);
    const last = draft[draft.length - 1];
    if (last && last[0] === p[0] && last[1] === p[1]) return;
    this.store.set({ draft: [...draft, p] });
  }

  finishDraft(): void {
    const draft = this.store.state.draft;
    if (draft.length < 3) {
      this.store.set({ message: { text: "A room needs at least three corners", kind: "error" } });
      return;
    }
    if (this.store.edit((c) => ops.addRoom(c, draft), { draft: [] })) {
      this.store.set({ selection: { type: "room", index: this.store.config.rooms.length - 1 }, tool: "select", sidebarTab: "item" });
    }
  }

  private onMove(e: PointerEvent): void {
    const s = this.local(e);
    if (this.pointers.has(e.pointerId)) this.pointers.set(e.pointerId, s);
    this.hover = s;
    const d = this.drag;
    if (!d) {
      if (this.store.state.tool === "room") this.snapped(this.toWorld(s), undefined, e);
      this.request();
      return;
    }
    if (d.kind === "pinch") {
      const [a, b] = [...this.pointers.values()] as [Vec, Vec];
      if (!a || !b) return;
      const scale = Math.min(40, Math.max(0.05, d.startScale * (dist(a, b) / Math.max(1, d.startDist))));
      const m = mid(a, b);
      const k = scale / d.startScale;
      this.store.set({
        view: { scale, ox: m.x - (d.startMid.x - d.origin.ox) * k, oy: m.y - (d.startMid.y - d.origin.oy) * k },
      });
      return;
    }
    if (d.kind === "pan") {
      this.store.set({ view: { ...this.store.state.view, ox: d.origin.ox + s.x - d.start.x, oy: d.origin.oy + s.y - d.start.y } });
      return;
    }
    const w = this.toWorld(s);
    if (d.kind === "vertex") {
      const p = worldToMap(this.snapped(w, { room: d.room, vertex: d.vertex }, e), this.size);
      this.store.live((c) => ops.moveVertex(c, d.room, d.vertex, p));
    } else {
      const p = worldToMap({ x: w.x + d.offset.x, y: w.y + d.offset.y }, this.size);
      this.store.live((c) => ops.movePin(c, d.pin, p));
    }
  }

  private onUp(e: PointerEvent): void {
    this.pointers.delete(e.pointerId);
    const d = this.drag;
    if (d && (d.kind === "vertex" || d.kind === "pin")) this.store.endGesture();
    if (this.pointers.size === 0) {
      this.drag = undefined;
      this.guides = {};
    } else if (d?.kind === "pinch") {
      const rest = [...this.pointers.values()][0] as Vec;
      this.startPan(rest);
    }
    this.request();
  }

  private onDouble(e: MouseEvent): void {
    const sel = this.store.state.selection;
    if (this.store.state.tool !== "select" || sel?.type !== "room") return;
    const room = this.store.config.rooms[sel.index];
    if (!room) return;
    const w = this.toWorld(this.local(e));
    const edge = nearestEdge(room.polygon.map((p) => this.worldOf(p)), w);
    if (edge.distance * this.store.state.view.scale > 12) return;
    const p = worldToMap(edge.point, this.size);
    if (this.store.edit((c) => ops.insertVertex(c, sel.index, edge.edge + 1, p))) {
      this.store.set({ selection: { type: "room", index: sel.index, vertex: edge.edge + 1 } });
    }
  }

  private onWheel(e: WheelEvent): void {
    e.preventDefault();
    const s = this.local(e);
    if (e.ctrlKey || e.metaKey) {
      this.zoomBy(Math.exp(-e.deltaY * 0.01), s);
    } else {
      const v = this.store.state.view;
      this.store.set({ view: { ...v, ox: v.ox - e.deltaX, oy: v.oy - e.deltaY } });
    }
  }

  // ---- drawing ----------------------------------------------------------------------------------

  private draw(): void {
    const ctx = this.ctx;
    const dpr = window.devicePixelRatio || 1;
    const st = this.store.state;
    const config = this.store.config;
    const { width, height } = this.size;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = "#0d1013";
    ctx.fillRect(0, 0, this.canvas.clientWidth, this.canvas.clientHeight);

    const tl = this.toScreen({ x: 0, y: 0 });
    const scale = st.view.scale;
    ctx.fillStyle = "#12161b";
    ctx.fillRect(tl.x, tl.y, width * scale, height * scale);
    if (st.backdrop) {
      ctx.globalAlpha = st.backdrop.opacity;
      ctx.drawImage(st.backdrop.image, tl.x, tl.y, width * scale, height * scale);
      ctx.globalAlpha = 1;
    }
    // A light 10% grid so a blank project has a sense of scale.
    ctx.strokeStyle = "rgba(255,255,255,0.05)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let k = 1; k < 10; k++) {
      ctx.moveTo(tl.x + (width * scale * k) / 10, tl.y);
      ctx.lineTo(tl.x + (width * scale * k) / 10, tl.y + height * scale);
      ctx.moveTo(tl.x, tl.y + (height * scale * k) / 10);
      ctx.lineTo(tl.x + width * scale, tl.y + (height * scale * k) / 10);
    }
    ctx.stroke();
    ctx.strokeStyle = "#3a424b";
    ctx.strokeRect(tl.x, tl.y, width * scale, height * scale);

    const bad = new Set<string>();
    for (const i of st.issues) {
      if (i.severity !== "error") continue;
      const t = issueTarget(i.path);
      if (t.kind === "room" || t.kind === "pin") bad.add(`${t.kind}:${t.index}`);
    }

    config.rooms.forEach((room, ri) => {
      const pts = room.polygon.map((p) => this.toScreen(this.worldOf(p)));
      const selected = st.selection?.type === "room" && st.selection.index === ri;
      ctx.beginPath();
      pts.forEach((p, k) => (k ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
      ctx.closePath();
      ctx.fillStyle = room.outdoor ? "rgba(48,164,108,0.10)" : selected ? "rgba(230,237,243,0.10)" : "rgba(230,237,243,0.04)";
      ctx.fill();
      ctx.setLineDash(room.outdoor ? [6, 4] : []);
      ctx.lineWidth = selected ? 3 : 2;
      ctx.strokeStyle = bad.has(`room:${ri}`) ? "#e5484d" : selected ? "#e6edf3" : "#6b7a88";
      ctx.stroke();
      ctx.setLineDash([]);
      // Name at the room's top-left, as on the panel, so it does not sit under centred pins.
      const bb = bounds(room.polygon);
      const c = this.toScreen(this.worldOf([bb.minX, bb.minY]));
      ctx.fillStyle = "#9aa4af";
      ctx.font = "13px system-ui, sans-serif";
      ctx.textAlign = "left";
      ctx.textBaseline = "top";
      ctx.fillText(room.name + (room.outdoor ? " (outdoor)" : ""), c.x + 6, c.y + 5);
    });

    const sel = st.selection;
    if (sel?.type === "room" && st.tool === "select") {
      const room = config.rooms[sel.index];
      if (room) {
        const pts = room.polygon.map((p) => this.toScreen(this.worldOf(p)));
        // Midpoint handles: tap or drag one to insert a vertex (works on touch, no double-tap).
        if (room.polygon.length < MAX_VERTICES) {
          ctx.fillStyle = "rgba(230,237,243,0.5)";
          pts.forEach((a, k) => {
            const b = pts[(k + 1) % pts.length] as Vec;
            ctx.beginPath();
            ctx.arc((a.x + b.x) / 2, (a.y + b.y) / 2, 4, 0, Math.PI * 2);
            ctx.fill();
          });
        }
        pts.forEach((p, k) => {
          ctx.fillStyle = sel.vertex === k ? "#f5a524" : "#e6edf3";
          ctx.fillRect(p.x - 5, p.y - 5, 10, 10);
          ctx.strokeStyle = "#0d1013";
          ctx.lineWidth = 1.5;
          ctx.strokeRect(p.x - 5, p.y - 5, 10, 10);
        });
      }
    }

    config.pins.forEach((pin, pi) => {
      const p = this.toScreen(this.worldOf(pin.position));
      const selected = sel?.type === "pin" && sel.index === pi;
      ctx.beginPath();
      ctx.arc(p.x, p.y, 9, 0, Math.PI * 2);
      ctx.fillStyle = KIND_COLOUR[pin.kind] ?? "#e6edf3";
      ctx.fill();
      ctx.lineWidth = selected ? 3 : 2;
      ctx.strokeStyle = bad.has(`pin:${pi}`) ? "#ff8a8a" : selected ? "#ffffff" : "#0d1013";
      ctx.stroke();
      ctx.fillStyle = "#0d1013";
      ctx.font = "bold 10px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(KIND_LETTER[pin.kind] ?? "?", p.x, p.y + 0.5);
      if (selected || scale > 0.9) {
        ctx.fillStyle = "#e6edf3";
        ctx.font = "12px system-ui, sans-serif";
        ctx.textAlign = "left";
        ctx.fillText(pin.name, p.x + 13, p.y);
      }
    });

    // Room being drawn.
    if (st.tool === "room") {
      const pts = st.draft.map((p) => this.toScreen(this.worldOf(p)));
      const cursor = this.hover ? this.toScreen(this.snapped(this.toWorld(this.hover))) : undefined;
      ctx.strokeStyle = "#f5a524";
      ctx.lineWidth = 2;
      ctx.beginPath();
      pts.forEach((p, k) => (k ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
      if (cursor && pts.length) ctx.lineTo(cursor.x, cursor.y);
      ctx.stroke();
      pts.forEach((p, k) => {
        ctx.fillStyle = k === 0 && pts.length >= 3 ? "#30a46c" : "#f5a524";
        ctx.beginPath();
        ctx.arc(p.x, p.y, k === 0 && pts.length >= 3 ? 7 : 4, 0, Math.PI * 2);
        ctx.fill();
      });
      if (cursor) {
        ctx.fillStyle = "#f5a524";
        ctx.beginPath();
        ctx.arc(cursor.x, cursor.y, 3, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    // Snap guides.
    ctx.strokeStyle = "rgba(245,165,36,0.6)";
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    if (this.guides.x !== undefined) {
      const x = this.toScreen({ x: this.guides.x, y: 0 }).x;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, this.canvas.clientHeight);
      ctx.stroke();
    }
    if (this.guides.y !== undefined) {
      const y = this.toScreen({ x: 0, y: this.guides.y }).y;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(this.canvas.clientWidth, y);
      ctx.stroke();
    }
    ctx.setLineDash([]);
  }
}

function mid(a: Vec, b: Vec): Vec {
  return { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
}

export function isTyping(e: KeyboardEvent): boolean {
  const t = e.target as HTMLElement | null;
  return !!t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT" || t.isContentEditable);
}

