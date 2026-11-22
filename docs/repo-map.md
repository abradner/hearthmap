# Repository Map

> Read this before touching anything boundary-sensitive: auth, tenancy, deletion/visibility,
> serialization, background delivery, routing, any external surface. Update it in the same PR as
> any change that moves an edge here. Never leave it describing a stale security boundary — a
> wrong map is worse than no map, because it is trusted.

## What belongs here — and what doesn't

This is a **boundary map, not a directory tree.** Anything an agent can answer with grep/glob in
seconds — where a symbol is defined, which files match a pattern — does *not* belong here: that
content goes stale fastest and helps least. What belongs is the knowledge that lives *between*
files and can't be reconstructed from any one of them:

- **Surfaces** — every distinct way into the system, each with its auth mechanism.
- **Trust boundaries** — for each edge: who is the caller, how is identity established, and what
  happens if they lie.
- **Cross-cutting flows** — the handful of sequences where one action touches many parts, and
  where a change to one part silently breaks an invariant held elsewhere.
- **Risk register** — numbered, stable IDs for known concentrations of risk, so rules, PRs, and
  code comments can cite them.

Start small and grow by incident: three accurate entries beat thirty aspirational ones. When the
map and the code disagree, the code is the truth and the map is the bug — fix it in the same
change that found the disagreement.

Every surface below exists and is tested. The firmware is built and flashed from the owner's
private configuration repository, never from this one (`AGENTS.md` §7).

## Surfaces

| Surface | Path / entry point | Auth mechanism | Notes |
|---|---|---|---|
| Config file -> build | `components/hearthmap/__init__.py` | None: the operator's own file, in their private repo | Validated against the schema before codegen; core's `validate()` runs at boot, and before a flash through `hearthmap-render --check` (D-048). Photo ids are pattern-restricted so they cannot name a path outside `photos/` (R-003) |
| Home Assistant -> device | ESPHome native API state subscriptions in `components/hearthmap` | ESPHome API encryption key, configured in the private device YAML, never here | Readings enter core as `Readings`; non-numeric states become `Unknown`. Values are re-stamped while connected (D-012, R-004) |
| Touch -> UI state | LVGL events -> `hearthmap::apply()` | Physical access to the panel | Read-only UI in v1: a tap opens or closes an overlay or switches a tab, and can change nothing in Home Assistant |
| Browser editor | `editor/`, static; GitHub Pages deploy is manual (`editor-pages.yml`) | Optional HA long-lived token typed into the page; opened config/photo/zip/calibration files | Token lives only in its password field, is read at fetch time and cleared after a successful fetch (R-002). Opened files are parsed and schema-checked, never executed; zips are unpacked in memory |
| Host renderer CLI | `host/` (`hearthmap-render`) | Local files | Reads config and fixture JSON; writes PNG, or with `--check` only reports `validate()`'s issues |
| Calibration tool | `tools/calibrate/` | HA token supplied at run time | Reads HA history; writes proposed ranges into a config for review |

## Trust boundaries

- **Home Assistant -> panel.** The panel trusts HA's states as measurements. A lying or broken
  HA can make the map show wrong colours but cannot make core crash or misbehave: any
  non-numeric or non-finite state becomes `Unknown` (stale), and timestamps in the future are
  clamped (D-012). The panel sends nothing back to HA in v1.
- **Config -> codegen and core.** The config is trusted operator input, but it is still parsed
  defensively: schema first, then core's `validate()`. Codegen must never interpolate config
  strings into generated C++ without escaping (display names are arbitrary text).
- **Browser -> Home Assistant (editor).** The operator's own browser talks to their own HA with a
  token they paste in. The editor must never persist the token, include it in an export, or send
  it anywhere but the HA URL the operator typed.
- **This repo -> the world.** Everything here is public. The boundary that matters most is the
  one that keeps real-house data out (`AGENTS.md` §4, R-001).

## Cross-cutting flows

