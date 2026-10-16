"""Refuse an external symbol that two title trees both define from their own sources.

A multi-title product links several title trees into one binary. A name two trees both define (a
strong pair the linker might reject, or a weak/inline pair it silently merges) is a collision unless
every definition comes from a header outside both trees, such as psxport, lucent or the standard
library. A product registers it with `psxport_add_title_link_isolation_test` (cmake/psxport.cmake).

Usage: title_link_isolation.py --nm NM --first LIST --first-root DIR --second LIST --second-root DIR
LIST files name one object per line. Exit 0 isolated, 1 collision, 2 refusal.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

CLASS_DATA_PREFIXES = (
    "vtable for ",
    "typeinfo for ",
    "typeinfo name for ",
    "VTT for ",
    "construction vtable for ",
)

# Entities only the C++ runtime can define; a title tree never owns one.
STANDARD_NAMESPACES = ("std::", "__gnu_cxx::", "__cxxabiv1::")
# Names the compiler emits for itself: PIC code refers to the EH personality through DW.ref.<routine>,
# and Clang gives every TU with a noexcept landing pad its own __clang_call_terminate.
COMPILER_GENERATED_PREFIXES = ("DW.ref.", "__clang_call_terminate")


class Refused(RuntimeError):
    """The inputs cannot support an isolation verdict."""


@dataclass
class ObjectSymbols:
    path: Path
    locations: dict[str, Path | None] = field(default_factory=dict)
    demangled: dict[str, str] = field(default_factory=dict)


def run_nm(nm: str, arguments: Sequence[str], obj: Path) -> list[str]:
    result = subprocess.run(
        [nm, *arguments, str(obj)], capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise Refused(f"{nm} failed on {obj}: {result.stderr.strip()}")
    return result.stdout.splitlines()


def parse_symbol_line(line: str) -> tuple[str, Path | None] | None:
    text, _, location = line.partition("\t")
    parts = text.split(" ", 2)
    if len(parts) != 3:
        return None
    path = location.rsplit(":", 1)[0] if location else ""
    return parts[2], (Path(path).resolve() if path else None)


def read_object(nm: str, obj: Path) -> ObjectSymbols:
    symbols = ObjectSymbols(obj)
    mangled = run_nm(nm, ["-g", "--defined-only", "-l"], obj)
    demangled = run_nm(nm, ["-g", "--defined-only", "-l", "-C"], obj)
    if len(mangled) != len(demangled):
        raise Refused(f"{nm} listed {obj} differently with and without demangling")
    for raw, readable in zip(mangled, demangled):
        parsed = parse_symbol_line(raw)
        parsed_readable = parse_symbol_line(readable)
        if parsed is None or parsed_readable is None:
            continue
        name, location = parsed
        symbols.locations[name] = location
        symbols.demangled[name] = parsed_readable[0]
    if symbols.locations and all(location is None for location in symbols.locations.values()):
        raise Refused(f"{obj} carries no line information; build the title objects with -g")
    return symbols


def entity(readable: str) -> str:
    for prefix in CLASS_DATA_PREFIXES:
        if readable.startswith(prefix):
            return readable[len(prefix) :]
    return readable


def class_location(objects: Sequence[ObjectSymbols], readable: str) -> Path | None:
    for prefix in CLASS_DATA_PREFIXES:
        if readable.startswith(prefix):
            owner = readable[len(prefix) :] + "::"
            for symbols in objects:
                for other, text in symbols.demangled.items():
                    if text.startswith(owner) and symbols.locations[other] is not None:
                        return symbols.locations[other]
    return None


def definition_location(
    symbols: ObjectSymbols, name: str, objects: Sequence[ObjectSymbols]
) -> Path | None:
    location = symbols.locations[name]
    if location is not None:
        return location
    # Class data (vtable, typeinfo) carries no line; its class's members say where the class lives.
    return class_location([symbols, *objects], symbols.demangled[name])


def is_under(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def require_gnu_nm(nm: str) -> None:
    try:
        version = subprocess.run([nm, "--version"], capture_output=True, text=True, check=False)
    except OSError as exc:
        raise Refused(f"cannot run {nm}: {exc}") from exc
    if "GNU nm" not in version.stdout:
        raise Refused(f"{nm} is not GNU nm; definition locations need binutils nm -l")


def read_list(path: Path) -> list[Path]:
    if not path.is_file():
        raise Refused(f"object list {path} does not exist")
    objects = [Path(line) for line in path.read_text().splitlines() if line.strip()]
    if not objects:
        raise Refused(f"object list {path} names no objects")
    missing = [obj for obj in objects if not obj.is_file()]
    if missing:
        raise Refused(f"object list {path} names {len(missing)} missing object(s), first {missing[0]}")
    return objects


def index(tree: Iterable[ObjectSymbols]) -> dict[str, list[ObjectSymbols]]:
    owners: dict[str, list[ObjectSymbols]] = {}
    for symbols in tree:
        for name in symbols.locations:
            owners.setdefault(name, []).append(symbols)
    return owners


def main(arguments: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", required=True)
    parser.add_argument("--first", type=Path, required=True)
    parser.add_argument("--first-root", type=Path, required=True)
    parser.add_argument("--second", type=Path, required=True)
    parser.add_argument("--second-root", type=Path, required=True)
    args = parser.parse_args(arguments)
    try:
        require_gnu_nm(args.nm)
        roots = (args.first_root.resolve(), args.second_root.resolve())
        trees = [
            [read_object(args.nm, obj) for obj in read_list(args.first)],
            [read_object(args.nm, obj) for obj in read_list(args.second)],
        ]
    except Refused as exc:
        print(f"title-link-isolation: refused: {exc}", file=sys.stderr)
        return 2

    first, second = index(trees[0]), index(trees[1])
    everything = [*trees[0], *trees[1]]
    shared = sorted(set(first) & set(second))
    collisions: list[str] = []
    unresolved: list[str] = []
    for name in shared:
        holders = first[name] + second[name]
        locations = [definition_location(symbols, name, everything) for symbols in holders]
        readable = holders[0].demangled[name]
        if entity(readable).startswith(STANDARD_NAMESPACES) or readable.startswith(COMPILER_GENERATED_PREFIXES):
            continue
        if any(location is None for location in locations):
            unresolved.append(readable)
            continue
        owned = [
            str(location)
            for location in locations
            if location is not None and any(is_under(location, root) for root in roots)
        ]
        if owned:
            collisions.append(f"{readable} defined at {owned[0]}")

    print(
        f"title-link-isolation: {sum(len(tree) for tree in trees)} object(s), "
        f"{len(first)} + {len(second)} defined external symbol(s), {len(shared)} shared, "
        f"{len(collisions)} collision(s), {len(unresolved)} without a definition location"
    )
    for line in collisions:
        print(f"  collision: {line}")
    for line in unresolved:
        print(f"  unresolved: {line}")
    return 1 if collisions or unresolved else 0


if __name__ == "__main__":
    raise SystemExit(main())
