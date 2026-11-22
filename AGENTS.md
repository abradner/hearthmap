# hearthmap — Agent Onboarding Guide

> Read this file first. It is the source of truth for AI agents working in this codebase.
> Plans and status live in GitHub issues and pull requests on this repo; design decisions and
> their rationale live in [`docs/decisions.md`](docs/decisions.md). When this file and a
> decision entry disagree, the decision entry wins on detail and this file wins on how to work
> here: reconcile them in the same change rather than leaving both standing.
> This file is operational: how to build, run, and write code here.

**This repository is public.** Everything committed, every PR body, comment and issue is
readable by anyone, forever (GitHub keeps force-pushed PR revisions reachable by SHA). §4's
public/private rule is the one most likely to be broken by accident.

## 1. What Is This?

A wall panel that shows a house as a heatmap built from Home Assistant sensors: a pin per sensor,
pulsing rooms where something is wrong, and a HUD column listing the plants that need water. The
target hardware is an ESP32-P4 board with a 10.1" capacitive touch screen used in landscape at
1280 x 800, running ESPHome with LVGL 9.6. Status: every part exists and is tested (core, the
LVGL UI, the host renderer, the pin editor, the ESPHome component, the calibration tool); the
component runs on the real panel (`README.md`, "Status", says which commit was measured). Each
directory's README describes what is there.

Domain nouns, several narrower than they read:

| Term | Means |
|---|---|
| **core** | The C++17 library in `core/`: pure functions, no dependencies. Not "the important part" in general |
| **layer** | One measured quantity drawn as a heat field: temperature, humidity, soil, air. One is shown at a time (a tab) |
| **pin** | One sensor location on the map. A pin may bind several Home Assistant entities, each in a **role** |
| **role** | What an entity measures on a pin (`temperature`, `moisture`, `co2`, ...). Roles, not kinds, decide which layers a pin feeds |
| **kind** | A pin's type (`temperature`, `humidity`, `soil`, `air_quality`, `plant`): its icon, its required role, HUD eligibility |
| **thirst** | Soil dryness 0..1 against the sensor's own calibration: 0 just watered, 1 bone dry. The soil layer's value |
| **band** | Per-layer comfort range plus warn/danger thresholds and colour-scale extent |
| **stale** | A reading that cannot be trusted: missing, `unavailable`, `unknown`, or older than the timeout. A severity, distinct from ok/warn/danger |
| **view model** | Everything the screen shows, computed by core from (config, readings, now, UI state) |
| **the example** | `examples/flat/`, a fictional flat. The only house config this repo ever holds |

## 2. Purpose & Principles

**Purpose:** the panel shows the house as it is, at a glance, and never shows a claim it cannot
back.

Principles: short, numbered, memorizable. Each one must be able to change a real decision — a
principle that never changes a decision is decoration. Principles 2–4 were drafted at init
without the operator; they are **provisional** until confirmed or replaced in review.

1. **Claims must hold.** A doc, comment, or commit message claiming a property the mechanism
   doesn't deliver is a bug, not documentation. Interim states say so in-place ("stated, not
   enforced — landing in a later PR"); stopgaps record the bar they miss. On this project the screen
   is a claim too: a stale sensor shows stale, an unmeasured room shows "no data", never an
   interpolated guess (`docs/decisions.md` D-005, D-012).
2. **If a test could check it, it lives in core.** Severity, thirst, interpolation, HUD order,
   hit-testing and UI state transitions are pure functions in `core/`, tested with `ctest`.
   `ui_lvgl` draws a view model; it does not decide. Logic found on the wall is logic nobody
   tested.
3. **Quiet when fine.** Inside the comfort band the map is neutral; only problems move or turn
   colour. A design that makes a healthy house look busy is wrong, however informative.
4. **Public by default, private by construction.** The code is public; the house is not. Real
   floor plans, entity ids and photos never enter this repo (§4), and the design keeps it that
   way structurally (config is data, supplied from elsewhere), not by vigilance alone.

### The tripwire protocol

A conflict with a principle is a stop-and-check, never something to quietly work around:

- If **your own approach** conflicts with a principle, treat that as a strong signal the approach
  is wrong. Re-derive it before proceeding — the principle has usually seen more failures than
  this session has.
- If the **operator's request** conflicts, say so explicitly, naming the principle. Then exactly
  one of two things is true:
  - **The principles have evolved.** Update the principle in the same change — and demand the
    justification: the *why* gets recorded next to the amendment, because an unjustified
    amendment is indistinguishable from erosion.
  - **The principles stand.** Then the request should be reconsidered — propose the nearest
    conforming alternative and let the operator choose with the conflict in view.
- Never proceed silently with a violation. An unremarked exception teaches every future reader —
  human or agent — that the principles are decorative (see §10, meta-rule 1).

## 3. Stack at a Glance

| Layer | Technology | Notes |
|---|---|---|
| Core library | C++17, CMake >= 3.21 | No dependencies. Warnings are errors (`cmake/HearthmapWarnings.cmake`). No exceptions, no RTTI (D-002) |
| UI | LVGL 9.6.0 | The version ESPHome 2026.11.0 pins; `ui_lvgl/` depends on core + LVGL only |
| Host renderer | C++17 + LVGL headless | `host/`: renders view models to PNG for golden-image tests; `--check` runs `validate()` on a config |
| Firmware | ESPHome 2026.11.0 external component | Python codegen (`components/hearthmap/__init__.py`) + thin C++ glue; tests: pytest, pinned in `components/hearthmap/tests/requirements.txt` |
| Editor | Vite + TypeScript, static | `editor/`: no backend, GitHub Pages; Vitest unit tests and a Playwright smoke test, exact versions in `package-lock.json` |
| Calibration tool | Python 3.11+, standard library only | `tools/calibrate/`; tests: pytest 9.2.0, pinned in `tools/calibrate/requirements-dev.txt` |
| Config contract | JSON Schema 2020-12 | `schema/hearthmap.schema.json`; validator `jsonschema==4.23.0` pinned in `schema/requirements.txt` |
| Testing | doctest v2.4.12 (commit-pinned, `cmake/HearthmapDoctest.cmake`) via `ctest`; Python `unittest` for the schema | |
| Formatting | clang-format 19.1.7 (`.clang-format`) | Pinned in CI |
| CI | GitHub Actions, GitHub-hosted runners only | See §7 |