1. **Reading to pixels.** HA state -> component glue -> `Readings` -> `assess()` (freshness, then
   severity against the band; moisture through calibration to thirst) -> `build_view_model()` and
   `heat_samples()` -> `compute_heat_field()` -> colour lookup table -> `ui_lvgl`. Invariant: a
   value that is not Fresh never reaches the heat field or the HUD, and never looks fresh on a
   pin. On the device "Fresh" means "Home Assistant still reports it" (R-004).
2. **Config change.** Editor export -> private repo -> codegen (schema check, static tables, photo
   conversion) -> firmware -> OTA. Invariant: the schema, `config.h`, codegen and the editor agree
   on every field; a field known to one and not another is silently dropped somewhere. Rooms and
   pins travel in the floor plan's own frame all the way to the source; every `ConfigSource`
   (the host's JSON source, the generated tables' `load()`, the device's stepped boot) turns
   them by `canvas.map_rotation` with core's `apply_map_rotation()` once `validate()` accepts
   them, and nothing after that point knows a rotation exists (D-080). The editor draws the same
   turn itself, in TypeScript, for display and drag only; it never writes turned coordinates.
3. **Tap.** LVGL event -> `UiEvent` -> `apply()` -> `UiState` -> `build_view_model()` -> diffed
   redraw, in the same device `loop()` call as the tap, whatever the heat image is doing (D-047).
   Invariant: `UiState` is the only UI state; nothing in `ui_lvgl` remembers anything core would
   need to know. The embedder's `SourceStatus` (offline, re-stamped, heat image behind) is
   plumbing state, not UI state, and core never sees it (D-046).

## Risk register

| ID | Risk | Where it lives | Mitigation / status |
|---|---|---|---|
| R-001 | Real-house data (floor plan, entity ids, photos, hostnames, tokens) committed to this public repo or written into a PR, issue or commit message | Every PR | `AGENTS.md` §4 rule; fictional `sensor.example_*` example; `.gitignore` guards `private/` and `*.private.json` (a guard, not a guarantee). Pre-push grep is manual. GitHub secret scanning and push protection are enabled on the repo (checked 2026-11-20); they catch credential formats GitHub knows, not house data such as entity ids or a floor plan |
| R-002 | The editor persists or leaks an HA token | `editor/` (PR 6) | Token never enters the store, autosave, config or messages (`editor/src/lib/haClient.ts`). Enforced by `editor/test/haClient.test.ts` (a planted token is absent from errors, console, localStorage and the export) and the e2e smoke test (absent from localStorage, the status line and the browser console after a failed fetch) |
| R-003 | A photo id or other config string reaches a file path or generated code unescaped | `components/hearthmap` | Schema restricts photo ids to `^[a-z0-9][a-z0-9_-]{0,47}$` and ids to `^[a-z][a-z0-9_]{0,47}$`, and the build-time checker enforces the same patterns. Codegen escapes every config string it writes into C++ (`hearthmap_codegen.py`); enforced by `components/hearthmap/tests/test_codegen.py` (string escapes, a lone surrogate in a name refused, the config file name never reaching a comment, escaped member paths) and `test_cpp.py` (names with an embedded NUL survive generation and compile) |
| R-004 | Stale or missing data shown as current | `core` severity and view model; the device's re-stamping | Freshness is assessed before severity, and stale never feeds the heat field or HUD (D-005, D-009, D-012), enforced by `core/tests/severity_test.cpp` and `core/tests/view_model_test.cpp` (stale bindings have no pin value, never pulse a room, never enter the HUD or the heat samples). **Gap on the device (D-046):** values are re-stamped while Home Assistant is connected, so `stale_after_s` applies only while disconnected, and a probe that stops reporting without Home Assistant marking it `unavailable` looks fresh (issue 11). What the screen claims is kept honest instead: no "seen" time is shown for re-stamped values, and "Offline" is shown while disconnected (`ui_lvgl/tests/screen_test.cpp`) |
