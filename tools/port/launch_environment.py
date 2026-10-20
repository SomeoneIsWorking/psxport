"""Process environment for a product run: the player's shipping run or an agent's hidden-window run.

Launchers call :func:`player_environment` only at the final product exec; provisioning and build
subprocesses keep the caller's environment. Agent tools call :func:`agent_environment`.
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

# Memory-card path key (runtime/psx/platform/memcard.cpp).
CARD_ENV = "PSXPORT_CARD"

# RmlUI and font assets ship in the framework checkout.
FRAMEWORK_ROOT = Path(__file__).resolve().parents[2]


def player_environment(environment: Mapping[str, str], *,
                       product: str,
                       settings: str | Path | None = None) -> dict[str, str]:
    """The windowed, audible, paced shipping environment.

    ``product`` names the run-log directory. ``settings`` names the tracked shipping .ini, so the
    picture never depends on an untracked file beside the binary; an explicit PSXPORT_SETTINGS wins.
    """
    result = dict(environment)
    for key in AGENT_RUNTIME_KEYS:
        result.pop(key, None)
    result["PSXPORT_VK_WINDOW"] = "1"
    result.setdefault("PSXPORT_ASSET_DIR", str(FRAMEWORK_ROOT))
    if settings is not None and "PSXPORT_SETTINGS" not in result:
        chosen = Path(settings)
        if chosen.is_file():
            result["PSXPORT_SETTINGS"] = str(chosen.resolve())
        else:
            print(f"[run] no tracked shipping settings at {chosen}; the product will use its own "
                  f"working-directory discovery", file=sys.stderr)
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

    ``settings`` (or PSXPORT_SETTINGS) must name an existing tracked .ini: unset would hand the
    product its working-directory discovery. ``card`` starts the run on a blank memory card.
    """
    result = dict(environment)
    result.pop("PSXPORT_VK_WINDOW", None)
    for key in AGENT_RUNTIME_KEYS:
        result[key] = "1"
    result.setdefault("PSXPORT_ASSET_DIR", str(FRAMEWORK_ROOT))
    result["PSXPORT_SETTINGS"] = str(_resolved_settings(settings, result))
    if card is not None:
        result = blank_card_environment(result, card)
    return result


def _resolved_settings(settings: str | Path | None, environment: Mapping[str, str]) -> Path:
    chosen = settings if settings is not None else environment.get("PSXPORT_SETTINGS")
    if not chosen:
        raise ValueError(
            "agent_environment needs a settings file: pass settings=<tracked .ini> or set PSXPORT_SETTINGS"
        )
    path = Path(chosen).resolve()
    if not path.is_file():
        raise FileNotFoundError(f"settings file {path} does not exist")
    return path