## 4. Critical Architectural Rules

These are non-negotiable. Violating them will break things or be rejected in review.

### Dependencies point inward

```
        components/hearthmap   host
                 \           /
                  v         v
                   ui_lvgl ----> LVGL
                      |
                      v
                     core        (depends on nothing)
```

- `core/` depends on **nothing** beyond the C++17 standard library. It knows no LVGL, no
  ESPHome, no Home Assistant, no JSON, no clock, no file and no socket. Time arrives as a
  `Timestamp` argument, readings as a `Readings` value, config as a `Config` value.
- `ui_lvgl/` depends on core and LVGL only.
- `host/` and `components/hearthmap/` depend on core and `ui_lvgl` (and their own platform:
  a JSON parser and PNG writer on the host, ESPHome on the device).
- Nothing points outward: core never includes a `ui_lvgl` header, `ui_lvgl` never includes an
  ESPHome or host header.

Why: the same core and UI code must run in three places (unit tests, the host renderer, the
firmware), and only code with no platform in it can. It is also what makes principle 2 true: a
pure function of its inputs can be tested exhaustively on a laptop; a function that reads the
clock or the network cannot. Enforcement today is the CMake target graph (`hearthmap_core` links
nothing) plus review; a CI check that core includes only standard headers is a candidate
structural guarantee (§10 meta-rule 5).

### Real configs live in a private repo; only the fictional example lives here

The real house's floor plan, room names and layout, Home Assistant entity ids, plant photos,
hostnames, addresses and tokens are **never** committed here, nor written into a PR body,
comment, issue or commit message. They live in the operator's private configuration repo and are
compiled into the firmware there. This repo carries one example, `examples/flat/`: a fictional
flat whose entity ids all start `sensor.example_`.

- A new example, fixture or test house is invented, never derived from a real one.
- Nothing location-identifying: no town or street words, no real dates of absence, no names
  beyond the GitHub handle.
- Screenshots and golden images are rendered from the example, never from a real config.
- If you need to test against the real house, do it locally and commit nothing from it.
- Before every push, grep the diff for anything that looks like a real hostname, address, entity
  id or name. A force-push does not un-publish: GitHub keeps every revision a PR pointed at.

### The schema is the contract

`schema/hearthmap.schema.json` is the single definition of a config. The editor exports it, the
component compiles it, the host parses it, and `core/include/hearthmap/config.h` mirrors it.
Change all four in the same PR, plus `schema/README.md`, the example and `schema/test_schema.py`.
A field added in one place only is a contract break that no single test will catch.

### Core's public headers are the interface between builders

`core/include/hearthmap/*.h` state behaviour precisely (boundaries, tie-breaks, degenerate
inputs) because several implementers work against them in parallel. An implementation that
needs a different behaviour changes the header's contract first, visibly, and records why in
`docs/decisions.md`; it never quietly diverges from the doc comment.

### ESPHome and Home Assistant facts are verified, not remembered

Both move faster than any model's training data. Before writing anything that depends on an
ESPHome component schema, the native API or LVGL integration details, check the pinned release's
docs or source. A vendor blog or summary is a hypothesis until the real tool's `--help` or
source agrees.

## 5. Repository Map

`docs/repo-map.md` is the living boundary map: surfaces, trust boundaries, cross-cutting flows,
and the risk register. It exists because that knowledge lives *between* files and can't be
reconstructed by grep — and because cold-started subagents and future sessions get no
conversation context, only what's written down.

- **Read it before touching anything boundary-sensitive**: auth, tenancy, deletion/visibility,
  serialization, background delivery, routing, any external surface.
- **Update it in the same PR** as any change that moves an edge on it. Never leave it describing
  a stale security boundary — a wrong map is worse than no map, because it is trusted.
- Cite risk-register IDs (R-00N) from rules, PRs, and code comments, so the register stays
  load-bearing instead of decorative.

## 6. Development Essentials

Needs CMake >= 3.21, a C++17 compiler (Apple clang and GCC are both exercised), Python >= 3.10,
and network access on first configure (doctest is fetched at a pinned commit).

`mise.toml` installs the local toolchain (Python 3.13, clang-format 19.1.7, CMake 4 and
Node 24, the same major as `editor/.nvmrc`) and names the common commands as tasks: `mise trust && mise install`
once per checkout, `mise run setup` for the Python test dependencies, `mise tasks` for the rest.
The C++ compiler is the system's. CI does not read `mise.toml`: it pins its own tools in
`.github/workflows/ci.yml`, so a version changes in both places in one commit. The commands
below are what the tasks run, for a machine without mise.

```sh
# Build and run core's tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# Also build ui_lvgl, the host renderer and the golden images (fetches LVGL 9.6.0, nlohmann/json
# and lodepng at pinned hashes; host/README.md says how to regenerate a golden)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DHEARTHMAP_BUILD_UI=ON
cmake --build build && ctest --test-dir build --output-on-failure

# Schema: validate the examples, then the negative controls
python3 -m venv .venv && .venv/bin/pip install -r schema/requirements.txt
.venv/bin/python schema/validate.py
.venv/bin/python schema/test_schema.py

# Formatting (clang-format 19.1.7; CI fails on any diff)
git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run --Werror
# fix in place:
git ls-files '*.h' '*.cpp' | xargs clang-format -i
```

Inside a trusted checkout `mise.toml` selects clang-format 19.1.7 (`mise run format`,
`mise run format:fix`). Outside one, name the version explicitly (for example
`mise exec clang-format@19.1.7 -- clang-format ...`); a different major formats differently and
CI will disagree with you. `pip install clang-format==19.1.7` into the venv also works.

