#!/usr/bin/env python3
"""Guest code to readable C, for one title: import, analyze, inventory, decompile a target set.

THE ONE COMMAND a per-title agent needs::

    uv run --frozen python external/psxport/tools/decomp_pipeline.py \
        --title spyro1 --image scratch/assets/spyro1/SCUS_942.28 \
        --target 0x800258F0 --target 0x800259FC --out scratch/decomp/spyro1

`external/psxport` is the framework (a symlink to the one writable tree), so the path is the same in
every port. The image is the admitted PS-X EXE, provisioned by that title's own tooling; the
targets are guest entry addresses, and the output lands under a git-ignored `scratch/`.

Exit codes, and they mean different things: 0 the run was audited clean; 1 a REFUSAL (missing
image, wrong base, no targets, unreachable Ghidra, another Ghidra held the lock); 2 the run completed
but its own audit found something untrustworthy -- a target with no body, or a non-return flag still
set. A run that produced nothing trustworthy never reports success.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from decomp import headless, images, lock, pipeline, report as report_module  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--title", help="a title in tools/decomp/manifest.json")
    parser.add_argument("--image", type=Path,
                        help="the admitted PS-X EXE (or an overlay, with its own manifest entry)")
    parser.add_argument("--target", action="append", default=[], metavar="0xADDR",
                        help="a guest entry address to decompile; repeatable")
    parser.add_argument("--out", type=Path,
                        help="a git-ignored output directory, normally scratch/decomp/<title>")
    parser.add_argument("--ghidra-home", type=Path, default=headless.DEFAULT_GHIDRA_HOME)
    parser.add_argument("--lock-dir", type=Path, default=None,
                        help="the one-Ghidra-at-a-time lock; defaults to the workspace coord/locks")
    parser.add_argument("--lock-wait", type=float, default=1800.0,
                        help="seconds to wait for the lock before refusing (default 1800)")
    parser.add_argument("--heap-mb", type=int, default=headless.DEFAULT_MAX_HEAP_MB,
                        help="Ghidra's -Xmx ceiling (default %d)" % headless.DEFAULT_MAX_HEAP_MB)
    parser.add_argument("--timeout", type=int, default=headless.DEFAULT_TIMEOUT_SECONDS,
                        help="seconds before the run is called a timeout (default %d)"
                             % headless.DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--inventory-limit", type=int, default=40,
                        help="how many inventory rows to print (default 40; 0 prints all)")
    parser.add_argument("--list-titles", action="store_true",
                        help="print the manifest and exit, so a caller can see what exists")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.list_titles:
        specs = images.load_manifest()
        print("%d titles in the manifest:" % len(specs))
        for spec in sorted(specs.values(), key=lambda s: s.title):
            fields = spec.as_dict()
            print("  %-12s %-14s load=%s offset=%s base=%s text=%s" % (
                fields["title"], fields["serial"], fields["text_load_address"],
                fields["text_file_offset"], fields["ghidra_base"],
                "0x%X" % spec.text_size if spec.text_size else "?"))
            if spec.note:
                print("               %s" % spec.note)
        return 0

    if not args.target:
        print("[decomp] REFUSED: no --target given. Name the functions to read; a run with no "
              "targets decompiles nothing and is not a result.")
        return 1
    try:
        targets = sorted({int(t, 16) for t in args.target})
    except ValueError as error:
        print("[decomp] REFUSED: %s is not a hex guest address." % error)
        return 1

    run = pipeline.DecompRun(
        title=args.title,
        image_path=args.image,
        output_dir=args.out,
        targets=targets,
        ghidra_home=args.ghidra_home,
        lock_dir=args.lock_dir,
        lock_wait_seconds=args.lock_wait,
        clear_noreturn="all",
        max_heap_mb=args.heap_mb,
        timeout_seconds=args.timeout,
    )

    try:
        result = pipeline.execute(run, repo_root=REPO_ROOT)
    except (images.ImageRefusal, lock.LockRefusal, headless.GhidraRefusal,
            report_module.ReportRefusal) as error:
        print("[decomp] REFUSED: %s" % error)
        return 1

    print(result.header())
    print("[decomp] function inventory (%d scanned):" % result.functions_scanned)
    limit = None if args.inventory_limit in (0, None) else args.inventory_limit
    print(result.inventory_text(limit=limit))
    print("[decomp] targets (%d requested):" % result.targets_requested)
    print(result.target_text())

    problems = report_module.audit(result)
    if problems:
        print("[decomp] AUDIT FAILED -- %d problem(s). The output above is NOT trustworthy:" %
              len(problems))
        for problem in problems:
            print("  - %s" % problem)
        return 2
    print("[decomp] AUDIT OK: no-return cleared on %d of %d functions, %d of %d targets carry a "
          "real body." % (result.noreturn_cleared, result.functions_scanned,
                          result.targets_with_body, result.targets_requested))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
