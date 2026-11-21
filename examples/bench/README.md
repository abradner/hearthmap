# examples/bench/

A benchmark firmware for the Guition JC8012P4A1 (ESP32-P4 rev v1.3 at 360 MHz, 32 MB hex PSRAM
at 200 MHz, 800 x 1280 MIPI-DSI panel shown landscape through LVGL's 90-degree software
rotation). It answers one question with measurements: what can the P4's hardware (the PPA, the
2D-DMA, the AXI GDMA, the PIE SIMD extension) do for this UI, and what does it cost to use?
The decisions taken from it are D-057 to D-060 in `docs/decisions.md`.

It is not part of the hearthmap component and needs no network, no Home Assistant and no
secrets. It runs one full pass 8 s after boot, then again 30 s after each pass, and prints one
INFO line per measurement on UART0, tagged `bench`. While it runs, the screen shows a test
scene and a status line; the frame-buffer tests scribble on it for a few seconds.

## Build

From the repository root, with the pinned image, one compile at a time:

```sh
docker run --rm -v "$PWD":/config ghcr.io/esphome/esphome:2026.11.0 compile examples/bench/esphome/bench.yaml
# LVGL's PPA draw unit forced on under ESP-IDF 5.5.6 (experiment, D-060):
docker run --rm -v "$PWD":/config ghcr.io/esphome/esphome:2026.11.0 \
  -s lvgl_ppa_unit true -s device_name hm-bench-ppa compile examples/bench/esphome/bench.yaml
# ESP-IDF 6.0.1 through ESPHome's framework version option:
docker run --rm -v "$PWD":/config ghcr.io/esphome/esphome:2026.11.0 \
  -s idf_version 6.0.1 -s device_name hm-bench-idf6 compile examples/bench/esphome/bench.yaml
```

The repository root is mounted (not this directory) because the bench compiles the shipped
`ui_lvgl` Blitter and the component's `PpaBlitter` into itself (`components/hm_bench/__init__.py`).
Flash `firmware.factory.bin` and capture UART0 at 115200 from boot to `bench run 0 done`.

## Line formats

- `bench <op> size=WxH src=<psram|iram> dst=<psram|iram> : X Mpx/s, call overhead Y us, n=N, Z us/call[, check ok|check FAIL k/66 samples]`.
  Call overhead is the time per call of the same operation at 2 x 2 pixels (32 x 2 for the GDMA,
  which needs whole cache lines). `check` compares 66 sampled pixels with a C reference after
  one run; "skip" means the buffers did not fit in internal RAM.
- `bench blitter_check <op> ...`: the PPA Blitter against `SoftwareBlitter` on clipped cases.
- `bench cache_trap ...`: the scale-rotate cache trap (below), raw and through `PpaBlitter`.
- `bench blit_<sw|ppa>_<op> ...`: the Blitter implementations into a 1280 x 800 surface.
- `bench dsi_draw_pixels`, `ppa_srm_*_to_fb`, `cpu_rows_to_fb`: putting pixels in the DSI frame buffer.
- `bench scene <case> buf=<placement> : T ms/refresh (render R ms, flush F ms in N calls) ...`:
  LVGL repaints of a test scene, five each, with the draw buffer moved at run time.
- `bench tile ...`: one small opaque tile through an LVGL refresh against the direct present path.

Small sizes in PSRAM are cache-resident after the first iteration (the CPU numbers at 64 and 128 px
are cache numbers, which is also how a repeatedly drawn small sprite behaves); DMA engines read
memory whatever the cache holds, and the drivers' cache write-back is inside their numbers.

## Results (2026-11-21, four firmwares, no crash or reboot)

Megapixels per second, destination pixels. PS = PSRAM, IN = internal RAM, source and destination
in the same place unless stated. Plain C rows are the portable baseline (they are what an
ESP32-S3 port starts from).

