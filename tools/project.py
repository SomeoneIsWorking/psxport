#!/usr/bin/env python3
"""psxport build policy: the checkout root and the default build tree."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build" / "ci"
