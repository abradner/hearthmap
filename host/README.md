# host/

Lands in **PR 5**. The host renderer: the same `ui_lvgl` code on LVGL's headless display driver.

```sh
hearthmap-render --config examples/flat/config.json \
  --state examples/flat/readings/thirsty-many.json --out frame.png [--layer soil] [--overlay plant_basil]
```

`now` comes from the readings fixture's `now` field unless `--time` overrides it. Also home to the
host's `ConfigSource` (JSON parsing lives here, never in core) and the golden-image tests CI runs
over every fixture. An optional SDL window may follow.

Depends on `core`, `ui_lvgl`, LVGL, a JSON parser and a PNG writer (chosen and pinned in PR 5).
