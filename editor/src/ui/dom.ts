// A tiny hyperscript helper. No framework: panels are re-rendered from state on every change,
// and focus is restored by `data-key` so typing into a field survives a re-render.

type Child = Node | string | number | false | null | undefined;
type Attrs = Record<string, unknown>;

const text = (v: unknown): string => (typeof v === "string" || typeof v === "number" || typeof v === "boolean" ? String(v) : "");

export function h<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Attrs = {}, ...children: (Child | Child[])[]): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith("on") && typeof v === "function") {
      el.addEventListener(k.slice(2).toLowerCase(), v as EventListener);
    } else if (k === "class") {
      el.className = text(v);
    } else if (k === "value" && "value" in el) {
      (el as HTMLInputElement).value = text(v);
    } else if (k === "checked" && "checked" in el) {
      (el as HTMLInputElement).checked = Boolean(v);
    } else if (k === "style" && typeof v === "object") {
      Object.assign(el.style, v);
    } else if (v === true) {
      el.setAttribute(k, "");
    } else {
      el.setAttribute(k, text(v));
    }
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    el.append(typeof c === "string" || typeof c === "number" ? String(c) : c);
  }
  return el;
}

/** Replace `host`'s children, keeping focus and caret on the element with the same data-key. */
export function renderInto(host: HTMLElement, content: Node[]): void {
  const active = document.activeElement as HTMLInputElement | null;
  const key = active && host.contains(active) ? active.dataset.key : undefined;
  const sel = key && "selectionStart" in active! ? [active.selectionStart, active.selectionEnd] : undefined;
  const scroll = host.scrollTop;
  host.replaceChildren(...content);
  host.scrollTop = scroll;
  if (key) {
    const next = host.querySelector<HTMLInputElement>(`[data-key="${CSS.escape(key)}"]`);
    if (next) {
      next.focus();
      if (sel && typeof next.setSelectionRange === "function" && (next.type === "text" || next.type === "search")) {
        try {
          next.setSelectionRange(sel[0] ?? null, sel[1] ?? null);
        } catch {
          // Not a text input after all.
        }
      }
    }
  }
}

/** Parse a number field; empty means "unset". NaN is reported as undefined too. */
export function numberOrUndefined(text: string): number | undefined {
  if (text.trim() === "") return undefined;
  const n = Number(text);
  return Number.isFinite(n) ? n : undefined;
}

export function readFileBytes(file: File): Promise<Uint8Array> {
  return file.arrayBuffer().then((b) => new Uint8Array(b));
}

export function download(name: string, bytes: Uint8Array | string, type: string): void {
  const blob = new Blob([bytes as BlobPart], { type });
  const url = URL.createObjectURL(blob);
  const a = h("a", { href: url, download: name });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}
