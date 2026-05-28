#!/usr/bin/env python3
"""Module entrypoint for the MacRunner configurator."""

from __future__ import annotations

import sys

from app.configurator.compatibility import main


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
