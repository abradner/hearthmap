// Draws a preview Scene onto a 1280x800 canvas, after the UI spec (colours, sizes, regions).
// Faithful enough to judge placement; not the device's pixels (the panel draws with LVGL).

import { heatColour, type Scene, type Severity } from "../preview/scene";
import type { LayerName } from "../lib/types";

const C = {
  bg: "#0d1013",
  card: "#161b21",
  track: "#2a3038",
  wall: "#4a535d",
  text: "#e6edf3",
  text2: "#9aa4af",
  muted: "#6b7280",
  danger: "#e5484d",
  warn: "#f5a524",
  ok: "#30a46c",
  stale: "#6b7280",
  garden: "#101a14",
};

const SEV: Record<Severity, string> = { ok: C.ok, warn: C.warn, danger: C.danger, stale: C.stale };
const TAB: Record<LayerName, string> = { temperature: "Temp", humidity: "Humidity", soil: "Soil", air: "Air" };
const FONT = "system-ui, -apple-system, Segoe UI, sans-serif";

function ring(ctx: CanvasRenderingContext2D, x: number, y: number, r: number, frac: number | undefined, colour: string, width: number) {
  ctx.lineCap = "round";
  ctx.lineWidth = width;
  ctx.strokeStyle = C.track;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  ctx.stroke();
  if (frac === undefined) return;
  ctx.strokeStyle = colour;
  ctx.beginPath();
  ctx.arc(x, y, r, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * Math.max(0.03, Math.min(1, frac)));
  ctx.stroke();
}

function leaf(ctx: CanvasRenderingContext2D, x: number, y: number, s: number, colour: string) {
  ctx.fillStyle = colour;
  ctx.beginPath();
  ctx.moveTo(x - s * 0.6, y + s * 0.6);
  ctx.quadraticCurveTo(x - s * 0.7, y - s * 0.7, x + s * 0.6, y - s * 0.6);
  ctx.quadraticCurveTo(x + s * 0.7, y + s * 0.7, x - s * 0.6, y + s * 0.6);
  ctx.fill();
}

function badge(ctx: CanvasRenderingContext2D, x: number, y: number, colour: string) {
  ctx.fillStyle = colour;
  ctx.beginPath();
  ctx.moveTo(x, y - 9);
  ctx.lineTo(x + 9, y + 7);
  ctx.lineTo(x - 9, y + 7);
  ctx.closePath();
  ctx.fill();
  ctx.fillStyle = C.bg;
  ctx.font = `bold 11px ${FONT}`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText("!", x, y + 1.5);
}

/**
 * Draw `scene`. `photos` are decoded plant images by pin index (optional); plants without one
 * get the placeholder leaf. `t` is seconds, for the pulse phase.
 */
