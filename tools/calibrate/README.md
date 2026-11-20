# tools/calibrate/

Lands in **PR 8**. Reads Home Assistant history for each soil sensor in a config, proposes `dry`
and `wet` from robust percentiles and watering-event jumps, and writes the ranges back into the
config for review in the editor. Raw `0` readings are treated as real (a bone-dry pot), never
as missing data.

The Home Assistant URL and token are supplied at run time (environment or a secrets manager
reference) and are never written to a file, a log line, or the config.
