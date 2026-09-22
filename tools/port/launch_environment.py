"""Authoritative process-environment policy for players and agent runs.

Game launchers call :func:`player_environment` only at the final product exec
boundary. Provisioning and build subprocesses keep the caller's environment.
Agent tools call :func:`agent_environment` explicitly when they want an
offscreen, silent, unpaced run.
"""

from __future__ import annotations

import os
import sys
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


def player_environment(environment: Mapping[str, str], *, product: str) -> dict[str, str]:
    """Return the windowed, audible, real-time-paced shipping environment.

    ``product`` names the port, and is REQUIRED because it is what keeps one title's run log out
    of another's. It is not decorative: the log below is the only copy of what the product said.

    WHY THE LOG. A player's run wrote its diagnostics to the terminal and nowhere else, so a crash
    the operator saw was a crash nobody could read. Spyro's issue 0128 is a user-visible abort
    before gameplay that eight agent runs across build, window, pacing, settings, audio and a
    900-frame overrun have all failed to reproduce; the abort prints the stage and the refusing
    producer, which is the entire diagnosis, and it has never been captured because it scrolled
    past in a terminal that was then closed. The next occurrence leaves the file behind without the
    player being told to run anything special.

    It is a DEFAULT, not a policy: a caller that already set ``PSXPORT_LOG_FILE`` keeps its value,
    and only the path invented here is prepared on disk.
    """
    result = dict(environment)
    for key in (*AGENT_RUNTIME_KEYS, *_LEGACY_HEADLESS_KEYS):
        result.pop(key, None)
    result["PSXPORT_VK_WINDOW"] = "1"
    if "PSXPORT_LOG_FILE" not in result:
        result["PSXPORT_LOG_FILE"] = str(_prepared_player_log(product, result))
    return result


def _prepared_player_log(product: str, environment: Mapping[str, str]) -> Path:
    """The default log path, with its directory made and the previous run's file cleared.

    Both halves matter and neither is incidental. The logger opens the path with ``fopen(.., "a")``
    and falls back to stderr when that fails, so a missing directory would turn this into exactly
    the silent no-op it exists to replace; and appending would make "last-run" a growing pile of
    runs with no boundary between them, which is worse to read than the terminal was.

    A failure here is not fatal: the product still runs and still says everything on stderr. It is
    reported so a run that has no log file says so, instead of leaving one to be looked for later.
    """
    path = player_log_path(product, environment)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("")
    except OSError as error:
        print(f"[run] no run log at {path}: {error}", file=sys.stderr)
    return path


def player_log_path(product: str, environment: Mapping[str, str] | None = None) -> Path:
    """Where a player's run log for ``product`` belongs, per the host's user-data conventions.

    Never the checkout, an AppImage mount or the working directory: a packaged player may have no
    write access to any of them, and a log that silently failed to open would be worse than none.
    """
    env = os.environ if environment is None else environment
    if sys.platform == "darwin":
        base = Path(env.get("HOME", "~")).expanduser() / "Library" / "Logs"
    elif sys.platform == "win32":
        base = Path(env.get("LOCALAPPDATA") or Path(env.get("USERPROFILE", "~")).expanduser()) / "Logs"
    else:
        base = Path(env.get("XDG_STATE_HOME") or (Path(env.get("HOME", "~")).expanduser() / ".local/state"))
    return base / "psxport" / product / "last-run.log"


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
