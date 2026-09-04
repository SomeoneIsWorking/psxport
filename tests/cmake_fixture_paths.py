"""Explicit dependency inputs shared by nested CMake fixture tests."""

from __future__ import annotations

import os
from pathlib import Path


def configured_lightrec() -> Path:
    raw = os.environ.get("PSXPORT_LIGHTREC_DIR")
    if not raw:
        raise RuntimeError(
            "PSXPORT_LIGHTREC_DIR is missing; CTest must pass the exact Lightrec path "
            "resolved by the parent CMake configure"
        )
    checkout = Path(raw).resolve()
    if not (checkout / "lightrec.h").is_file():
        raise RuntimeError(f"configured Lightrec checkout is missing lightrec.h: {checkout}")
    return checkout
