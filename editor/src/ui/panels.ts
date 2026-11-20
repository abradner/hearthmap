// The sidebar: inspector for the selected room or pin, lists, entities, layers and issues.
// Re-rendered from state on every change; inputs commit on "change" so a re-render never eats
// a half-typed value.

import { applyCalibrations, parseCalibrationFile } from "../lib/calibrationImport";
import {
  DEFAULT_BANDS,
  DEFAULT_HEAT,
  DEFAULT_HUD_MAX_ITEMS,
  DEFAULT_STALE_AFTER_S,
  LAYER_UNITS,
  PHOTO_SIZE_PX,
  PRIMARY_ROLE,
  allowsMoisture,
  allowsPhoto,
  needsCalibration,
} from "../lib/defaults";
import { ROLE_DEVICE_CLASSES, deviceClassesIn, filterEntities, findDuplicateBindings, parseStatesExport } from "../lib/entities";
import { fetchStates } from "../lib/haClient";
import { ENTITY_ID_PATTERN, ID_PATTERN, NAME_MAX, codePointLength } from "../lib/ids";
import * as ops from "../lib/ops";
import { issueTarget, type Issue } from "../lib/validate";
import { BAND_FIELDS, KINDS, LAYERS, ROLES, THRESHOLD_FIELDS, type Config, type Kind, type LayerName, type Pin, type Role } from "../lib/types";
import { h, numberOrUndefined, renderInto } from "./dom";
import { toDevicePhoto } from "./photoResize";
import type { State, Store } from "./store";

const thumbUrls = new WeakMap<Uint8Array, string>();
function thumbUrl(bytes: Uint8Array): string {
  let u = thumbUrls.get(bytes);
  if (!u) {
    u = URL.createObjectURL(new Blob([bytes as BlobPart], { type: "image/png" }));
    thumbUrls.set(bytes, u);
  }
  return u;
}

const KIND_LABEL: Record<Kind, string> = {
  temperature: "Temperature",
  humidity: "Humidity",
  soil: "Soil probe",
  air_quality: "Air quality",
  plant: "Plant",
};

const ROLE_LABEL: Record<Role, string> = {
  temperature: "Temperature",
  humidity: "Humidity",
  moisture: "Soil moisture",
  co2: "CO2",
  pm25: "PM2.5",
  voc: "VOC",
  battery: "Battery",
  predicted_dry_at: "Predicted dry at (reserved)",
};

const TABS: [State["sidebarTab"], string][] = [
  ["item", "Selected"],
  ["rooms", "Rooms"],
  ["pins", "Pins"],
  ["entities", "Entities"],
  ["layers", "Layers"],
  ["issues", "Issues"],
];

function issuesFor(issues: readonly Issue[], kind: "room" | "pin", index: number): Issue[] {
  return issues.filter((i) => {
    const t = issueTarget(i.path);
    return t.kind === kind && t.index === index;
  });
}

function issueList(list: readonly Issue[]): HTMLElement | null {
  if (!list.length) return null;
  return h(
    "ul",
    { class: "issues", "data-testid": "item-issues" },
    list.map((i) => h("li", { class: i.severity }, h("span", { class: "sev" }, i.severity === "error" ? "Error" : "Warning"), ` ${i.message}`)),
  );
}

function field(label: string, control: HTMLElement, hint?: string): HTMLElement {
  return h("label", { class: "field" }, h("span", { class: "label" }, label), control, hint ? h("span", { class: "hint" }, hint) : null);
}

/** Roles a pin of `kind` may bind, required first. Moisture only on soil and plant (R1). */
export function rolesFor(kind: Kind): Role[] {
  const primary = PRIMARY_ROLE[kind];
  return [primary, ...ROLES.filter((r) => r !== primary && (r !== "moisture" || allowsMoisture(kind)))];
}

export class Sidebar {
  private tabs: HTMLElement;
  private body: HTMLElement;
  /** Built once and re-attached on every render, so typed values and open/closed state survive. */
  private pasteBlock: HTMLElement;
  private haBlock: HTMLElement;

  constructor(
    host: HTMLElement,
    private store: Store,
  ) {
    this.tabs = h("nav", { class: "tabs", role: "tablist" });
    this.body = h("div", { class: "panel-body" });
    this.pasteBlock = this.buildPasteBlock();
    this.haBlock = this.buildHaBlock();
    host.append(this.tabs, this.body);
    store.subscribe(() => this.render());
    this.render();
  }

