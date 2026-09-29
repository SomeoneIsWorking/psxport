"""What the inventory claims, with denominators, and whether it can be believed.

ONE CONCEPT: reading the post-script's document and turning it into a report that CANNOT be read as
the opposite question. Nothing here re-derives a Ghidra fact; it checks the run's own claims and
refuses the ones that fail.

THE REPORT CONTRACT, which is the whole reason this module exists:

  * every count is ``matched of scanned``, never a bare number, and a zero is stated as a zero;
  * ``functions_scanned == 0`` is a REFUSAL, because an inventory of nothing is a failed run that
    answered "0" -- "0 of 0" is a bug, not a result;
  * the no-return policy is checked to have been APPLIED (nothing still marked), not merely
    requested;
  * every requested target must have a present body, and one that does not is named, with the
    reason, so a truncated decompile cannot sit in the output looking finished.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path


class ReportRefusal(Exception):
    """The run produced an inventory that cannot be believed, or did not produce one."""


@dataclass(frozen=True)
class TargetResult:
    requested: str
    function_found: bool
    decompiled: bool
    body_present: bool
    body_reason: str
    instruction_count: int
    c_path: str | None
    c_bytes: int
    name: str | None = None
    inside_function: str | None = None
    inside_function_name: str | None = None
    inside_body_range: str | None = None
    offset_into_body: int | None = None

    def row(self) -> str:
        inside = ""
        if self.inside_function:
            inside = " inside=%s" % self.inside_function
        return ("  %-12s %-22s found=%-3s decompiled=%-3s body=%-3s insns=%-4d c=%sB%s  %s"
                % (self.requested, self.name or "-", str(self.function_found),
                   str(self.decompiled), str(self.body_present), self.instruction_count,
                   self.c_bytes, inside, self.body_reason))


@dataclass
class Report:
    """One run's numbers. Every field is a count with its denominator attached."""

    program: str
    language: str
    noreturn_policy: str
    noreturn_cleared: int
    functions_scanned: int
    inventory: list[dict]
    targets: list[TargetResult]
    noreturn_still_marked: list[str] = field(default_factory=list)
    preseed: dict | None = None
    warnings: list[str] = field(default_factory=list)

    # -- denominators, each a matched-of-scanned pair -------------------------------------------------

    @property
    def targets_requested(self) -> int:
        return len(self.targets)

    @property
    def targets_found(self) -> int:
        return sum(1 for t in self.targets if t.function_found)

    @property
    def targets_decompiled(self) -> int:
        return sum(1 for t in self.targets if t.decompiled)

    @property
    def targets_with_body(self) -> int:
        return sum(1 for t in self.targets if t.body_present)

    @property
    def functions_with_instructions(self) -> int:
        return sum(1 for f in self.inventory if f.get("instruction_count", 0) > 0)

    def inventory_text(self, limit: int | None = None) -> str:
        lines = ["  %-12s %-24s %-10s %-8s %s" % ("entry", "name", "body", "insns", "body range")]
        rows = sorted(self.inventory, key=lambda f: int(f["entry"], 16))
        shown = rows if limit is None else rows[:limit]
        for entry in shown:
            lines.append("  %-12s %-24s %-10s %-8d %s..%s" % (
                entry["entry"], entry["name"], "%dB" % entry.get("body_bytes", 0),
                entry.get("instruction_count", 0), entry.get("body_first", "?"),
                entry.get("body_last", "?")))
        if limit is not None and len(rows) > limit:
            lines.append("  ... %d of %d functions listed; the rest are in the JSON"
                         % (limit, len(rows)))
        return "\n".join(lines)

    def header(self) -> str:
        seed = ("no pre-script record" if self.preseed is None else
                "entry=%s instructions=%d" % (self.preseed.get("entry", "?"),
                                              self.preseed.get("instructions_from_entry", 0)))
        return (
            "[decomp] program=%s language=%s\n"
            "[decomp] pre-script seed: %s\n"
            "[decomp] no-return policy=%s cleared=%d of %d functions; still marked after clear=%d\n"
            "[decomp] functions scanned=%d, of which %d hold at least one instruction\n"
            "[decomp] targets requested=%d, function found=%d, decompiled=%d, body present=%d"
            % (self.program, self.language, seed, self.noreturn_policy, self.noreturn_cleared,
               self.functions_scanned, len(self.noreturn_still_marked), self.functions_scanned,
               self.functions_with_instructions, self.targets_requested, self.targets_found,
               self.targets_decompiled, self.targets_with_body)
        )

    def target_text(self) -> str:
        return "\n".join(t.row() for t in self.targets)