| Operation | Call overhead (us) | 64x64 PS | 64x64 IN | 128x128 PS | 128x128 IN | 320x240 PS | 320x240 IN | 920x736 PS | 1280x800 PS |
|---|---|---|---|---|---|---|---|---|---|
| libc memcpy | 0.99 | 275 | 275 | 285 | 285 | 21 | 189 | 22 | 22 |
| C word copy (8x unrolled) | 0.98 | 246 | 246 | 254 | 254 | 21 | 169 | 22 | 22 |
| PIE 128-bit copy | 1.01 | 742 | 737 | 820 | 820 | 21 | 362 | 21 | 22 |
| AXI GDMA async memcpy | 94.76 | 24 | 31 | 39 | 59 | 47 | 79 | 49 | 49 |
| PPA SRM 1:1 | 75.12 | 17 | 30 | 21 | 53 | 22 | 66 | 22 | 22 |
| PPA SRM rotate 90 | 75.10 | 17 | 30 | 20 | 53 | 22 | 66 | 22 | 22 |
| C rotate 90 | 1.03 | 49 | 49 | 50 | 50 | 16 | 48 | 15 | 12 |
| PPA SRM scale 2x (interpolates: not pixel doubling) | 75.11 | 28 | 30 | 51 | 60 | 55 | 81 | 63 | 60 |
| C pixel doubling | 1.07 | 105 | 105 | 117 | 117 | 27 | 108 | 27 | 27 |
| PPA fill | 46.60 | 55 | 59 | 112 | 117 | 159 | 161 | 177 | 178 |
| C constant-alpha blend 565 | 1.30 | 10 | 10 | 10 | 10 | 6.2 | skip | 6.3 | 6.3 |
| PIE constant-alpha blend 565 | 1.39 | 80 | 80 | 69 | 69 | 14 | skip | 14 | 14 |
| PPA blend 565 at constant alpha, in place | 79.69 | 21 | 28 | 34 | 46 | 42 | skip | 44 | 45 |
| PPA blend ARGB8888 over 565, in place | 79.47 | 18 | 28 | 26 | 46 | 31 | n/a | 33 | 33 |
| PPA blend A8 mask in one colour, in place | 79.67 | 22 | 28 | 36 | 46 | 44 | skip | 48 | 48 |
| C colour-table fill (u8 -> 565) | 0.96 | 51 | 51 | 51 | 51 | 19 | 48 | 18 | 18 |

| LVGL draw call into an RGB565 canvas | Firmware | Call overhead (us) | 64x64 PS | 128x128 PS | 320x240 PS | 320x240 IN | 920x736 PS | 1280x800 PS |
|---|---|---|---|---|---|---|---|---|
| opaque RGB565 image (PSRAM source) | 1 SW | 28.79 | 81 | 162 | 16 | 160 | 21 | 21 |
| opaque RGB565 image (PSRAM source) | 2 PPA unit, 5.5.6 | fails at 2x2 (a) | 18 | 33 | 44 | 55 | 49 | 49 |
| opaque RGB565 image (PSRAM source) | 3 IDF 6.0.1 | 28.80 | 81 | 161 | 16 | 160 | 21 | 21 |
| opaque RGB565 image (flash source) | 1 SW | 28.81 | 80 | 162 | n/a | n/a | n/a | n/a |
| opaque RGB565 image (flash source) | 2 PPA unit, 5.5.6 | fails at 2x2 (a) | 3.9 | 4.4 | n/a | n/a | n/a | n/a |
| opaque RGB565 image (flash source) | 3 IDF 6.0.1 | 28.78 | 80 | 161 | n/a | n/a | n/a | n/a |
| RGB565 image at 50 % | 1 SW | 29.10 | 9.9 | 10 | 6.3 | skip | 6.9 | 6.9 |
| RGB565 image at 50 % | 2 PPA unit, 5.5.6 | 30.54 | 9.9 | 10 | 6.3 | skip | 6.9 | 6.9 |
| RGB565 image at 50 % | 3 IDF 6.0.1 | 29.06 | 9.8 | 10 | 6.2 | skip | 6.8 | 6.8 |
| ARGB8888 image | 1 SW | 29.51 | 7.2 | 6.7 | 4.8 | n/a | 5.2 | 5.2 |
| ARGB8888 image | 2 PPA unit, 5.5.6 | 31.12 | 7.2 | 6.9 | 4.8 | n/a | 5.2 | 5.2 |
| ARGB8888 image | 3 IDF 6.0.1 | 29.38 | 7.2 | 7.0 | 4.7 | n/a | 5.1 | 5.1 |
| opaque fill | 1 SW | 14.48 | 178 | 335 | 33 | 298 | 31 | 31 |
| opaque fill | 2 PPA unit, 5.5.6 | fails at 2x2 (a) | 38 | 90 | 148 | 152 | 176 | 177 |
| opaque fill | 3 IDF 6.0.1 | fails at 2x2 (a) | 38 | 91 | 149 | 152 | 176 | 177 |
| fill at 50 % | 1 SW | 14.83 | 36 | 41 | 21 | 40 | 18 | 18 |
| fill at 50 % | 2 PPA unit, 5.5.6 | 15.91 | 36 | 41 | 20 | 40 | 18 | 18 |
| fill at 50 % | 3 IDF 6.0.1 | 15.50 | 35 | 39 | 20 | 38 | 17 | 18 |
| anti-aliased ring, width 6 | 1 SW | 25.30 | 12 | 18 | n/a | n/a | n/a | n/a |
| anti-aliased ring, width 6 | 2 PPA unit, 5.5.6 | 26.26 | 12 | 18 | n/a | n/a | n/a | n/a |
| anti-aliased ring, width 6 | 3 IDF 6.0.1 | 26.84 | 12 | 19 | n/a | n/a | n/a | n/a |