  private render(): void {
    const st = this.store.state;
    const errors = st.issues.filter((i) => i.severity === "error").length;
    renderInto(
      this.tabs,
      TABS.map(([id, label]) =>
        h(
          "button",
          {
            class: st.sidebarTab === id ? "tab active" : "tab",
            role: "tab",
            "aria-selected": st.sidebarTab === id ? "true" : "false",
            "data-testid": `tab-${id}`,
            onclick: () => this.store.set({ sidebarTab: id }),
          },
          label,
          id === "issues" && st.issues.length ? h("span", { class: errors ? "count err" : "count" }, String(st.issues.length)) : null,
        ),
      ),
    );
    const content =
      st.sidebarTab === "item"
        ? this.item()
        : st.sidebarTab === "rooms"
          ? this.rooms()
          : st.sidebarTab === "pins"
            ? this.pins()
            : st.sidebarTab === "entities"
              ? this.entities()
              : st.sidebarTab === "layers"
                ? this.layers()
                : this.issues();
    renderInto(this.body, content);
  }

  // ---- selected item ----------------------------------------------------------------------------

  private item(): HTMLElement[] {
    const sel = this.store.state.selection;
    if (sel?.type === "room") return this.roomInspector(sel.index, sel.vertex);
    if (sel?.type === "pin") return this.pinInspector(sel.index);
    return [
      h("h2", {}, "Nothing selected"),
      h(
        "div",
        { class: "help" },
        h("p", {}, "Load a floor plan as a backdrop, then trace rooms and drop pins on it. Only geometry is exported; the backdrop never leaves this page."),
        h(
          "ul",
          {},
          h("li", {}, h("b", {}, "Draw room (R): "), "click each corner; click the first corner, press Enter or use Finish to close it. Snaps to corners and to horizontal/vertical (hold Alt to stop snapping)."),
          h("li", {}, h("b", {}, "Select (V): "), "click a room, then drag its corners; drag a small dot on an edge to add a corner; select a corner and press Delete to remove it."),
          h("li", {}, h("b", {}, "Add pin (P): "), "choose a kind, then click inside a room."),
          h("li", {}, h("b", {}, "Pan and zoom: "), "drag empty space, two-finger scroll, pinch, or the zoom buttons. Space+drag pans while any tool is active."),
        ),
      ),
    ];
  }

  private roomInspector(i: number, vertex?: number): HTMLElement[] {
    const c = this.store.config;
    const room = c.rooms[i];
    if (!room) return [h("p", {}, "That room no longer exists.")];
    const s = this.store;
    return [
      h("h2", {}, "Room"),
      field(
        "Name",
        h("input", {
          type: "text",
          value: room.name,
          "data-key": `room-name-${i}`,
          "data-testid": "room-name",
          onchange: (e: Event) => {
            const v = (e.target as HTMLInputElement).value.trim();
            if (!nameOk(s, v)) return;
            s.edit((x) => ops.renameRoom(x, i, v));
          },
        }),
      ),
      field(
        "Id",
        h("input", {
          type: "text",
          value: room.id,
          pattern: ID_PATTERN.source,
          "data-key": `room-id-${i}`,
          onchange: (e: Event) => {
            const v = (e.target as HTMLInputElement).value.trim();
            if (!ID_PATTERN.test(v)) return s.set({ message: { text: "Ids are lower case letters, digits and _, starting with a letter", kind: "error" } });
            s.edit((x) => ops.setRoomId(x, i, v));
          },
        }),
        "Pins refer to the room by id. Follows the name until you set it by hand.",
      ),
      h(
        "label",
        { class: "check" },
        h("input", {
          type: "checkbox",
          checked: room.outdoor === true,
          "data-testid": "room-outdoor",
          onchange: (e: Event) => s.edit((x) => ops.setOutdoor(x, i, (e.target as HTMLInputElement).checked)),
        }),
        " Outdoor (balcony, garden): never judged against indoor comfort, drawn without heat colour",
      ),
      h("p", { class: "muted" }, `${room.polygon.length} corners. Earlier rooms own any overlap.`),
      h(
        "div",
        { class: "row" },
        vertex !== undefined
          ? h(
              "button",
              {
                "data-testid": "delete-vertex",
                disabled: room.polygon.length <= 3,
                onclick: () => s.edit((x) => ops.deleteVertex(x, i, vertex), { selection: { type: "room", index: i } }),
              },
              "Delete corner",
            )
          : null,
        h("button", { onclick: () => s.edit((x) => ops.reorderRoom(x, i, -1), { selection: { type: "room", index: Math.max(0, i - 1) } }) }, "Move up"),
        h(
          "button",
          { onclick: () => s.edit((x) => ops.reorderRoom(x, i, 1), { selection: { type: "room", index: Math.min(c.rooms.length - 1, i + 1) } }) },
          "Move down",
        ),
        h("button", { class: "danger", "data-testid": "delete-room", onclick: () => s.edit((x) => ops.deleteRoom(x, i), { selection: undefined }) }, "Delete room"),
      ),
      issueList(issuesFor(s.state.issues, "room", i)),
    ].filter((x): x is HTMLElement => x !== null);
  }

