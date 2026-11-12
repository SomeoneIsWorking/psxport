"""Process environment for a product run: the player's shipping run or an agent's hidden-window run.

Launchers call :func:`player_environment` only at the final product exec; provisioning and build
subprocesses keep the caller's environment. Agent tools call :func:`agent_environment`.
"""

from __future__ import annotations

import os
import shutil
import sys
from collections.abc import Mapping
from pathlib import Path

AGENT_RUNTIME_KEYS = (
    "PSXPORT_NOAUDIO",
    "PSXPORT_NOPACE",
)

# The player's window is shown; every other run keeps the same window unmapped.
SHOW_WINDOW = "PSXPORT_SHOW_WINDOW"

# Memory-card path key (runtime/psx/platform/memcard.cpp).
CARD_ENV = "PSXPORT_CARD"

# RmlUI and font assets ship in the framework checkout.
FRAMEWORK_ROOT = Path(__file__).resolve().parents[2]


def player_environment(environment: Mapping[str, str], *, product: str) -> dict[str, str]:
    """The windowed, audible, paced shipping environment.

    ``product`` names the run-log directory. The product keeps the player's settings in its own
    user-data file (runtime/psx/config/config.cpp), so no PSXPORT_SETTINGS is set here.
    """
    result = dict(environment)
    for key in AGENT_RUNTIME_KEYS:
        result.pop(key, None)
    result[SHOW_WINDOW] = "1"
    result.setdefault("PSXPORT_ASSET_DIR", str(FRAMEWORK_ROOT))
    if "PSXPORT_LOG_FILE" not in result:
        result["PSXPORT_LOG_FILE"] = str(_prepared_player_log(product, result))
    return result


def _prepared_player_log(product: str, environment: Mapping[str, str]) -> Path:
    """The default log path with its directory made and the previous run cleared; failure is reported."""
    path = player_log_path(product, environment)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("")
    except OSError as error:
        print(f"[run] no run log at {path}: {error}", file=sys.stderr)
    return path


def player_log_path(product: str, environment: Mapping[str, str] | None = None) -> Path:
    """Where a player's run log for ``product`` belongs under the host's user-data conventions."""
    env = os.environ if environment is None else environment
    if sys.platform == "darwin":
        base = Path(env.get("HOME", "~")).expanduser() / "Library" / "Logs"
    elif sys.platform == "win32":
        base = Path(env.get("LOCALAPPDATA") or Path(env.get("USERPROFILE", "~")).expanduser()) / "Logs"
    else:
        base = Path(env.get("XDG_STATE_HOME") or (Path(env.get("HOME", "~")).expanduser() / ".local/state"))
    return base / "psxport" / product / "last-run.log"


def blank_card_environment(environment: Mapping[str, str], card: str | Path) -> dict[str, str]:
    """``environment`` pointing at a memory card that does not exist yet.

    The start state then depends only on the binary and the disc. A directory at ``card`` is refused.
    """
    path = Path(card)
    if path.is_dir():
        raise IsADirectoryError(f"card path {path} is a directory; name the card image file")
    path.unlink(missing_ok=True)
    result = dict(environment)
    result[CARD_ENV] = str(path)
    return result


def agent_environment(environment: Mapping[str, str],
                      settings: str | Path | None = None,
                      *,
                      card: str | Path | None = None) -> dict[str, str]:
    """The hidden-window, silent, unpaced automation environment.

    The run gets its own settings file under ``scratch/settings/`` of the working directory, seeded
    from ``settings`` (or PSXPORT_SETTINGS) when given, so a tool run never reads or writes the
    player's file and never rewrites a tracked seed. ``card`` starts the run on a blank memory card.
    """
    result = dict(environment)
    result.update(session_display(result))
    result[SHOW_WINDOW] = "0"
    for key in AGENT_RUNTIME_KEYS:
        result[key] = "1"
    result.setdefault("PSXPORT_ASSET_DIR", str(FRAMEWORK_ROOT))
    result["PSXPORT_SETTINGS"] = str(tool_settings(settings or result.get("PSXPORT_SETTINGS")))
    if card is not None:
        result = blank_card_environment(result, card)
    return result


class NoSessionDisplay(RuntimeError):
    """The hidden window has no user session to open on."""


def session_display(environment: Mapping[str, str]) -> dict[str, str]:
    """The user's display for the unmapped window, found in XDG_RUNTIME_DIR when the caller has none.

    Background agent shells inherit no WAYLAND_DISPLAY; the window still belongs on the user's session.
    """
    if environment.get("WAYLAND_DISPLAY") or environment.get("DISPLAY"):
        return {}
    runtime = environment.get("XDG_RUNTIME_DIR", "")
    sockets = sorted(p.name for p in Path(runtime).glob("wayland-*") if p.is_socket()) if runtime else []
    if len(sockets) != 1:
        raise NoSessionDisplay(
            f"no single Wayland socket in XDG_RUNTIME_DIR={runtime or '(unset)'} (found {sockets}); "
            "set WAYLAND_DISPLAY to the user's session")
    return {"WAYLAND_DISPLAY": sockets[0]}


def tool_settings(seed: str | Path | None) -> Path:
    """This tool's settings file under scratch/settings/: a copy of ``seed``, else empty (defaults).

    The name carries the tool's PID, so concurrent tools in one checkout never share a file.
    """
    directory = Path.cwd() / "scratch" / "settings"
    directory.mkdir(parents=True, exist_ok=True)
    if not seed:
        path = directory / f"{os.getpid()}-defaults.ini"
        path.write_text("")
        return path
    source = Path(seed).resolve()
    if not source.is_file():
        raise FileNotFoundError(f"settings file {source} does not exist")
    path = directory / f"{os.getpid()}-{source.name}"
    if path != source:
        shutil.copyfile(source, path)
    return path
