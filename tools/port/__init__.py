"""Tooling every title runs; the release platforms come from the shared target-platform checkout."""

import sys
from pathlib import Path

_FRAMEWORK = Path(__file__).resolve().parents[2]
_TARGET_PLATFORM = _FRAMEWORK / "vendor" / "target-platform"
if not (_TARGET_PLATFORM / "target_platform").is_dir():
    # A checkout pulled from before the submodule existed.
    import psxport_fetch

    psxport_fetch.init_submodules(str(_FRAMEWORK))
sys.path.insert(0, str(_TARGET_PLATFORM))