  private pinInspector(i: number): HTMLElement[] {
    const c = this.store.config;
    const pin = c.pins[i];
    if (!pin) return [h("p", {}, "That pin no longer exists.")];
    const s = this.store;
    const suggested = ops.suggestRoom(c, pin.position);
    const dupes = findDuplicateBindings(c);
    const out: (HTMLElement | null)[] = [
      h("h2", {}, KIND_LABEL[pin.kind], " pin"),
      field(
        "Name",
        h("input", {
          type: "text",
          value: pin.name,
          "data-key": `pin-name-${i}`,
          "data-testid": "pin-name",
          onchange: (e: Event) => {
            const v = (e.target as HTMLInputElement).value.trim();
            if (!nameOk(s, v)) return;
            s.edit((x) => ops.updatePin(x, i, { name: v }));
          },
        }),
        pin.kind === "plant" ? "The plant's name, as the HUD shows it." : undefined,
      ),
      field(
        "Id",
        h("input", {
          type: "text",
          value: pin.id,
          "data-key": `pin-id-${i}`,
          onchange: (e: Event) => {
            const v = (e.target as HTMLInputElement).value.trim();
            if (!ID_PATTERN.test(v)) return s.set({ message: { text: "Ids are lower case letters, digits and _, starting with a letter", kind: "error" } });
            s.edit((x) => ops.updatePin(x, i, { id: v }));
          },
        }),
      ),
      field(
        "Kind",
        h(
          "select",
          { "data-testid": "pin-kind", onchange: (e: Event) => s.edit((x) => ops.setPinKind(x, i, (e.target as HTMLSelectElement).value as Kind)) },
          KINDS.map((k) => h("option", { value: k, selected: k === pin.kind }, KIND_LABEL[k])),
        ),
        "Changing kind drops fields the new kind cannot have (undo brings them back).",
      ),
      field(
        "Room",
        h(
          "select",
          { "data-testid": "pin-room", onchange: (e: Event) => s.edit((x) => ops.updatePin(x, i, { room: (e.target as HTMLSelectElement).value })) },
          c.rooms.map((r) => h("option", { value: r.id, selected: r.id === pin.room }, r.name + (r.id === suggested ? " (under the pin)" : ""))),
          c.rooms.some((r) => r.id === pin.room) ? null : h("option", { value: pin.room, selected: true }, `${pin.room} (missing)`),
        ),
        suggested === undefined ? "The pin is outside every room." : suggested !== pin.room ? "The pin sits in a different room than this one." : undefined,
      ),
      h("h3", {}, "Entities"),
      ...rolesFor(pin.kind).map((role) => this.roleRow(i, pin, role, dupes.get(pin.entities[role] ?? "")?.length ?? 0)),
    ];
    if (needsCalibration(pin.kind)) out.push(...this.calibration(i, pin));
    if (allowsPhoto(pin.kind)) out.push(...this.photo(i, pin));
    out.push(
      field(
        "Stale after (seconds)",
        h("input", {
          type: "number",
          min: 1,
          step: 1,
          value: pin.stale_after_s ?? "",
          placeholder: String(c.stale_after_s ?? DEFAULT_STALE_AFTER_S),
          "data-key": `pin-stale-${i}`,
          onchange: (e: Event) => s.edit((x) => ops.setPinStaleAfter(x, i, intOrUndefined((e.target as HTMLInputElement).value))),
        }),
        "Empty: the project's timeout. Soil sensors often report rarely.",
      ),
      h("div", { class: "row" }, h("button", { class: "danger", "data-testid": "delete-pin", onclick: () => s.edit((x) => ops.deletePin(x, i), { selection: undefined }) }, "Delete pin")),
      issueList(issuesFor(s.state.issues, "pin", i)),
    );
    return out.filter((x): x is HTMLElement => x !== null);
  }

