#!/usr/bin/env python3
"""Guest code to readable C and guest cross-references, for one title: import, analyze, ask.

THE ONE COMMAND a per-title agent needs::

    uv run --frozen python external/psxport/tools/decomp_pipeline.py \
        --image-name spyro1 --image scratch/assets/spyro1/SCUS_942.28 \
        --target 0x800258F0 --target 0x800259FC --out scratch/decomp/spyro1

    # and, once the image has been analyzed once, the questions -- many per run, one Ghidra launch:
    uv run --frozen python external/psxport/tools/decomp_pipeline.py \
        --image-name spyro2 --image scratch/assets/spyro2/SCUS_944.25 \
        --callers 0x80044AE0 --refs 0x800A11E4 --function-at 0x80044B00 \
        --out scratch/decomp/spyro2

`external/psxport` is the framework (a link to the one writable tree), so the path is the same in
every port. The image is the admitted PS-X EXE or overlay, provisioned by that title's own tooling;
`--target` names guest entry addresses to decompile, the query flags name addresses to ask about, and
the output lands under a git-ignored `scratch/`.

ANALYSIS IS CACHED, and that is what makes the second command cheap. The Ghidra project for an image
is kept under the CONSUMING repository's ``build/ghidra/<image>/<sha256>/``, keyed by the image's
SHA-256, so a different region or revision can never be answered by the wrong analysis. The first run
imports and analyzes; later runs open that project with ``-noanalysis`` and ask. The mode and the wall
time of every run are printed, because "it is faster now" is a claim and a stopwatch is the evidence.
``--fresh`` discards the cached analysis and re-analyzes.

Exit codes, and they mean different things: 0 the run was audited clean; 1 a REFUSAL (missing image,
wrong base, nothing asked, unreachable Ghidra, another Ghidra held the lock); 2 the run completed but
its own audit found something untrustworthy -- a target with no body, a non-return flag still set, or
a query whose count does not match the sites it lists. A run that produced nothing trustworthy never
reports success.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from decomp import cache, headless, images, lock, pipeline, queries as queries_module  # noqa: E402
from decomp import report as report_module  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image-name", help="an image entry in tools/decomp/manifest.json: a "
                                              "resident title, or a module/overlay name")
    parser.add_argument("--target", action="append", default=[], metavar="0xADDR",
                        help="a guest entry address to decompile; repeatable")
    parser.add_argument("--callers", action="append", default=[], metavar="0xADDR",
                        help="which functions CALL this address; repeatable")
    parser.add_argument("--refs", action="append", default=[], metavar="0xADDR",
                        help="what READS or WRITES this address, with the access and the enclosing "
                             "function; repeatable")
    parser.add_argument("--function-at", action="append", default=[], metavar="0xPC",
                        help="which function contains this PC, and whether it is an entry or a label "
                             "inside a body; repeatable")
    parser.add_argument("--out", type=Path,
                        help="a git-ignored output directory, normally scratch/decomp/<title>")
    parser.add_argument("--project-dir", type=Path, default=None,
                        help="where the analyzed Ghidra project lives. Defaults to the consuming "
                             "repository's build/ghidra/<image>/<sha256>. A directory named here is "
                             "yours: --fresh will refuse to delete it.")
    parser.add_argument("--fresh", action="store_true",
                        help="discard the cached analysis for this image and analyze it again")
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
    parser.add_argument("--image", type=Path,
                        help="the admitted image file: a title's PS-X EXE, or a module/overlay")
    parser.add_argument("--list-titles", action="store_true",
                        help="print the manifest and exit, so a caller can see what exists")
    return parser


def _collect_queries(args) -> list[queries_module.Query]:
    """The question set, in the order the flags were filled, with duplicates collapsed.

    A malformed address refuses here, before a Ghidra launch is even considered: the alternative is
    a queue behind other agents and then a refusal, which is the same refusal with a worse cost.
    """
    collected: list[queries_module.Query] = []
    for kind, values in ((queries_module.CALLERS, args.callers),
                         (queries_module.REFS, args.refs),
                         (queries_module.FUNCTION_AT, args.function_at)):
        for value in values:
            try:
                address = int(value, 16)
            except ValueError as error:
                raise queries_module.QueryRefusal(
                    "--%s %r is not a hex guest address." % (kind.replace("_", "-"), value)) from error
            if not 0 <= address <= 0xFFFFFFFF:
                raise queries_module.QueryRefusal(
                    "--%s 0x%X is not a 32-bit guest address." % (kind.replace("_", "-"), address))
            collected.append(queries_module.Query(kind, address))
    unique: list[queries_module.Query] = []
    seen = set()
    for query in collected:
        if (query.kind, query.address) in seen:
            continue
        seen.add((query.kind, query.address))
        unique.append(query)
    return unique


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.list_titles:
        specs = images.load_manifest()
        residents = [s for s in specs.values() if s.kind == images.RESIDENT]
        modules = [s for s in specs.values() if s.kind == images.MODULE]
        print("%d images in the manifest: %d resident, %d module" % (
            len(specs), len(residents), len(modules)))
        for spec in sorted(specs.values(), key=lambda s: (s.kind, s.name)):
            fields = spec.as_dict()
            print("  %-9s %-16s %-14s base=%s window=%s%s" % (
                fields["kind"], fields["name"], fields["serial"] or "-", fields["ghidra_base"],
                fields["window"], "  sha1=%s" % fields["sha1"] if fields["sha1"] else ""))
            if spec.note:
                print("             %s" % fields["note"])
        return 0

    try:
        targets = sorted({int(t, 16) for t in args.target})
        questions = _collect_queries(args)
    except (ValueError, queries_module.QueryRefusal) as error:
        print("[decomp] REFUSED: %s" % error)
        return 1

    if not targets and not questions:
        print("[decomp] REFUSED: nothing was asked. Name functions to read with --target, or "
              "addresses to ask about with --callers, --refs or --function-at. A run with neither "
              "decompiles nothing, answers nothing, and is not a result.")
        return 1
    if args.image is None:
        print("[decomp] REFUSED: --image is required: the path to the admitted PS-X EXE, or to a "
              "module/overlay. The manifest holds the load geometry, not the file.")
        return 1
    run = pipeline.DecompRun(
        image_name=args.image_name,
        image_path=args.image,
        output_dir=args.out,
        targets=targets,
        queries=questions,
        project_dir=args.project_dir,
        fresh=args.fresh,
        ghidra_home=args.ghidra_home,
        lock_dir=args.lock_dir,
        lock_wait_seconds=args.lock_wait,
        clear_noreturn="all",
        max_heap_mb=args.heap_mb,
        timeout_seconds=args.timeout,
    )

    try:
        outcome = pipeline.execute(run)
    except (images.ImageRefusal, lock.LockRefusal, headless.GhidraRefusal, cache.CacheRefusal,
            queries_module.QueryRefusal, report_module.ReportRefusal) as error:
        print("[decomp] REFUSED: %s" % error)
        return 1

    # The mode and the stopwatch, printed FIRST and in the same line, so "it got fast" is a number a
    # reader can compare rather than a property of the tool they have to take on trust.
    print("[decomp] %s run in %.1f s; project %s (%s), analyzed=%s"
          % (outcome.mode, outcome.seconds, outcome.slot.directory,
             outcome.slot.sha256[:16], cache.is_analyzed(outcome.slot)))

    problems = []
    if outcome.report is not None:
        result = outcome.report
        print(result.header())
        print("[decomp] function inventory (%d scanned):" % result.functions_scanned)
        limit = None if args.inventory_limit in (0, None) else args.inventory_limit
        print(result.inventory_text(limit=limit))
        print("[decomp] targets (%d requested):" % result.targets_requested)
        print(result.target_text())
        problems.extend(report_module.audit(result))
    if outcome.query_report is not None:
        print(outcome.query_report.header())
        print("[decomp] queries (%d asked):" % len(outcome.query_report.asked or []))
        print(outcome.query_report.text())
        problems.extend(queries_module.audit(outcome.query_report))

    if problems:
        print("[decomp] AUDIT FAILED -- %d problem(s). The output above is NOT trustworthy:" %
              len(problems))
        for problem in problems:
            print("  - %s" % problem)
        return 2
    print("[decomp] AUDIT OK%s" % (
        ": no-return cleared on %d of %d functions, %d of %d targets carry a real body." % (
            outcome.report.noreturn_cleared, outcome.report.functions_scanned,
            outcome.report.targets_with_body, outcome.report.targets_requested)
        if outcome.report is not None else
        ": every query asked is answered, and every count matches the sites it lists."))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())