| Through the Blitter, into a 1280x800 PSRAM surface | 8x8 | 32x32 | 64x64 | 128x128 | 320x240 | 920x736 | 1280x800 |
|---|---|---|---|---|---|---|---|
| SoftwareBlitter copy | 24 | 96 | 152 | 154 | 20 | 20 | 22 |
| PpaBlitter copy (blend engine) | 0.6 | 8.2 | 22 | 37 | 45 | 48 | 49 |
| SoftwareBlitter fill | 26 | 46 | 49 | 50 | 31 | 25 | 25 |
| PpaBlitter fill | 1.2 | 15 | 48 | 107 | 158 | 176 | 178 |
| SoftwareBlitter blend (exact /255) | 6.7 | 7.7 | 7.8 | 7.8 | 5.9 | 5.9 | 5.9 |
| PpaBlitter blend | 0.7 | 7.8 | 20 | 33 | 41 | 43 | 45 |
| SoftwareBlitter scale2x | 22 | 49 | 56 | 60 | 24 | 22 | 23 |


(a) LVGL's PPA unit refuses a draw buffer whose start or size is not whole 64-byte cache lines
(`ppa_core: out.buffer addr or out.buffer_size not aligned`) and then draws **nothing**: no
software fallback. The 2 x 2 canvas here is such a buffer. The time shown is the warning it logs.

Flash sources through the PPA run at 4 Mpx/s, 40 times slower than the CPU (the generated plant
photos are flash data).

### Frame buffer (firmware 1; DSI frame buffer in PSRAM, scanned continuously)