  private roleRow(i: number, pin: Pin, role: Role, uses: number): HTMLElement {
    const required = PRIMARY_ROLE[pin.kind] === role;
    const ents = this.store.state.entities;
    const classes = ROLE_DEVICE_CLASSES[role];
    const options = ents.filter((e) => e.device_class !== undefined && classes.includes(e.device_class));
    const listId = `dl-${role}`;
    const value = pin.entities[role] ?? "";
    return h(
      "div",
      { class: "role-row" },
      field(
        `${ROLE_LABEL[role]}${required ? " (required)" : ""}`,
        h("input", {
          type: "text",
          list: listId,
          value,
          placeholder: required ? "sensor.example_..." : "optional",
          "data-key": `role-${i}-${role}`,
          "data-testid": `bind-${role}`,
          spellcheck: "false",
          autocomplete: "off",
          onchange: (e: Event) => {
            const v = (e.target as HTMLInputElement).value.trim();
            if (v && !ENTITY_ID_PATTERN.test(v)) return this.store.set({ message: { text: `"${v}" is not an entity id (domain.object_id)`, kind: "error" } });
            this.store.edit((x) => ops.bindEntity(x, i, role, v || undefined));
          },
        }),
        uses > 1 ? `Warning: bound ${uses} times in this project.` : undefined,
      ),
      h(
        "datalist",
        { id: listId },
        (options.length ? options : ents).map((e) => h("option", { value: e.entity_id }, e.name ?? "")),
      ),
    );
  }

  private calibration(i: number, pin: Pin): HTMLElement[] {
    const cal = pin.calibration ?? { dry: 0, wet: 100 };
    const set = (patch: Partial<typeof cal>) => this.store.edit((x) => ops.setCalibration(x, i, { ...cal, ...patch }));
    const num = (key: "dry" | "wet" | "happy_min" | "happy_max", required: boolean) =>
      h("input", {
        type: "number",
        step: "any",
        value: cal[key] ?? "",
        placeholder: required ? "" : "none",
        "data-key": `cal-${i}-${key}`,
        "data-testid": `cal-${key}`,
        onchange: (e: Event) => {
          const v = numberOrUndefined((e.target as HTMLInputElement).value);
          if (required && v === undefined) return this.store.set({ message: { text: `${key} is required`, kind: "error" } });
          set({ [key]: v });
        },
      });
    return [
      h("h3", {}, "Calibration"),
      h("p", { class: "muted" }, "In the sensor's raw units, as Home Assistant shows them. Wet may be below dry (capacitive probes)."),
      h("div", { class: "grid2" }, field("Dry", num("dry", true)), field("Wet", num("wet", true)), field("Happy min", num("happy_min", false)), field("Happy max", num("happy_max", false))),
      h("p", { class: "muted" }, "Happy min is the driest reading the plant is happy at; happy max the wettest. Import a file from tools/calibrate in the Pins tab."),
    ];
  }

  private photo(i: number, pin: Pin): HTMLElement[] {
    const s = this.store;
    const bytes = pin.photo ? s.state.photos.get(pin.photo) : undefined;
    const thumb = bytes ? h("img", { class: "thumb", alt: "", src: thumbUrl(bytes) }) : null;
    return [
      h("h3", {}, "Photo"),
      h(
        "div",
        { class: "photo-row" },
        thumb,
        h(
          "div",
          {},
          h("p", { class: "muted" }, pin.photo ? `photos/${pin.photo}.png${bytes ? "" : " (not loaded: attach it again)"}` : "No photo"),
          h("input", {
            type: "file",
            accept: "image/*",
            "data-testid": "photo-file",
            onchange: async (e: Event) => {
              const f = (e.target as HTMLInputElement).files?.[0];
              if (!f) return;
              try {
                const png = await toDevicePhoto(f);
                const id = pin.photo ?? ops.photoIdFor(s.config, i);
                s.setPhoto(id, png);
                if (pin.photo !== id) s.edit((x) => ops.setPinPhoto(x, i, id));
              } catch (err) {
                s.set({ message: { text: `Could not read that image: ${(err as Error).message}`, kind: "error" } });
              }
            },
          }),
          pin.photo ? h("button", { onclick: () => s.edit((x) => ops.setPinPhoto(x, i, undefined)) }, "Remove photo") : null,
        ),
      ),
      h("p", { class: "muted" }, `Exported as a ${PHOTO_SIZE_PX} x ${PHOTO_SIZE_PX} PNG cut from the centre of the image. Photos are not autosaved.`),
    ];
  }

