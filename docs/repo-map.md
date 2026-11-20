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

As of PR 1 only `core/` headers, the schema and the example exist. Rows marked *planned* describe
the design in the build plan; each is corrected by the PR that builds it.

## Surfaces

| Surface | Path / entry point | Auth mechanism | Notes |
|---|---|---|---|
| Config file -> build | `components/hearthmap/__init__.py` (PR 7, planned) | None: the operator's own file, in their private repo | Validated against the schema before codegen. Photo ids are pattern-restricted so they cannot name a path outside `photos/` (R-003) |
| Home Assistant -> device | ESPHome native API subscription in `components/hearthmap` (PR 7, planned) | ESPHome API encryption key, configured in the private device YAML, never here | Readings enter core as `Readings`; non-numeric states become `Unknown` |
| Touch -> UI state | LVGL events -> `hearthmap::apply()` (PR 5, planned) | Physical access to the panel | Read-only UI in v1: a tap opens or closes an overlay or switches a tab, and can change nothing in Home Assistant |
| Browser editor | `editor/` (PR 6, planned), static, GitHub Pages | Optional HA long-lived token typed into the page | Token held in memory only (R-002) |
| Host renderer CLI | `host/` (PR 5, planned) | Local files | Reads config and fixture JSON; writes PNG |
| Calibration tool | `tools/calibrate/` (PR 8, planned) | HA token supplied at run time | Reads HA history; writes proposed ranges into a config for review |

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
   `heat_samples()` -> `compute_heat_field()` -> `colour_for()` -> `ui_lvgl`. Invariant: a value
   that is not Fresh never reaches the heat field or the HUD, and never looks fresh on a pin.
2. **Config change.** Editor export -> private repo -> codegen (schema check, static tables, photo
   conversion) -> firmware -> OTA. Invariant: the schema, `config.h`, codegen and the editor agree
   on every field; a field known to one and not another is silently dropped somewhere.
3. **Tap.** LVGL event -> `UiEvent` -> `apply()` -> `UiState` -> `build_view_model()` -> diffed
   redraw. Invariant: `UiState` is the only UI state; nothing in `ui_lvgl` remembers anything core
   would need to know.

## Risk register

| ID | Risk | Where it lives | Mitigation / status |
|---|---|---|---|
| R-001 | Real-house data (floor plan, entity ids, photos, hostnames, tokens) committed to this public repo or written into a PR, issue or commit message | Every PR | `AGENTS.md` §4 rule; fictional `sensor.example_*` example; `.gitignore` guards `private/` and `*.private.json` (a guard, not a guarantee). Pre-push grep is manual. Repo-level secret scanning and push protection: the operator's call, not yet confirmed on |
| R-002 | The editor persists or leaks an HA token | `editor/` (PR 6) | Designed: token in memory only, never in storage or exports. Stated, not enforced until PR 6 ships with a test for it |
| R-003 | A photo id or other config string reaches a file path or generated code unescaped | `components/hearthmap` (PR 7) | Schema restricts photo ids to `^[a-z0-9][a-z0-9_-]{0,47}$` and ids to `^[a-z][a-z0-9_]{0,47}$`; display names must be escaped by codegen (stated, not enforced until PR 7) |
| R-004 | Stale or missing data shown as current | `core` severity and view model | Freshness is assessed before severity, and stale never feeds the heat field or HUD (D-005, D-009, D-012). Enforced by PR 2–4 tests |
