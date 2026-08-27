"""Authoritative process-environment policy for players and agent runs.

Game launchers call :func:`player_environment` only at the final product exec
boundary. Provisioning and build subprocesses keep the caller's environment.
Agent tools call :func:`agent_environment` explicitly when they want an
offscreen, silent, unpaced run.
"""

from __future__ import annotations

from collections.abc import Mapping

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


def agent_environment(environment: Mapping[str, str]) -> dict[str, str]:
    """Return the explicit headless, silent, unpaced automation environment."""
    result = dict(environment)
    result.pop("PSXPORT_VK_WINDOW", None)
    for key in _LEGACY_HEADLESS_KEYS:
        result.pop(key, None)
    for key in AGENT_RUNTIME_KEYS:
        result[key] = "1"
    return result