  // ---- lists ------------------------------------------------------------------------------------

  private rooms(): HTMLElement[] {
    const st = this.store.state;
    const c = this.store.config;
    if (!c.rooms.length) return [h("p", { class: "muted" }, "No rooms yet. Use Draw room.")];
    return [
      h(
        "ul",
        { class: "list" },
        c.rooms.map((r, i) => {
          const n = issuesFor(st.issues, "room", i).length;
          return h(
            "li",
            { class: st.selection?.type === "room" && st.selection.index === i ? "selected" : "" },
            h("button", { class: "link", onclick: () => this.store.set({ selection: { type: "room", index: i }, sidebarTab: "item" }) }, r.name),
            h("span", { class: "muted" }, ` ${r.id}${r.outdoor ? ", outdoor" : ""}`),
            n ? h("span", { class: "count err" }, String(n)) : null,
          );
        }),
      ),
    ];
  }

  private pins(): HTMLElement[] {
    const st = this.store.state;
    const c = this.store.config;
    return [
      h(
        "div",
        { class: "row" },
        h(
          "label",
          { class: "button" },
          "Import calibration...",
          h("input", {
            type: "file",
            accept: ".json,application/json",
            hidden: true,
            "data-testid": "calibration-file",
            onchange: async (e: Event) => {
              const f = (e.target as HTMLInputElement).files?.[0];
              if (!f) return;
              try {
                const file = parseCalibrationFile(await f.text());
                const r = applyCalibrations(this.store.config, file);
                const applied = r.outcomes.filter((o) => o.result === "applied").length;
                this.store.edit(() => r.config, {
                  calibrationReport: { source: `${f.name} (${file.generator}, ${file.generated_at})`, outcomes: r.outcomes },
                });
                this.store.set({ message: { text: `Calibration applied for ${applied} of ${r.outcomes.length} sensor(s); details in the Pins tab`, kind: "info" } });
              } catch (err) {
                this.store.set({ message: { text: `Calibration file: ${(err as Error).message}`, kind: "error" } });
              }
            },
          }),
        ),
      ),
      this.calibrationReport(),
      c.pins.length
        ? h(
            "ul",
            { class: "list" },
            c.pins.map((p, i) => {
              const n = issuesFor(st.issues, "pin", i).length;
              return h(
                "li",
                { class: st.selection?.type === "pin" && st.selection.index === i ? "selected" : "" },
                h("button", { class: "link", onclick: () => this.store.set({ selection: { type: "pin", index: i }, sidebarTab: "item" }) }, p.name),
                h("span", { class: "muted" }, ` ${KIND_LABEL[p.kind]}, ${c.rooms.find((r) => r.id === p.room)?.name ?? p.room}`),
                n ? h("span", { class: "count err" }, String(n)) : null,
              );
            }),
          )
        : h("p", { class: "muted" }, "No pins yet. Use Add pin."),
    ].filter((x): x is HTMLElement => x !== null);
  }

  private calibrationReport(): HTMLElement | null {
    const rep = this.store.state.calibrationReport;
    if (!rep) return null;
    const label: Record<string, string> = { applied: "Applied", skipped: "Skipped", unmatched: "No pin binds it", invalid: "Refused" };
    return h(
      "details",
      { open: true, "data-testid": "calibration-report" },
      h("summary", {}, `Last calibration import: ${rep.source}`),
      h(
        "ul",
        { class: "issues" },
        rep.outcomes.map((o) =>
          h(
            "li",
            { class: o.result === "applied" ? "" : "warning" },
            h("code", {}, o.entity_id),
            ` ${label[o.result]}${o.pins.length ? ` to ${o.pins.join(", ")}` : ""}; confidence ${o.confidence}`,
            o.happy_is_default ? h("div", { class: "muted" }, "Happy range is the tool's default, not species knowledge: tune it per plant.") : null,
            o.message ? h("div", { class: "warn" }, o.message) : null,
            o.warnings.length ? h("div", { class: "muted" }, o.warnings.join("; ")) : null,
          ),
        ),
      ),
    );
  }

  // ---- entities ---------------------------------------------------------------------------------

