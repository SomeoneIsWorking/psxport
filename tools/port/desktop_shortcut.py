"""Install this title's desktop shortcut: the menu entry, its icon and the Desktop copy.

    uv run --frozen python external/psxport/tools/port/desktop_shortcut.py

Run from the title root. The title owns ``packaging/linux/<app-id>.desktop`` and the SVG its
``Icon=`` names; this engine rewrites ``Exec`` / ``TryExec`` / ``Path`` to the launcher in this
checkout and installs both files into the user's data home, so the shortcut is per machine and never
committed. The shortcut launches the newest build already made and builds only when there is none. Re-run it when the launcher, the product name or the icon changes.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from automation.process import ToolError
from port import launcher as launcher_module

PACKAGING = Path("packaging") / "linux"
LAUNCHER = Path("run.sh")
LAUNCH_PATH = "/usr/local/bin:/usr/bin:/bin"
SAFE_EXEC = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-")


@dataclass(frozen=True)
class Shortcut:
    """One title's entry: the committed source, the icon it names and where the user keeps them."""

    root: Path
    source: Path
    icon: Path

    @property
    def app_id(self) -> str:
        return self.source.stem


def field(text: str, key: str) -> str:
    """A desktop entry's ``key=value`` value, or empty when the entry does not carry it."""
    prefix = f"{key}="
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return ""


def quoted_exec(value: str) -> str:
    """Quote one Exec argument the way the desktop entry spec reads it."""
    if value and all(character in SAFE_EXEC for character in value):
        return value
    return '"' + "".join(f"\\{character}" if character in '"`$\\' else character
                         for character in value) + '"'


def with_field(text: str, key: str, value: str) -> str:
    """The entry with ``key=value`` set in its group, appended when the entry omits it."""
    lines = text.splitlines()
    prefix = f"{key}="
    for index, line in enumerate(lines):
        if line.startswith(prefix):
            lines[index] = prefix + value
            break
    else:
        lines.append(prefix + value)
    return "\n".join(lines).rstrip() + "\n"


def resolve(root: Path) -> Shortcut:
    """The title's one committed entry and the icon that entry names."""
    entries = sorted(PACKAGING.glob("*.desktop"))
    if not entries:
        raise ToolError(f"no desktop entry in {root / PACKAGING}; the title owns its product identity")
    if len(entries) > 1:
        raise ToolError(f"{root / PACKAGING} names several desktop entries: "
                        + ", ".join(entry.name for entry in entries))
    source = entries[0]
    text = source.read_text(encoding="utf-8")
    icon_name = field(text, "Icon")
    if not icon_name:
        raise ToolError(f"{source} names no Icon=, so the shortcut would have no icon")
    icon = source.parent / f"{icon_name}.svg"
    if not icon.is_file():
        raise ToolError(f"{source} names Icon={icon_name} but {icon} does not exist")
    return Shortcut(root=root, source=source, icon=icon)


def data_home() -> Path:
    configured = os.environ.get("XDG_DATA_HOME", "")
    return Path(configured) if configured else Path.home() / ".local" / "share"


def desktop_home() -> Path | None:
    """The user's Desktop directory; XDG's setting wins, and an absent one gets no copy."""
    configured = os.environ.get("XDG_DESKTOP_DIR", "")
    if not configured:
        directories = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "user-dirs.dirs"
        try:
            for line in directories.read_text(encoding="utf-8", errors="replace").splitlines():
                key, separator, value = line.partition("=")
                if separator and key.strip() == "XDG_DESKTOP_DIR":
                    configured = value.strip().strip('"').replace("$HOME", str(Path.home()))
        except OSError:
            configured = ""
    candidate = Path(configured) if configured else Path.home() / "Desktop"
    return candidate if candidate.is_dir() else None


def launcher_exec(root: Path) -> str:
    """The command the shortcut runs: the title launcher on the newest build already made, with uv reachable."""
    launcher = root / LAUNCHER
    if not launcher.is_file():
        raise ToolError(f"no title launcher at {launcher}")
    if not os.access(launcher, os.X_OK):
        raise ToolError(f"{launcher} is not executable")
    uv = shutil.which("uv")
    if uv is None:
        raise ToolError("uv is not on PATH, so the shortcut could not run the launcher")
    search = os.pathsep.join((str(Path(uv).resolve().parent), LAUNCH_PATH))
    return " ".join(quoted_exec(part) for part in
                    ("env", f"PATH={search}", f"{launcher_module.RUN_BUILT}=1", str(launcher)))


def entry_text(shortcut: Shortcut, exec_line: str, try_exec: str, path: str) -> str:
    """The committed entry with its launch fields pointed at one install."""
    text = shortcut.source.read_text(encoding="utf-8")
    text = with_field(text, "Exec", exec_line)
    text = with_field(text, "TryExec", try_exec)
    return with_field(text, "Path", path)


def install_entry(shortcut: Shortcut) -> Path:
    """Write the icon and the menu entry; returns the installed entry's path."""
    text = entry_text(shortcut, launcher_exec(shortcut.root), str(shortcut.root / LAUNCHER), str(shortcut.root))
    data = data_home()
    icons = data / "icons" / "hicolor" / "scalable" / "apps"
    icons.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(shortcut.icon, icons / shortcut.icon.name)
    entry = data / "applications" / f"{shortcut.app_id}.desktop"
    entry.parent.mkdir(parents=True, exist_ok=True)
    entry.write_text(text, encoding="utf-8")
    entry.chmod(0o755)
    desktop = desktop_home()
    if desktop is not None:
        copy = desktop / entry.name
        copy.write_text(text, encoding="utf-8")
        copy.chmod(0o755)
    refresh_caches(data)
    return entry


def cache_commands(data: str) -> list[list[str]]:
    """The menu and icon-theme refreshes for the data home ``data``; either tool may be absent."""
    return [["update-desktop-database", f"{data}/applications"],
            ["gtk-update-icon-cache", "-q", "-t", "-f", f"{data}/icons/hicolor"]]


def refresh_caches(data: Path) -> None:
    """Refresh the menus and the icon theme when their tools are installed; neither is required."""
    for command in cache_commands(str(data)):
        if shutil.which(command[0]):
            subprocess.run(command, check=False, capture_output=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.parse_args(argv)
    try:
        shortcut = resolve(Path.cwd().resolve())
        entry = install_entry(shortcut)
    except ToolError as error:
        print(f"[shortcut] FAILED: {error}", file=sys.stderr)
        return 1
    print(f"[shortcut] {field(shortcut.source.read_text(encoding='utf-8'), 'Name')}: {entry}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
