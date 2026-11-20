# components/hearthmap/

Lands in **PR 7**. The ESPHome external component. A device YAML needs a few lines:

```yaml
hearthmap:
  config: hearthmap.json   # a schema-valid config, kept in a private repo
  lvgl_id: lvgl_main
```

Python (`__init__.py`): validates the config against `schema/hearthmap.schema.json`, generates the
static config tables core reads through a `ConfigSource`, and converts plant photos to RGB565 at
build time. C++ glue: subscribes to each configured entity through the Home Assistant native API
(no per-sensor YAML), fills `hearthmap::Readings`, passes ESPHome's clock in as `now`, and drives
`ui_lvgl`. Core and `ui_lvgl` reach the firmware build by `add_idf_component` pinned to a tag of
this repo, with a vendored-copy fallback if that cannot work on the build host.

The component's own docs are verified against the pinned ESPHome release at build time, not
written from memory (`AGENTS.md` §6.1).