  private entities(): HTMLElement[] {
    const s = this.store;
    const st = s.state;
    const c = s.config;
    const sel = st.selection?.type === "pin" ? st.selection.index : undefined;
    const selPin = sel === undefined ? undefined : c.pins[sel];
    const bound = new Map<string, string[]>();
    c.pins.forEach((p) => {
      for (const [role, id] of Object.entries(p.entities)) bound.set(id, [...(bound.get(id) ?? []), `${p.name} (${role})`]);
    });
    const list = filterEntities(st.entities, st.entityFilter);
    return [
      h("h2", {}, "Home Assistant entities"),
      h("p", { class: "muted" }, "Only a picker's source: the config stores entity ids. Nothing here is saved."),
      h(
        "div",
        { class: "row" },
        h(
          "label",
          { class: "button" },
          "Load states file...",
          h("input", {
            type: "file",
            accept: ".json,application/json",
            hidden: true,
            "data-testid": "states-file",
            onchange: async (e: Event) => {
              const f = (e.target as HTMLInputElement).files?.[0];
              if (f) this.loadStates(await f.text(), f.name);
            },
          }),
        ),
      ),
      this.pasteBlock,
      this.haBlock,
      st.entities.length
        ? h(
            "div",
            { class: "filters" },
            h(
              "select",
              { "data-testid": "entity-class", onchange: (e: Event) => s.set({ entityFilter: { ...st.entityFilter, deviceClass: (e.target as HTMLSelectElement).value } }) },
              h("option", { value: "", selected: st.entityFilter.deviceClass === "" }, "Any device class"),
              h("option", { value: "none", selected: st.entityFilter.deviceClass === "none" }, "No device class"),
              deviceClassesIn(st.entities).map((d) => h("option", { value: d, selected: st.entityFilter.deviceClass === d }, d)),
            ),
            h("input", {
              type: "search",
              placeholder: "Search id or name",
              value: st.entityFilter.search,
              "data-key": "entity-search",
              "data-testid": "entity-search",
              oninput: (e: Event) => s.set({ entityFilter: { ...st.entityFilter, search: (e.target as HTMLInputElement).value } }),
            }),
          )
        : null,
      st.entities.length
        ? h(
            "ul",
            { class: "list entities", "data-testid": "entity-list" },
            list.slice(0, 300).map((e) => {
              const where = bound.get(e.entity_id);
              return h(
                "li",
                {},
                h("code", {}, e.entity_id),
                h("span", { class: "muted" }, ` ${e.name ?? ""} ${e.state ?? ""}${e.unit ? ` ${e.unit}` : ""}${e.device_class ? ` [${e.device_class}]` : ""}`),
                where ? h("div", { class: where.length > 1 ? "warn" : "muted" }, `${where.length > 1 ? "Bound twice: " : "Bound: "}${where.join(", ")}`) : null,
                selPin
                  ? h(
                      "div",
                      { class: "row" },
                      rolesFor(selPin.kind)
                        .filter((r) => !e.device_class || ROLE_DEVICE_CLASSES[r].includes(e.device_class))
                        .slice(0, 3)
                        .map((role) =>
                          h("button", { class: "small", onclick: () => s.edit((x) => ops.bindEntity(x, sel as number, role, e.entity_id)) }, `Bind to ${selPin.name} as ${ROLE_LABEL[role]}`),
                        ),
                    )
                  : null,
              );
            }),
            list.length > 300 ? h("li", { class: "muted" }, `${list.length - 300} more: narrow the search`) : null,
          )
        : null,
    ].filter((x): x is HTMLElement => x !== null);
  }

  private loadStates(text: string, from: string): void {
    try {
      const r = parseStatesExport(text);
      this.store.set({ entities: r.entities, message: { text: `${r.entities.length} entities from ${from}${r.skipped ? `, ${r.skipped} skipped` : ""}`, kind: "info" } });
    } catch (err) {
      this.store.set({ message: { text: `${from}: ${(err as Error).message}`, kind: "error" } });
    }
  }

  private buildPasteBlock(): HTMLElement {
    const ta = h("textarea", { rows: 4, placeholder: "Output of GET /api/states (a JSON array)", "data-testid": "states-paste" });
    return h(
      "details",
      {},
      h("summary", {}, "Paste a states export"),
      ta,
      h("button", { "data-testid": "states-paste-use", onclick: () => this.loadStates(ta.value, "pasted text") }, "Use pasted states"),
    );
  }