def load_report(path: Path) -> Report:
    """Read the post-script's document, or REFUSE. Never an empty report for an empty file."""
    path = Path(path)
    if not path.is_file():
        raise ReportRefusal(
            f"no inventory at {path}. The run produced no document, so nothing was established; "
            "this is reported as a refusal rather than as an empty result."
        )
    try:
        raw = json.loads(path.read_text())
    except json.JSONDecodeError as error:
        raise ReportRefusal(f"{path} is not valid JSON, so the run's own output is unreadable: "
                            f"{error}") from error
    for key in ("program", "noreturn_policy", "functions_scanned", "inventory", "targets"):
        if key not in raw:
            raise ReportRefusal(
                f"{path} has no {key!r}, so it was not written by this post-script. Refusing rather "
                "than reading a partial document as a result."
            )
    targets = [
        TargetResult(
            requested=t["requested"],
            function_found=bool(t.get("function_found")),
            decompiled=bool(t.get("decompiled")),
            body_present=bool(t.get("body_present")),
            body_reason=t.get("body_reason", ""),
            instruction_count=int(t.get("instruction_count", 0)),
            c_path=t.get("c_path"),
            c_bytes=int(t.get("c_bytes", 0)),
            name=t.get("name"),
            inside_function=t.get("inside_function"),
            inside_function_name=t.get("inside_function_name"),
            inside_body_range=t.get("inside_body_range"),
            offset_into_body=t.get("offset_into_body"),
        )
        for t in raw["targets"]
    ]
    return Report(
        program=raw["program"],
        language=raw.get("language", "?"),
        noreturn_policy=raw["noreturn_policy"],
        noreturn_cleared=int(raw.get("noreturn_cleared", 0)),
        functions_scanned=int(raw["functions_scanned"]),
        inventory=raw["inventory"],
        targets=targets,
        noreturn_still_marked=list(raw.get("noreturn_still_marked", [])),
        preseed=raw.get("preseed"),
    )


def audit(report: Report, *, strict_policy: str = "all") -> list[str]:
    """The problems that make this run untrustworthy. An EMPTY list is the pass condition.

    Returned rather than raised so the caller can print every problem at once; a report that stops
    at the first failure hides the rest, and the reader then fixes one defect, re-runs, and meets the
    next.
    """
    problems: list[str] = []
    if report.preseed is None:
        problems.append(
            "the pre-script left no record of what it did, so disassembly was seeded by something "
            "this run cannot name. A pre-script that raises leaves Ghidra running the analysis "
            "anyway and logging 'Post-analysis succeeded', so its absence has to be refused rather "
            "than read as 'it worked'.")
    elif report.preseed.get("instructions_from_entry", 0) <= 0:
        problems.append(
            "the pre-script disassembled from the declared entry %s and got 0 instructions, so the "
            "seed did not happen."
            % report.preseed.get("entry", "?"))
    if report.functions_scanned == 0:
        problems.append(
            "the scan found 0 of 0 functions, which is a failed run rather than an empty image: "
            "Ghidra reports success for an import whose script never ran. Check that the launcher "
            "is the PyGhidra module, not analyzeHeadless.")
    if report.targets_requested == 0:
        problems.append(
            "0 of 0 targets were requested. A run with no targets decompiles nothing and is not a "
            "result.")
    if strict_policy == "all" and report.noreturn_policy != "all":
        problems.append(
            "the run applied no-return policy %r where 'all' was required. Every caller may "
            "decompile truncated with a fabricated return." % report.noreturn_policy)
    if report.noreturn_still_marked:
        shown = ", ".join(report.noreturn_still_marked[:8])
        more = "" if len(report.noreturn_still_marked) <= 8 else \
            " (+%d more)" % (len(report.noreturn_still_marked) - 8)
        problems.append(
            "%d function(s) are STILL marked non-returning after the clear (%s%s), so the flag was "
            "not actually applied. Their callers decompile with the body after the call discarded."
            % (len(report.noreturn_still_marked), shown, more))
    for target in report.targets:
        if target.inside_function:
            # A SEPARATE PROBLEM, and a different fix from "no function here": the address is a
            # label inside a body that IS present, so the answer is to read the enclosing function.
            # Merging this into the no-function message would send a reader to check the load base
            # for a case where the load base is fine and the body was found perfectly.
            problems.append(
                "target %s is +%s bytes INSIDE %s's body (%s), not a function entry, so it was NOT "
                "decompiled. Read the enclosing function: carving a function here splits its body "
                "and yields two overlapping decompilations that both look complete."
                % (target.requested, target.offset_into_body,
                   target.inside_function_name or target.inside_function,
                   target.inside_body_range or "?"))
        elif not target.function_found:
            problems.append(
                "target %s has NO function at that address, and it is not inside another function's "
                "body either. Auto-analysis only defines a function where it saw a call or branch, "
                "so a jump-table-only entry is the first thing to check; this message does not "
                "establish a cause." % target.requested)
        elif not target.decompiled:
            problems.append(
                "target %s (%s) has a function but the decompiler produced no C, so it has not "
                "been read." % (target.requested, target.name or "-"))
        elif not target.body_present:
            problems.append(
                "target %s (%s) decompiled to %d byte(s) but its body is NOT present: %s"
                % (target.requested, target.name or "-", target.c_bytes, target.body_reason))
    return problems