| Path | Call overhead (us) | 64x64 | 128x128 | 920x736 class | full screen |
|---|---|---|---|---|---|
| `Display::draw_pixels_at` (DMA2D copy, the lvgl component's own flush path), PSRAM source | 52 | 38 | 57 | 69 | 69 |
| same, internal-RAM source | 44 | 55 | 125 | - | - |
| PPA rotate straight into the frame buffer, PSRAM source | 76 | 16 | 21 | 22 | 22 |
| CPU rows into the frame buffer + cache write-back | 5 | 65 | 73 | 21 | 22 |

The lvgl component's flush is PPA rotate into a PSRAM buffer (22 Mpx/s) then the DMA2D copy
(69): about 16-17 Mpx/s, which matches the scene flush below (1024 kpx in 63 ms).

### Scenes (ms per refresh: total, render / flush)

| Case | default buffer (2 MB PSRAM) fw 1 | PPA unit fw 2 | IDF 6 fw 3 | 256 kB internal fw 1 | 256 kB internal fw 2 | 128 kB internal fw 1 | 64 kB internal fw 1 | 256 kB PSRAM fw 1 |
|---|---|---|---|---|---|---|---|---|
| full screen | 152 (89/63) | 102 (40/62) | 125 (61/64) | 126 (80/46) | 72 (36/36) | 152 (92/60) | 146 (79/66) | 223 (138/85) |
| map 920x736 | 113 (70/43) | 74 (32/42) | 95 (52/43) | 85 (53/32) | 53 (28/24) | 118 (74/44) | 102 (57/45) | 152 (97/56) |
| plain image 400x300 | 24.3 (15.1/9.2) | 11.7 (4.2/7.5) | 20.2 (10.9/9.3) | 12.4 (7.4/5.0) | 7.1 (3.4/3.7) | 17.2 (10.5/6.7) | 14.3 (7.6/6.7) | 24.1 (15.0/9.2) |
| overlay card 600x400 at 60 % | 64.5 (44.7/19.8) | 48.8 (29.1/19.7) | 57.9 (37.6/20.2) | 42.6 (30.1/12.5) | 27.6 (18.2/9.4) | 54.0 (37.4/16.6) | 51.8 (32.9/18.9) | 76.8 (53.6/23.2) |
| ring 100x100 | 3.3 (2.1/1.2) | 3.3 (2.3/1.0) | 3.7 (2.5/1.2) | 2.3 (1.7/0.6) | 2.0 (1.5/0.5) | 2.3 (1.7/0.6) | 2.2 (1.6/0.6) | 3.2 (2.0/1.2) |

### Tiles (firmware 4): one small opaque image tile

An LVGL refresh with nothing invalid costs 14 us; with one 2 x 2 area, 660 us (flush 170 us).
So about 0.49 ms of every refreshed area is LVGL's own per-area work and 0.17 ms the flush
set-up (PPA rotate call, DMA2D, cache write-back, the semaphore wait), before any pixel cost.

| Tile | LVGL invalidate, 2 MB PSRAM buffer, 1 / 9 tiles (us per tile) | same, 256 kB internal buffer | Present: CPU rotate into internal RAM + `draw_pixels_at` | Present from PSRAM | CPU rotate only | Rotate straight into frame buffer |
|---|---|---|---|---|---|---|
| 32x32 | 707 / 739 | 678 / 718 | 83 | 103 | 25 | 348 |
| 64x64 | 992 / 1234 | 843 / 1062 | 169 | 232 | 94 | 827 |
| 96x96 | 1921 / 2370 | 1329 / 1745 | 306 | 473 | 209 | 1436 |
| 128x128 | 4747 / 4961 | 2664 / 3082 | 497 | 774 | 368 | 2225 |

### Checks

- `blitter_check`: copy and fill through the PPA are identical to `SoftwareBlitter`; blend differs
  by at most one step per channel (the Blitter contract allows one); the PPA's 2x scale differs by
  up to 21 steps (it interpolates), so `PpaBlitter` never scales.
- **Cache trap**: the PPA driver writes its inputs back from the cache but only *invalidates* an
  output it does not also read. CPU writes still in the cache, in the output rows but outside the
  block, are discarded: 15,840 to 16,228 of 16,384 such pixels lost in every firmware. Through
  `PpaBlitter`, which writes those rows back first, none.
- Every other `check` passed, including the PIE kernels against their C references.

## Reproducing

Firmwares 1 to 3 were built from commit b4ca74e (its first version of this bench, whose
`PpaBlitter` sent every call to the PPA and scaled with the PPA's scaler); firmware 4 from 25d95b8
(tiles added; `PpaBlitter` with today's thresholds, copies through the blend engine, no PPA
scaling). The `blit_ppa_*` and `blitter_check` rows above are firmware 4's. Commits are this
branch's; flashed one after the other on one panel. The raw UART logs are not kept here.
