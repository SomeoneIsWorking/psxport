"""Authoritative process-environment policy for players and agent runs.

Game launchers call :func:`player_environment` only at the final product exec
boundary. Provisioning and build subprocesses keep the caller's environment.
Agent tools call :func:`agent_environment` explicitly when they want an
offscreen, silent, unpaced run.
"""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path

AGENT_RUNTIME_KEYS = (
    "PSXPORT_VK_HEADLESS",
    "PSXPORT_NOAUDIO",
    "PSXPORT_NOPACE",
)

_LEGACY_HEADLESS_KEYS = (
    "PSXPORT_NOWINDOW",
    "PSXPORT_HEADLESS",
)


def player_environment(environment: Mapping[str, str]) -> dict[str, str]:
    """Return the windowed, audible, real-time-paced shipping environment."""
    result = dict(environment)
    for key in (*AGENT_RUNTIME_KEYS, *_LEGACY_HEADLESS_KEYS):
        result.pop(key, None)
    result["PSXPORT_VK_WINDOW"] = "1"
    return result


def agent_environment(environment: Mapping[str, str],
                      settings: str | Path | None = None) -> dict[str, str]:
    """Return the explicit headless, silent, unpaced automation environment.

    An agent run must also declare the presentation configuration it is gating, which is why
    ``settings`` names a tracked file and this refuses without one. Leaving ``PSXPORT_SETTINGS``
    unset does not mean "product defaults": it hands the product back its own working-directory
    discovery, so the run is configured by whichever untracked settings file happens to sit beside
    it. Measured 2026-09-19 — Spyro oracle comparisons launched from the repository root had always
    run with widescreen and 60fps on, from the operator's personal file, while recording nothing
    about it; the same command on a fresh clone or in CI would have compared an unenhanced product
    and looked identical.

    A configuration with the enhancements OFF is a tracked file saying so, not an absent one. A path
    that does not resolve is refused for the same reason: the product would fall back to built-in
    defaults and the run would look like one that had honoured the file.
    """
    result = dict(environment)
    result.pop("PSXPORT_VK_WINDOW", None)
    for key in _LEGACY_HEADLESS_KEYS:
        result.pop(key, None)
    for key in AGENT_RUNTIME_KEYS:
        result[key] = "1"
    result["PSXPORT_SETTINGS"] = str(_resolved_settings(settings, result))
    return result


def _resolved_settings(settings: str | Path | None, environment: Mapping[str, str]) -> Path:
    chosen = settings if settings is not None else environment.get("PSXPORT_SETTINGS")
    if not chosen:
        raise ValueError(
            "agent_environment needs a settings file: pass settings=<tracked .ini> or set "
            "PSXPORT_SETTINGS. Unset would hand the product its working-directory discovery, so "
            "the run's configuration would be whatever untracked file sits beside it"
        )
    path = Path(chosen).resolve()
    if not path.is_file():
        raise FileNotFoundError(
            f"settings file {path} does not exist; the product would run on built-in defaults and "
            "the run would look like one that had honoured the file"
        )
    return path