export function drawPreview(ctx: CanvasRenderingContext2D, scene: Scene, photos: Map<number, CanvasImageSource>, t = 0): void {
  const W = scene.canvas.width;
  const H = scene.canvas.height;
  ctx.save();
  ctx.fillStyle = C.bg;
  ctx.fillRect(0, 0, W, H);

  // Heat field, coarse cells drawn smoothed by the canvas's image scaling.
  const { cols, rows, cells, band } = scene.heat;
  const img = new ImageData(cols, rows);
  for (let k = 0; k < cells.length; k++) {
    const v = cells[k];
    if (v === undefined) continue; // outside rooms, or outdoor (R10): transparent
    const [r, g, b] = v === null ? [22, 26, 31] : heatColour(v, band);
    img.data.set([r, g, b, 255], k * 4);
  }
  const off = document.createElement("canvas");
  off.width = cols;
  off.height = rows;
  off.getContext("2d")?.putImageData(img, 0, 0);
  const ma = scene.mapArea;
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(off, ma.x, ma.y, ma.width, ma.height);

  // Rooms: outdoor treatment, walls, names, pulses.
  const pulse = 0.35 + 0.65 * (0.5 + 0.5 * Math.sin((t * 2 * Math.PI) / 2.8));
  for (const room of scene.rooms) {
    ctx.beginPath();
    room.points.forEach((p, k) => (k ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
    ctx.closePath();
    if (room.outdoor) {
      ctx.fillStyle = C.garden;
      ctx.fill();
    }
    ctx.lineWidth = 2;
    ctx.strokeStyle = C.wall;
    ctx.stroke();
    if (room.pulse) {
      ctx.globalAlpha = pulse;
      ctx.lineWidth = 5;
      ctx.strokeStyle = SEV[room.pulse];
      ctx.stroke();
      ctx.globalAlpha = 1;
    }
    const minX = Math.min(...room.points.map((p) => p.x));
    const minY = Math.min(...room.points.map((p) => p.y));
    ctx.fillStyle = C.text2;
    ctx.font = `21px ${FONT}`;
    ctx.textAlign = "left";
    ctx.textBaseline = "top";
    ctx.fillText(room.name, minX + 10, minY + 8);
  }

  // Pins.
  for (const pin of scene.pins) {
    const colour = SEV[pin.severity];
    if (pin.kind === "plant" || pin.kind === "soil") {
      if (pin.severity === "danger") {
        const ph = (t % 1.6) / 1.6;
        ctx.globalAlpha = 1 - ph;
        ctx.lineWidth = 4;
        ctx.strokeStyle = C.danger;
        ctx.beginPath();
        ctx.arc(pin.x, pin.y, 26 + 26 * ph, 0, Math.PI * 2);
        ctx.stroke();
        ctx.globalAlpha = 1;
      }
      ctx.fillStyle = "#16261c";
      ctx.beginPath();
      ctx.arc(pin.x, pin.y, 22, 0, Math.PI * 2);
      ctx.fill();
      const photo = photos.get(pin.index);
      if (photo && pin.kind === "plant") {
        ctx.save();
        ctx.beginPath();
        ctx.arc(pin.x, pin.y, 19, 0, Math.PI * 2);
        ctx.clip();
        if (pin.severity === "stale") ctx.filter = "grayscale(1)";
        ctx.drawImage(photo, pin.x - 19, pin.y - 19, 38, 38);
        ctx.restore();
      } else {
        leaf(ctx, pin.x, pin.y, 12, pin.severity === "stale" ? "#4b5563" : "#3fa56b");
      }
      ring(ctx, pin.x, pin.y, 22, pin.moisture, colour, 5);
      if (pin.severity === "warn" || pin.severity === "danger") badge(ctx, pin.x + 20, pin.y - 20, colour);
    } else {
      ctx.beginPath();
      ctx.arc(pin.x, pin.y, 7, 0, Math.PI * 2);
      ctx.fillStyle = pin.severity === "stale" ? C.stale : C.text;
      ctx.fill();
      ctx.lineWidth = 3;
      ctx.strokeStyle = C.bg;
      ctx.stroke();
      ctx.fillStyle = pin.severity === "warn" || pin.severity === "danger" ? colour : pin.severity === "stale" ? C.muted : C.text;
      ctx.font = `500 22px ${FONT}`;
      ctx.textAlign = "left";
      ctx.textBaseline = "middle";
      ctx.fillText(pin.label, pin.x + 14, pin.y);
      if (pin.severity === "warn" || pin.severity === "danger") badge(ctx, pin.x + 8, pin.y - 14, colour);
    }
  }

  // Top bar: clock placeholder and layer tabs.
  ctx.fillStyle = C.bg;
  ctx.fillRect(ma.x + ma.width, 0, W - ma.x - ma.width, H);
  ctx.fillStyle = C.text;
  ctx.font = `500 30px ${FONT}`;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  ctx.fillText("09:00", 24, 32);
  let x = W - 16;
  ctx.font = `21px ${FONT}`;
  for (const layer of [...scene.enabledLayers].reverse()) {
    const label = TAB[layer];
    const w = ctx.measureText(label).width + 36;
    x -= w;
    ctx.fillStyle = layer === scene.layer ? C.text : "#1b2026";
    ctx.beginPath();
    ctx.roundRect(x, 10, w, 44, 22);
    ctx.fill();
    ctx.fillStyle = layer === scene.layer ? "#0d1013" : C.text2;
    ctx.textAlign = "center";
    ctx.fillText(label, x + w / 2, 32);
    x -= 12;
  }

  // HUD column.
  const hx = 936;
  let hy = 76;
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillStyle = C.text;
  ctx.font = `500 24px ${FONT}`;
  ctx.fillText("Thirsty plants", hx, hy);
  const tw = ctx.measureText("Thirsty plants ").width;
  ctx.fillStyle = C.danger;
  ctx.fillText(String(scene.hud.total), hx + tw, hy);
  hy += 44;
  for (const item of scene.hud.items) {
    ctx.fillStyle = C.card;
    ctx.beginPath();
    ctx.roundRect(hx, hy, 328, 84, 12);
    ctx.fill();
    const cx = hx + 44;
    const cy = hy + 42;
    ctx.fillStyle = "#16261c";
    ctx.beginPath();
    ctx.arc(cx, cy, 26, 0, Math.PI * 2);
    ctx.fill();
    const photo = photos.get(item.pin);
    if (photo) {
      ctx.save();
      ctx.beginPath();
      ctx.arc(cx, cy, 22, 0, Math.PI * 2);
      ctx.clip();
      ctx.drawImage(photo, cx - 22, cy - 22, 44, 44);
      ctx.restore();
    } else {
      leaf(ctx, cx, cy, 13, "#3fa56b");
    }
    ring(ctx, cx, cy, 26, item.percent / 100, SEV[item.severity], 5);
    ctx.fillStyle = C.text;
    ctx.font = `500 22px ${FONT}`;
    ctx.textAlign = "left";
    ctx.fillText(item.name, hx + 84, hy + 14, 170);
    ctx.fillStyle = C.text2;
    ctx.font = `21px ${FONT}`;
    ctx.fillText(item.room, hx + 84, hy + 44, 170);
    ctx.fillStyle = SEV[item.severity];
    ctx.font = `500 28px ${FONT}`;
    ctx.textAlign = "right";
    ctx.fillText(`${item.percent}%`, hx + 312, hy + 26);
    hy += 94;
  }
  if (scene.hud.overflow > 0) {
    ctx.fillStyle = C.text2;
    ctx.font = `21px ${FONT}`;
    ctx.textAlign = "left";
    ctx.fillText(`+${scene.hud.overflow} more`, hx, hy + 4);
  }
  if (scene.hud.total === 0) {
    ctx.fillStyle = C.muted;
    ctx.font = `21px ${FONT}`;
    ctx.fillText("None right now", hx, hy);
  }
  ctx.restore();
}