  private buildHaBlock(): HTMLElement {
    const urlInput = h("input", { type: "url", placeholder: "http://homeassistant.local:8123", autocomplete: "off" });
    // The token is read from this field at the moment of the fetch and never copied anywhere:
    // not into the store, not into localStorage, not into the config, not into a message.
    const tokenInput = h("input", { type: "password", placeholder: "Long-lived access token", autocomplete: "off", spellcheck: "false" });
    return h(
      "details",
      {},
      h("summary", {}, "Fetch live from Home Assistant"),
      h(
        "p",
        { class: "muted" },
        "The token is kept only in this field, sent only to the address you type, and never stored or exported. Home Assistant must allow this page's origin (http: cors_allowed_origins), and an https page cannot reach an http address.",
      ),
      field("Address", urlInput),
      field("Token", tokenInput),
      h(
        "button",
        {
          onclick: async () => {
            try {
              const r = await fetchStates(urlInput.value, tokenInput.value);
              tokenInput.value = "";
              this.store.set({ entities: r.entities, message: { text: `${r.entities.length} entities fetched; the token field was cleared`, kind: "info" } });
            } catch (err) {
              this.store.set({ message: { text: (err as Error).message, kind: "error" } });
            }
          },
        },
        "Fetch entities",
      ),
    );
  }

  // ---- layers and project settings --------------------------------------------------------------

  private layers(): HTMLElement[] {
    const s = this.store;
    const c = s.config;
    const unit = c.temperature_unit ?? "C";
    return [
      h("h2", {}, "Project"),
      field(
        "Temperature unit",
        h(
          "select",
          { onchange: (e: Event) => s.edit((x) => ops.setTemperatureUnit(x, ((e.target as HTMLSelectElement).value || undefined) as "C" | "F" | undefined)) },
          h("option", { value: "", selected: c.temperature_unit === undefined }, "°C (default)"),
          h("option", { value: "C", selected: c.temperature_unit === "C" }, "°C"),
          h("option", { value: "F", selected: c.temperature_unit === "F" }, "°F (set every temperature field below)"),
        ),
        "Core does not convert: readings and bands must be in this unit.",
      ),
      field(
        "Stale after (seconds)",
        h("input", {
          type: "number",
          min: 1,
          step: 1,
          value: c.stale_after_s ?? "",
          placeholder: `${DEFAULT_STALE_AFTER_S} (default, 3 h)`,
          "data-key": "stale",
          "data-testid": "stale-after",
          onchange: (e: Event) => s.edit((x) => ops.setStaleAfter(x, intOrUndefined((e.target as HTMLInputElement).value))),
        }),
      ),
      field(
        "HUD cap",
        h("input", {
          type: "number",
          min: 1,
          max: 20,
          step: 1,
          value: c.hud?.max_items ?? "",
          placeholder: `${DEFAULT_HUD_MAX_ITEMS} (default)`,
          "data-key": "hud",
          "data-testid": "hud-max",
          onchange: (e: Event) => s.edit((x) => ops.setHudMax(x, intOrUndefined((e.target as HTMLInputElement).value))),
        }),
        "Thirsty plants listed before \"+N more\".",
      ),
      field(
        "Start-up layer",
        h(
          "select",
          { onchange: (e: Event) => s.edit((x) => ops.setDefaultLayer(x, ((e.target as HTMLSelectElement).value || undefined) as LayerName | undefined)) },
          h("option", { value: "", selected: c.layers?.default === undefined }, "Temperature (default)"),
          LAYERS.map((l) => h("option", { value: l, selected: c.layers?.default === l }, l)),
        ),
      ),
      h(
        "details",
        {},
        h("summary", {}, "Heat-field tuning"),
        ...(["grid_cols", "grid_rows", "idw_power"] as const).map((k) =>
          field(
            k,
            h("input", {
              type: "number",
              step: k === "idw_power" ? "any" : 1,
              value: c.heat?.[k] ?? "",
              placeholder: `${DEFAULT_HEAT[k]} (default)`,
              "data-key": `heat-${k}`,
              onchange: (e: Event) => {
                const v = numberOrUndefined((e.target as HTMLInputElement).value);
                s.edit((x) => {
                  const heat = { ...(x.heat ?? {}) };
                  if (v === undefined) delete heat[k];
                  else heat[k] = v;
                  const { heat: _h, ...rest } = x;
                  return Object.keys(heat).length ? { ...rest, heat } : rest;
                });
              },
            }),
          ),
        ),
      ),
      h("h2", {}, "Layers"),
      h("p", { class: "muted" }, "Empty fields take the default shown in grey. Tick \"none\" to remove a threshold (written as null)."),
      ...LAYERS.map((l) => this.layerEditor(c, l, unit)),
    ];
  }