Adding a core source file: list it in `HEARTHMAP_CORE_SOURCES` in `core/CMakeLists.txt`; adding
a public header: list it in `HEARTHMAP_CORE_HEADERS` there too, which also compiles it alone
under the strict warnings to prove it is self-contained. Adding a test file: list it in
`core/tests/CMakeLists.txt`.

### 6.1 Known Environment Gotchas

Things that cost real time to discover once — don't rediscover them. Add entries as they're found;
add rather than rewrite.

- **A version-manager shim with no version selected fails as if the tool were broken.** On a dev
  machine with clang-format managed by `mise`, a bare `clang-format` printed `mise ERROR No version
  is set for shim: clang-format` (2026-11-20). Select the pinned version explicitly (§6).
  Since 2026-11-22 `mise.toml` selects it inside this repo, once the checkout is trusted
  (`mise trust`); an untrusted checkout still fails the same way.
- **`npm ci` in `editor/` refuses any Node but 24** (`EBADENGINE`, seen 2026-11-22 with a global
  Node 26). `mise.toml` pins Node 24; run `mise install` once, or use `mise run editor`.
- **A Homebrew tool can still shadow mise's inside an already-activated shell.** Seen 2026-11-22
  in an agent's shell: `mise exec` and `mise run` put the pinned tools *after* `/opt/homebrew/bin`
  on `PATH`, so `npm` ran Homebrew's Node 26 and the editor task failed, while the same task
  passed from a clean environment (`env -i HOME=$HOME PATH=/opt/homebrew/bin:/usr/bin:/bin mise run
  editor:check`). The editor tasks now check Node's major first and say so. If any task reports
  the wrong version, print `PATH` inside `mise exec -- sh -c` before blaming `mise.toml`.
- **CMake 4 refuses projects that declare `cmake_minimum_required` below 3.5**, which includes
  some third-party CMakeLists. doctest is consumed header-only for that reason
  (`SOURCE_SUBDIR` in `cmake/HearthmapDoctest.cmake` keeps its CMakeLists out of the build); do
  the same for any other old dependency rather than lowering policy settings globally.
- **ESPHome's LVGL compiles only some software-renderer colour formats.** For the panel it enables
  RGB565, ARGB8888 and RGB888; scaling or rotating an RGB565 image also needs RGB565A8, so such an
  image logs "Color format 0x12 is not enabled" and is not drawn (D-039). `host/lvgl/lv_conf.h`
  mirrors the panel's set so host renders take the same paths; keep the two in step when ESPHome
  or the YAML changes what it enables.
- **The ESP32-P4 has no double-precision hardware.** Every `double` operation is a software
  routine of tens to hundreds of cycles, so code that is instant on a laptop can take seconds on
  the panel. The first hardware run boot-looped on the task watchdog because a per-pixel
  `point_in_polygon()` loop (double) ran inside `setup()` (D-038). Keep `double` out of per-pixel
  and per-cell loops, never do unbounded work in a component's `setup()`, and check new device
  paths with the pipeline benchmark in `components/hearthmap/tests/test_cpp.py`.
- **The PIE (xesppie) assembler takes address operands only in x8-x15.** `esp.vld.128.ip q0, a6, 16`
  fails with "illegal operands", and GCC 14 here has no `cr` constraint to ask for those registers;
  pin the pointers with local register variables (`register T* p asm("a0")`), as
  `tools/bench/components/hm_bench/kernels.cpp` does. `esp.vld.128` also ignores the low four
  address bits, so an unaligned pointer reads the wrong pixels rather than faulting.
- **LVGL 9.6's PPA draw unit draws nothing when it cannot use a buffer.** For a draw buffer whose
  start or size is not whole 64-byte lines it logs `ppa_core: out.buffer addr or out.buffer_size
  not aligned` and leaves the area untouched (no software fallback); it also reads flash images at
  4 Mpx/s. Measured with `tools/bench` (D-060), which is why the unit is not enabled.
- **The PPA's scaler interpolates.** A 2x `ppa_do_scale_rotate_mirror` is not pixel doubling (up to
  21 steps per channel away), so it cannot stand in for `Blitter::scale2x` (D-057).
- **ESPHome prints other tasks' log lines from the main loop.** `ESP_LOGx` from any task but the
  main one goes into a ring buffer that the logger's `loop()` drains, so a task reporting that the
  main loop is stuck is never heard until it recovers. Write such lines with `esp_rom_printf`, as
  `components/hearthmap/stall_monitor.cpp` does (D-062).
- **With `esp32_hosted`, a Wi-Fi call can block the main loop for the whole RPC timeout.** It is
  5 s by default, equal to ESPHome's default task watchdog, so a silent co-processor resets the
  panel; the component requires `esp32: watchdog_timeout` of at least 16 s (D-061).
- **ESPHome compiles ESP-IDF's log at ERROR, which hides esp-hosted's warnings.** `esp32:
  framework: log_level` defaults to `ERROR` (CONFIG_LOG_DEFAULT_LEVEL_ERROR), and esp-hosted
  3.1.0 reports a P4 out of DMA-capable RAM only at WARN or DEBUG (`mempool OOM start (TX)`,
  `dma_alloc(...) failed`, `rx_get_buffer(...) failed`) while it drops the packet. A link that
  dies that way logs nothing at all. `hearthmap: diagnostics: true` opens them (D-063); otherwise
  set `log_level: WARN` before reading a quiet log as "no Wi-Fi error" (the example does, D-066).
- **ESPHome's LVGL allocator moves to internal RAM for good after one PSRAM failure.**
  `lv_malloc_core()` in ESPHome 2026.11.0's `lvgl_esphome.cpp` asks for PSRAM first, but on one
  failure it changes its static `cap_bits` to plain 8-bit memory and never goes back, so every
  later LVGL object and draw buffer competes for internal RAM. A boot that once ran PSRAM dry
  starves the Wi-Fi link afterwards; the internal-RAM floor's warning (D-064) is where it shows.
