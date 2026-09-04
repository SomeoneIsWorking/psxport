#!/usr/bin/env python3
"""Syntax-check selected translation units with their recorded compile commands."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path


def command_arguments(entry: dict[str, object]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(value, str) for value in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    raise ValueError("compile command has neither a string command nor a string arguments list")


def syntax_command(entry: dict[str, object]) -> list[str]:
    original = command_arguments(entry)
    if not original:
        raise ValueError("compile command is empty")
    result = [original[0], "-fsyntax-only", "-Wformat"]
    index = 1
    while index < len(original):
        argument = original[index]
        if argument == "-o":
            index += 2
            continue
        if argument == "-c" or argument == "-w" or argument.startswith("-o"):
            index += 1
            continue
        result.append(argument)
        index += 1
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="+")
    parser.add_argument("--database", type=Path)
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    database = arguments.database or root / "build/compile_commands.json"
    if not database.is_file():
        print(f"syntaxcheck: REFUSED: compile database does not exist: {database}", file=sys.stderr)
        return 2
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"syntaxcheck: REFUSED: cannot read {database}: {error}", file=sys.stderr)
        return 2

    for source_argument in arguments.sources:
        source = Path(source_argument).resolve()
        matches = [
            entry
            for entry in entries
            if isinstance(entry, dict)
            and isinstance(entry.get("directory"), str)
            and isinstance(entry.get("file"), str)
            and (Path(entry["directory"]) / entry["file"]).resolve() == source
        ]
        if not matches:
            print(f"syntaxcheck: {source} is not in {database} — NOT CHECKED", file=sys.stderr)
            return 2
        entry = matches[-1]
        try:
            command = syntax_command(entry)
        except ValueError as error:
            print(f"syntaxcheck: REFUSED: {source}: {error}", file=sys.stderr)
            return 2
        completed = subprocess.run(command, cwd=entry["directory"], check=False)
        verdict = "OK" if completed.returncode == 0 else "FAILED"
        print(f"syntaxcheck {source.name}: {verdict}")
        if completed.returncode:
            return completed.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
