#!/usr/bin/env python3
"""Entry point: python3 tools/calibrate/calibrate.py {fetch,propose,merge} ... (Python 3.11+)."""

import sys

if sys.version_info < (3, 11):
    sys.exit("hearthmap-calibrate needs Python 3.11 or newer")

from hearthmap_calibrate.cli import main  # noqa: E402

sys.exit(main())