  private layerEditor(c: Config, layer: LayerName, unit: string): HTMLElement {
    const s = this.store;
    const set = c.layers?.[layer] ?? {};
    const units = layer === "temperature" ? `°${unit}` : LAYER_UNITS[layer];
    const layerIssues = s.state.issues.filter((i) => {
      const t = issueTarget(i.path);
      return t.kind === "layer" && t.layer === layer;
    });
    return h(
      "fieldset",
      { class: "layer", "data-testid": `layer-${layer}` },
      h("legend", {}, `${layer} (${units})`),
      h(
        "label",
        { class: "check" },
        h("input", {
          type: "checkbox",
          checked: set.enabled ?? true,
          onchange: (e: Event) => s.edit((x) => ops.setLayerField(x, layer, "enabled", (e.target as HTMLInputElement).checked ? undefined : false)),
        }),
        " Enabled",
      ),
      h(
        "div",
        { class: "grid2" },
        BAND_FIELDS.map((f) => {
          const v = set[f];
          const def = DEFAULT_BANDS[layer][f];
          const isThreshold = THRESHOLD_FIELDS.includes(f);
          return h(
            "div",
            { class: "band-field" },
            field(
              f.replace("_", " "),
              h("input", {
                type: "number",
                step: "any",
                value: v === null || v === undefined ? "" : v,
                placeholder: v === null ? "none" : def === null ? "none (default)" : String(def),
                disabled: v === null,
                "data-key": `band-${layer}-${f}`,
                "data-testid": `band-${layer}-${f}`,
                onchange: (e: Event) => s.edit((x) => ops.setLayerField(x, layer, f, numberOrUndefined((e.target as HTMLInputElement).value))),
              }),
            ),
            isThreshold
              ? h(
                  "label",
                  { class: "check small" },
                  h("input", {
                    type: "checkbox",
                    checked: v === null,
                    onchange: (e: Event) => s.edit((x) => ops.setLayerField(x, layer, f, (e.target as HTMLInputElement).checked ? null : undefined)),
                  }),
                  " none",
                )
              : null,
          );
        }),
      ),
      issueList(layerIssues),
    );
  }

  // ---- issues -----------------------------------------------------------------------------------

  private issues(): HTMLElement[] {
    const s = this.store;
    const c = s.config;
    const list = s.state.issues;
    if (!list.length) return [h("p", { class: "ok", "data-testid": "no-issues" }, "No problems: the config validates against the schema.")];
    const label = (i: Issue) => {
      const t = issueTarget(i.path);
      if (t.kind === "room") return `Room "${c.rooms[t.index]?.name ?? t.index}"`;
      if (t.kind === "pin") return `Pin "${c.pins[t.index]?.name ?? t.index}"`;
      if (t.kind === "layer") return `Layer ${t.layer}`;
      return "Project";
    };
    return [
      h("p", { class: "muted" }, "Errors block export. Schema errors come from schema/hearthmap.schema.json; the rest repeat checks the firmware build also makes."),
      h(
        "ul",
        { class: "issues", "data-testid": "issue-list" },
        list.map((i) =>
          h(
            "li",
            { class: i.severity },
            h(
              "button",
              {
                class: "link",
                onclick: () => {
                  const t = issueTarget(i.path);
                  if (t.kind === "room" || t.kind === "pin") s.set({ selection: { type: t.kind, index: t.index }, sidebarTab: "item" });
                  else if (t.kind === "layer") s.set({ sidebarTab: "layers" });
                },
              },
              label(i),
            ),
            h("span", { class: "sev" }, i.severity === "error" ? " Error" : " Warning"),
            `: ${i.message} `,
            h("code", { class: "muted" }, i.path),
          ),
        ),
      ),
    ];
  }
}

/**
 * Names are 1..48 Unicode code points (schema maxLength, and core counts the same way). An HTML
 * maxlength would count UTF-16 units and cut an emoji name at 24, so the check is done here.
 */
function nameOk(s: Store, v: string): boolean {
  if (v === "") return false;
  if (codePointLength(v) > NAME_MAX) {
    s.set({ message: { text: `Names are at most ${NAME_MAX} characters (this one has ${codePointLength(v)})`, kind: "error" } });
    return false;
  }
  return true;
}

function intOrUndefined(text: string): number | undefined {
  const n = numberOrUndefined(text);
  return n === undefined ? undefined : Math.round(n);
}