- **`operator new` aborts on this target.** Exceptions are off, so a `std::vector` that cannot
  allocate calls `abort()` on every boot that reaches it; the present self-check did this with
  internal RAM full. Large pixel buffers go in PSRAM through `heap_caps_malloc`, checked.
  (Since D-064, the UI's go through the memory port, `hearthmap/ui/memory.h`, instead.)
- **Small allocations go to internal RAM under `CONFIG_SPIRAM_USE_MALLOC`.** Only blocks over
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16 KiB) go to PSRAM; everything smaller is tried in
  internal RAM first. A UI that makes many 4-12 KiB buffers (sprite captures, coverage tables,
  frames) fills internal RAM, and on the ESP32-P4 that starves DMA users such as the hosted
  Wi-Fi driver, whose frames cannot come from PSRAM (ESP-IDF 5.5.6 registers the P4's PSRAM
  heap without `MALLOC_CAP_DMA`, D-063). The link then dies silently while the panel runs on. Anything ui_lvgl sizes by pixels,
  cells or pulses goes through the memory port (D-064); `host/tests/memory_budget_test.cpp`
  fails if it does not. And no reset is not a live link: a quiet panel with no watchdog resets
  can still have lost its network (D-061's correction).
- **A largest-free-block query can flash the whole panel blue.** `heap_caps_get_largest_free_block`
  (and `heap_caps_get_info`) walk every block of every matching heap with that heap's spinlock
  held: interrupts off on the calling core for the whole walk, which on the PSRAM heap means
  thousands of headers through the cache. ESP-IDF 5.5.6's DPI panel re-arms its frame DMA from an
  interrupt at the end of every frame (`mipi_dsi_dma_trans_done_cb`); held off past the vertical
  blanking, the DSI bridge's FIFO runs dry and it sends its reserved pixel (a blue) for the frame.
  The driver reports it only with `esp_rom_printf` ("underrun happens"), never in ESPHome's log.
  Read free bytes with `heap_caps_get_free_size` / `heap_caps_get_minimum_free_size` (counters,
  no lock); keep walks out of anything periodic (D-067). Nothing counts underruns: a console
  tap was tried and never installed on the panel (D-068); the line shows on ESP-IDF's primary
  console only.

## 7. CI & Deployment

`.github/workflows/ci.yml` runs on every pull request and on pushes to `main`, on GitHub-hosted
runners only (a public repo must never use self-hosted runners: anyone can open a PR). Jobs:

- **core**: configure, build and `ctest` on ubuntu-latest with GCC and with Clang. The ctest
  entry fails if doctest reports zero test cases.
- **ui-host**: the same with `-DHEARTHMAP_BUILD_UI=ON`, on GCC and Clang: ui_lvgl's and the
  host's tests and the golden images, compared exactly. On failure the actual and diff images
  are uploaded as an artifact.
- **editor**: in `editor/` with Node 24, `npm ci` from the lockfile, then lint, typecheck, Vitest
  unit tests (including `schema/semantic-cases.json`, shared with core), the production build
  and the Playwright smoke test in Chromium (report uploaded on failure).
- **tsan**: ui_lvgl's tests built with ThreadSanitizer (Clang), running every case named
  `worker:*`: the worker hand-off on a real second thread (D-069). A race report fails it.
- **schema**: installs the pinned validator, runs `schema/validate.py` (every example config and
  fixture, plus cross-references) and `schema/test_schema.py` (mutations that must be rejected).
- **calibrate**: `pytest` for `tools/calibrate/` on Python 3.11 and 3.13, with the pinned
  `tools/calibrate/requirements-dev.txt`.
- **format**: `clang-format --dry-run --Werror` with clang-format 19.1.7 from PyPI.
- **component-codegen**: pytest over `components/hearthmap/tests` (the ESPHome component's
  generator: schema checker against `jsonschema`, defaults and present bits, photos, the library
  copy, and C++ checks linking core's real `validate()` against generated tables, plus the device
  pipeline's per-step bounds and its heat-rebuild policy (D-038, D-047);
  `HEARTHMAP_REQUIRE_CORE=1` makes those fail rather than skip).
- **esphome-config**: `esphome config` on `examples/flat/esphome/flat-panel.yaml` with the
  digest-pinned `ghcr.io/esphome/esphome:2026.11.0` image, plus a negative control (a broken config
  must fail it). A full firmware compile is the manual `esphome-compile` workflow
  (`workflow_dispatch`), too slow for every PR; it has not been run on `main` yet (issue
  23), so the firmware compile is checked by hand, with the same pinned image, before a PR
  that changes device code is opened.

Every action is pinned by commit SHA. There are no path filters: every job runs on every PR.

Deployment: none from this repo. The firmware is built and flashed from the operator's private
configuration repo, which pins this repo by tag. **A merge is not a release**: merged work
reaches no panel until a `vX.Y.Z` tag exists and the private repo moves its pin. Tags are cut only
on an operator go-ahead.

Universal cautions, whatever the pipeline:

- **A skipped job is not a passing job.** A green run where path filtering skipped half the suite
  means that half never saw the commit. Check what actually ran before trusting the check.
- **A test job that cannot distinguish "everything passed" from "nothing ran" is not a gate.**
  When a test runner is swapped, changing the local command is half the job: read what CI actually
  invokes and confirm the run count it reports is non-zero. (A generated workflow in a sibling
  repo kept running the abandoned runner against an empty directory — green on every push while
  executing zero examples, because "no tests found" was a success to it.) Applies equally to any
  generated pipeline adopted wholesale.
- **A PR gate fails only for reasons inside the diff — with one carve-out, named here so it does
  not get removed by someone applying this rule literally.** Dependency *currency* belongs to
  scheduled tooling (dependabot, staleness checks), not a PR gate: nothing should redden a PR that
  changed no dependencies merely because someone else shipped a release. A gate that cries wolf
  gets ignored, including on the day it is right. (Instance: a generated scanner wrapper passed
  `--ensure-latest`, so an upstream release turned every PR in a sibling repo red while reporting a
  version fact through the channel reserved for security failures.)
  - **A vulnerability disclosure is not staleness.** A scanner that fails because a *newly
    disclosed CVE* affects an already-pinned dependency stays a PR gate on purpose: merging a
    known-vulnerable dependency is unsafe whether or not this diff introduced it. This repo has no
    dependency scanner yet; when one is added, name it here. The test is "is there a known defect in what we ship" (gate it)
    versus "is there a newer version" (schedule it).

## 8. Working Rules

The portable core. These rules are pre-filled from hard-won convention across this repo's sibling
projects; adjust only with reason, and record the reason (see §10).

### Planning & approval

- Propose an implementation plan for any moderately or highly complex change, and get it reviewed
  and approved before making edits. Don't dive into large builds or refactors on your own read of
  the situation.
- For anything genuinely ambiguous or not yet decided by the operator, prefer the reversible
  option and leave a clear marker rather than picking silently. Two-way doors over one-way doors.
- **Building structure where no convention is stated is a decision, not a default.** When a change
  introduces more than trivial structure (a new page/screen, a new service layer, a new module
  shape) and §4 states no convention for that surface, do not silently invent one — and do not
  treat "match the surrounding code" as cover: over undesigned code that instruction *propagates*
  the blur. Adopt a pattern, then name it in the PR body as an explicit proposal ("no stated
  convention for X; this PR uses Y; codify or correct"). The operator's review then decides a
  standard once, instead of re-litigating shape per PR. (Motivating instance: a sibling repo's
  first UI-heavy PR mixed layout, scaffolding, and domain components with no container/presenter
  separation — the agent had nothing to follow, faithfully extended the existing blur, and the
  standard got stated for the first time as a review comment, the most expensive place to state
  it.)

### Landing changes

- **Every change lands through a pull request** — including one-line copy tweaks. Size is not the
  criterion. (Motivating instance: a few commits went straight to `main` during a sibling repo's
  init. Automated reviewers fire on the ready-for-review edge, so a direct push is a change nothing
  reviewed, and `main`'s history stops meaning "reviewed states".)
- Opening a PR is not merging it. Push permission is granted per session — ask once before the
  first push unless already told.

### Destructive & outward-facing actions

- Destructive actions (dropping data, deleting files you didn't just create, rewriting published
  history) and outward-facing actions (publishing, sending, deploying) need explicit approval,
  every time. Approval for one instance is not approval for the next.
- **Merging needs the owner's go-ahead.** A completed review, green CI or a finished followup is
  not authorization: report readiness and stop. On the go-ahead, bind the merge to the head that
  was reviewed (`gh pr merge <n> --squash --match-head-commit <sha>`; `docs/pr-review-machinery.md`
  §6), so a push after the check cannot be merged unread.
- Never force-push or rewrite published history without an explicit, current go-ahead.
- **Visibility and content are separate axes.** Before making anything public, do not let a narrow
  confirmation (repo name, a visibility toggle) stand in for consent to the *content* — git
  history, comments, and design docs go out too. Separately flag anything describing a
  still-unfixed vulnerability or written more candidly than the operator likely pictured, and ask
  about that specifically: published history cannot be taken back.
- If you encounter a violation of a safety rule already committed (a plaintext secret, a
  destructive migration lying in wait), flag it immediately — finding it is not the same as
  having caused it, and silence helps nobody.

### Tooling version floors

- **A version floor is an interrupt, not a workaround.** When a skill or workflow depends on
  tooling at or above some version and the environment is below it, stop and tell the operator
  what to upgrade. Do not silently take a degraded path, reimplement the missing capability by
  hand, or work around it — the operator can fix an install in seconds, and the workaround is
  what ends up load-bearing and unreviewed.
- Distinguish **fixable** from **unavailable**. A missing or outdated tool is fixable: interrupt.
  A capability the platform genuinely doesn't offer here — wrong host, feature not enabled for
  this repo, a documented limit — is unavailable: take the documented fallback and record why.
- Name the floor and the exact upgrade command when you interrupt. "Your `gh` is too old" costs
  the operator a search; "`gh extension upgrade stack` — `merge` landed in v0.1.0" does not.
- The same applies mid-run: if tooling turns out to be below the floor after work has started,
  stop and say so rather than finishing on the degraded path and reporting success.
- **Adding a dependency can raise the project's floor without asking.** Package managers resolve a
  new dep's own requirements by bumping yours. After any dependency add, read the manifest diff for
  the language/toolchain lines specifically; if a dep forced a bump, pin the *dep* to the newest
  version whose floor matches the repo rather than raising the repo. A toolchain bump touches the
  production image and is a deliberate decision, not a side effect of installing something.
  (Instance: `go get <dep>@latest` rewrote `go.mod`'s Go version and deleted the `toolchain` pin,
  breaking the Docker build against a pinned base image. The `test` job still passed — only `build`
  caught it.)

### Configuration

- Fail fast on boot: never provide fallback defaults when reading *required* configuration.
  Missing required config must raise at startup rather than silently degrade. Defaults are fine
  for genuinely optional tuning knobs. (This isn't hypothetical: a sibling repo baked a stand-in
  value into an image-wide ENV to make a build step pass, which would have silently defeated this
  rule in production. If a build step needs a stand-in, scope it to that step, never image-wide.)
- The same explicitness applies to what the code writes: anything persisted that holds data states
  its permissions explicitly rather than inheriting the process umask.

### Review feedback

- **A finding is a claim, not a verdict.** Whether it comes from a bot, a human, or your own
  earlier session: trace or reproduce it before acting on it. Ask whether the flagged path is
  actually reachable. When a finding says code and docs disagree, work out which end is wrong
  before "fixing" either. Declining findings has to actually happen — a round that accepts every
  finding is a warning sign, not a good score.
- **Verify a delegated claim against the artifact before relaying or acting on it.** Read the
  diff, grep the branch, run the command — a subagent's report is a claim like any other. (Two
  agents once filed contradictory security reports and *both were correct about different
  artifacts*; only diffing them resolved it, and doing so exposed a real defect — a branch that
  deleted a control introduced by the PR below it, which the final merge accidentally restored.
  Merged-result-correct but per-PR-wrong is exactly what a reviewer catches and loses trust over.
  Separately, an agent has returned a placeholder summary while having done complete, correct work:
  believing the report would have discarded it.)
- Reject suggestions that violate the rules in this file, and say why. Automated reviewers read
  this file too; that's expected — reviewer-side agents should review fully as normal, and rules
  here that bind only author-side agents say so explicitly.

### Testing & verification

- **Verify the output, not the instrument.** Green means nothing threw, and nothing more. Before
  claiming something works, name the artifact it should have produced and go look at it — the
  rendered page, the written file, the actual rows. A tool reporting on itself is not the
  artifact: `git bundle verify` once passed on backup bundles that could not actually be cloned,
  because the verifier checks internal structure, not that the bundle can reconstitute a repo —
  the suite that replaced it performs a real clone.
- **Prove a new test can fail.** For any bug fix: write the regression test, confirm it passes
  with the fix, revert just the fix, confirm the test fails for the right reason, restore the fix.
  A test that was never seen to fail hasn't proven anything. This is part of writing the fix, not
  review debt to defer — one reviewer's "add regression coverage for your own fix" finding recurred
  three times across a single PR series and was right every time. And the mutated code must
  *compile*: a build-failed mutant proves nothing (re-learned three times in one night).
  - **A revert that doesn't fail doesn't mean the test is good — diagnose which of these it is.**
    The single highest-yield habit this rule has: it has caught a hollow test five times across
    two sibling repos, and every time the test looked convincing first.
    1. **Two independent guards.** Reverting one leaves the other covering it, so the test still
       passes. Revert *all* of them, or the proof is vacuous.
    2. **The test never reaches the mechanism.** Ordering or setup means the code under test
       doesn't run at all — a stale-state test that refreshes first is never stale; a persistence
       test that never saves has nothing to lose. A later fix can do this to a test that used to
       work, so re-prove a test after changing its setup.
    3. **The lever is a no-op.** The thing you changed to reproduce the failure doesn't affect the
       output. Confirm the lever moves the output *before* concluding anything from it.
    4. **False positives are masking real ones.** A check that flags noise gets tuned until it's
       quiet, and the tuning hides the signal. Look at what a guard *stops* flagging.
    5. **A sequential test can't see a concurrent defect.** Races need real threads on separate
       connections; a single-threaded test passes regardless.
- **For any isolation or authorization test, name the attacker and write *their* request.** A spec
  that asserts the mechanism's own definition back at itself cannot fail for any input — it reads
  like coverage and gates nothing. If you cannot describe an input that would make the assertion
  fail, the test is documentation, not a gate. A test that builds its data through the mechanism
  it claims to test is the usual way this goes wrong.
- **Inference from a plausible nearby cause is not diagnosis.** Wait for the real signal and read
  the actual failure before naming a cause. The incident and the review-time form of this rule live
  in `docs/pr-review-machinery.md` §5 — that file is canonical for it; don't restate it here.
- **Watch the setup, not just the assertion.** Four tests in one feature passed against genuinely
  broken code because their setup could never exercise the branch — an assertion on node count
  only, a fixed `now` so the fade never completed either way, a payload rejected as malformed
  before its size mattered. Also beware a test that captures current behavior so faithfully it
  archives the defect.
- Ask what else satisfies your assertion — a count-based check that an empty-state row also
  matches, a visibility check that passes for a scrolled-away element. When asserting absence,
  include a positive control so a broken probe can't read as success.
- Every conditional branch that encodes real logic gets a test that exercises it — especially the
  rare/edge branches, not just the happy path.

### Git hygiene in shared checkouts

- **Create branches only from an explicit start point** (`git checkout -b <name> origin/main`) when
  anything else might be operating in the same checkout. Use `-b`, which fails if the name is
  taken, not `-B`, which silently resets someone else's branch of that name; reset a branch only
  as a separate step, once its work is known to be disposable. Check `git branch --show-current`
  before any operation that depends on HEAD. (A "one-line docs PR" silently inherited five feature
  commits from an in-repo builder agent's branch and was reviewed in that state. "Only one builder
  running" is not "only one git user"; prefer worktree isolation for delegated builders.)
- **Never pair `git stash` with a later `pop` unless the stash verifiably created an entry.** A
  stash on a clean tree stashes nothing, and the paired pop then pops whatever stranger's entry was
  on top of a stack you don't control. For "test against a clean copy", use `git show REV:path` or
  a scratch worktree instead of stashing at all.

### Layered checks

- Apply the **deletion test** to any validation that exists in more than one place: enforcement
  exists exactly once; anything layered on top must, if deleted, change only politeness (a
  friendly error instead of an ugly one), never possibility. If deleting either check would make a
  new state possible, you have enforcement in two shapes, and they will drift.

### Documentation & discovered work

- Docs describing a boundary or behavior change in the same PR as the change. A doc describing a
  boundary that no longer exists is worse than none, because it is trusted.
- Log discovered work (bugs found in passing, deferred improvements) as a GitHub issue on this
  repo (public: nothing private in it), or as a `docs/decisions.md` entry when it is a design
  question — never a PR body or a code comment. A triage table in a PR body goes stale between rounds and
  vanishes on merge.
- When you fix a subtle bug or get burned by a non-obvious behavior, write the general form of the
  lesson into §9 before moving on — same session, not later.
- **A review comment that names a missing standard forks.** "We need a convention for X" /
  "this pattern was never established" is not an instance finding — resolving it means both
  fixing the instance *and* codifying the standard into §4 (or the doc §4 points to) in the same
  round; if the standard needs real design time, open a GitHub issue with the review link.
  A convention stated only in a review thread doesn't exist for the next session — the thread
  dies with the PR, and the next agent re-improvises. (This is §9's ratchet, aimed at
  conventions: incidents accrete into gotchas, review-discovered standards accrete into §4.)

### Tooling

- Use the agent's structured file tools rather than `cat`/`sed`/shell here-docs for inspecting
  and modifying files during a session. (Scope: this governs session file operations; committed
  shell scripts do what shell scripts do.)

### Which shipping flow

- The default is one PR at a time: react to feedback immediately, merge when green. Use
  `.claude/skills/single-pr` — it makes that default rigorous rather than merely simple.
- Several PRs open together as one body of work is a different regime: use
  `.claude/skills/batch-review` (fan out, feedback write-only until synthesis, one followup PR).
  It is opt-in for genuine multi-PR fan-outs, not a replacement for the default. The tell is
  reviewer attention fragmenting across live threads, not the size of the diff.
- Both skills read `docs/pr-review-machinery.md` for the parts that don't differ between them —
  reviewer triggers, the three-surface comment harvest, triage, the round cap, and the green-signal
  traps. It is the canonical copy; don't restate it in a skill, and don't let a skill contradict it.
- When more than one branch is in flight against the same code — or any branch went through an
  agent-performed merge or rebase — run `.claude/skills/stack-integration-check` before opening
  PRs. Per-branch review is structurally blind to what happens between branches; this is the check
  that runs on the combination.

### Context & compaction

- When the operator signals they are near the context limit and about to compact, use
  `.claude/skills/park-context` rather than improvising a summary. Compaction keeps a paraphrase
  and discards the transcript, so the park writes only what compaction destroys — intent, rejected
  alternatives, what was actually verified versus assumed, what was mid-flight when the turn was
  cut — and never the diff, which is reconstructible.
- Do not finish work, commit, or push while parking. Parking is triggered by interrupting a live
  turn, so the tree may hold a partial edit nobody intended; record it as observed and stop.
- Resuming from a handoff uses `.claude/skills/resume-context`. The handoff is a claim, not a
  verdict (see Review feedback above) — the session that wrote it is gone and cannot be asked what
  it meant. Verify its state claims against the repo and report drift before building on them.
- Durable lessons never live in a handoff. They land in §9 / §6.1, or in `docs/decisions.md` / a GitHub
  issue, before the work merges — handoff files are gitignored local state and get deleted.

## 9. Gotchas & Lessons Learned

Numbered, accreting. Add to it rather than rewriting it — the numbering is stable so entries can
be cited from code comments and PR descriptions.

Entry shape: **what happened → the actual root cause → the general rule → where the regression
test lives** (if one exists).

1. **"Seen just now" on every sensor, and an inert staleness timeout.** The device re-stamps
   every value while Home Assistant is connected (ruling R8, so steady sensors do not age into
   stale), and the overlay printed "seen N min ago" from that same timestamp, so it always read
   "just now". Root cause: a value the glue synthesises was passed on looking like one the
   source reported, and the UI could not tell. General rule: when an embedder fabricates or
   rewrites a field to satisfy one consumer, every other consumer of that field must be told, or
   must not read it; say in the field's doc what it means on each platform. Regression test:
   `ui_lvgl/tests/screen_test.cpp`, "re-stamped readings show no seen time" (D-046).
2. **Chatty sensors starved the screen.** On the panel a heat rebuild takes about a second, and
   one was started for every changed reading every 500 ms, with taps applied only between
   rebuilds, so the pipeline never caught up and taps waited. It was invisible on the host,
   where a rebuild takes milliseconds. Root cause: one pipeline served two jobs with different
   costs and deadlines (a cheap view that must follow taps at once, an expensive image that can
   lag). General rule: on the device, split work by deadline, not by data flow; give the
   expensive path its own budget, and test the policy (what triggers it, how often) on the host
   with a fake clock, since the host's speed hides the failure. Regression test:
   `components/hearthmap/tests/test_cpp.py::test_heat_rebuild_policy_and_immediate_taps`
   (D-047).
3. **The panel spent three quarters of its time redrawing a pulse.** Rooms pulsed by animating
   the opacity of one `lv_line` per room, and on the panel LVGL refreshed 13 times a second at
   about 60 ms each, leaving the main loop (and touch) 13 passes a second. Root cause: LVGL 9.6
   sizes a line object from its own origin (0, 0) to its farthest point, so a polyline placed at
   the map's origin covered everything up to the room's far corner, and every animation frame
   re-rendered that whole rectangle in software; the host renders it in microseconds, so nothing
   showed until the device's perf log. General rule: on the panel, cost is pixels invalidated per
   second; know each animated object's real area (`lv_obj_get_coords`), animate in steps on a
   shared clock rather than per frame, and keep a host budget on what LVGL is asked to draw.
   Regression tests: `host/tests/perf_proxy_test.cpp` and the pulse cases in
   `ui_lvgl/tests/screen_test.cpp` (D-051).
4. **A PPA call silently threw away pixels the CPU had just drawn.** The bench wrote a whole
   buffer from the CPU, then had the PPA's scale-rotate engine copy a 64 x 64 block into it; of
   the 16,384 CPU-written pixels in the same rows but outside the block, 15,840 to 16,228 were
   gone. Root cause: ESP-IDF 5.5's PPA driver writes its *inputs* back from the cache, but for an
   output it does not also read it only *invalidates* the output rows' cache lines, which
   discards CPU writes not yet written back (`esp_driver_ppa/src/ppa_srm.c`; the blend engine's
   output is the same when the destination is not also the background). General rule: before
   handing memory the CPU has written to a DMA engine as an *output*, write the affected rows back
   yourself; never rely on a driver's cache handling for bytes outside the block it was given.
   Regression check: `tools/bench` (`bench cache_trap`, raw and through `PpaBlitter`, which
   writes the rows back first); there is no host test because the host has no such cache (D-057).
5. **Animated pulses killed the Wi-Fi link, and fixing the resets hid it.** Once the pulse sprites
   were built, the panel's hosted Wi-Fi link died a few minutes after boot. First it showed as
   task-watchdog resets; D-061 lengthened the watchdog, the resets stopped, and the panel then ran
   on for hours with no network and nothing in the log. The diagnostic build (D-063) showed
   internal RAM falling from 236 KiB to 1 KiB within 20 s of the sprite build starting, then
   thousands of failed 1536 B DMA allocations: the sprite code's many 4-12 KiB `std::vector`s
   had gone to internal RAM under `CONFIG_SPIRAM_USE_MALLOC` (16 KiB threshold), and the hosted
   driver's frames can only come from there. On the host nothing showed: memory is plentiful and
   undivided. Root causes: allocation placement left to a global threshold, and a symptom (the
   reset) treated as the fault. General rules: on a target with a split heap, decide where every
   bulk buffer lives, through one port the embedder owns, and test the default heap's budget on
   the host, where the failure cannot happen by itself; and when a fix removes a symptom, check
   the function it was a symptom of (the link), not just the symptom's absence. Regression tests:
   `host/tests/memory_budget_test.cpp` (failed before the port: +1.59 MB in small blocks over the
   walk) and `components/hearthmap/tests/test_cpp.py::test_internal_ram_guard` (D-064).

## 10. Maintaining This Document

Meta-rules for editing this file — they exist because each was violated somewhere first:

1. **A rule everyone knows is wrong is worse than no rule** — it teaches agents that the rules are
   decorative, which devalues the ones that are load-bearing. When reality has moved, revise the
   rule openly: state what the old rule said, why it was the wrong shape, and what replaces it.
2. **Ground rules in incidents.** When adding a rule, say what happened. When correcting a wrong
   rule, record the correction in-place so the old rule doesn't get quietly reintroduced.
3. **Scope absolutes.** Name the exceptions at authoring time, or agents will either violate the
   rule or over-apply it.
4. **Name the audience** when a rule doesn't bind every reader — author-side agents, reviewer
   bots, and humans all read this same file.
5. **Prefer structural guarantees to written claims.** If a rule can be made mechanically true (a
   build-context exclusion, a CI gate, an import stub instead of a second copy), do that and
   document the mechanism, rather than adding another sentence.
6. **Expect to compress.** First-draft steering prose runs vague and grandiose; cut it to concrete
   nouns and commands the same day. When compressing an established doc, treat it like a refactor:
   diff the imperatives before/after and confirm nothing operative was dropped.
7. **Porting rules between sibling repos is judgment, not copying.** Verify each candidate rule
   against this repo's own files, CI, and conventions; adopt, adapt, or reject explicitly — and
   note rejections in the Adaptation Record below. Watch especially for rules that silently assume
   another repo's merge strategy, bot roster, or deploy shape.

## 11. Adaptation Record

What was pruned or changed from the steering template this repo was initialised from, and why —
kept in the doc (not just a commit message) so future readers don't need git archaeology to know
what was deliberately excluded. The template is not public, so this record names it only
generically.

Initialised 2026-11-20 by an unattended agent run, from an approved build plan, with no operator
available for the template's interview. Every interview answer below was taken from that plan
or, where the plan was silent, chosen as the reversible option and marked provisional.

**Interview answers (from the plan, or provisional)**

- Project: hearthmap, greenfield. Purpose and principles 2–4: drafted from the plan,
  provisional (§2).
- Stack: decided by the plan (C++17 core, LVGL 9.6, ESPHome, Vite + TypeScript, Python).
- Tracker: none named. Provisional: GitHub issues on this repo; decisions in
  `docs/decisions.md` (D-014). The template's "external tracker" lines were rewritten to say so.
- Visibility: **public**. Hence: no lineage block, no private names, a public/private rule in §4,
  and no self-hosted runners.
- Licence: **Apache-2.0**, chosen by the operator on 2026-11-20 (`LICENSE`).
- Merge strategy: squash (provisional; GitHub default for a single-commit-per-PR flow).
- Review bots: Codex (scarce), CodeRabbit (per-PR), Copilot, **all unconfirmed**: no PR had been
  reviewed when this was written. `docs/pr-review-machinery.md` §1 says so in place.

**Pruned**

- `competition-build`: no trust-boundary surface in this repo (no auth, no tenancy, no key
  custody). The editor's Home Assistant token never leaves the browser and is never stored; if
  that changes, revisit.
- `dependabot-sweep`: no Dependabot or Renovate configured yet. Revisit when one is.
- `quality-audit`, `quality-audit-refresh`: multi-million-token fan-outs; out of proportion for a
  young single-maintainer repo.
- `docs/rails-prometheus-metrics.md`: not a Rails service.
- Caveman tone module (`.cursor/`, `.windsurf/`, `.clinerules/`, `.opencode/`,
  `.github/copilot-instructions.md`): a session-level tone preference, not repo content.
- `mise.toml`: declined at init ("tool versions are pinned where they are consumed (CI,
  `schema/requirements.txt`, `cmake/HearthmapDoctest.cmake`) rather than through a version
  manager file"). **Reversed 2026-11-22** on the owner's word, after the editor refused to
  install under the machine's default Node: the repo now carries a `mise.toml` for the local
  toolchain (§6). CI still pins its own tools, and library versions stay pinned where they are
  consumed.
- LICENSE: see above.

**Kept**

- `single-pr`, `batch-review` (the build plan ships as a multi-PR batch), `independent-commit-review`
  (required by both), `stack-integration-check` (required by `batch-review`), `park-context` /
  `resume-context` (a pair), `docs/pr-review-machinery.md` (read by the PR skills),
  `docs/repo-map.md` (this repo has real edges: Home Assistant's API, a browser holding a token,
  OTA firmware built from this code).

**Adapted**

- §4 written for this repo: the layering rule, the public/private rule, the schema contract,
  headers as the inter-builder interface, verify-don't-remember for ESPHome/Home Assistant.
- §8's discovered-work and compaction rules point at GitHub issues and `docs/decisions.md`
  instead of an external tracker.
- Skills' "which copy" paragraphs and the review doc's evidence notes were edited to remove
  names of private repositories; their bodies are otherwise the template's.
- `batch-review`'s Repo specifics filled for this repo.
- §6.1 seeded with the two environment traps met while building PR 1; §9 starts empty.
