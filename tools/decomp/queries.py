"""The questions an agent asks ABOUT an image, and the report that cannot be read as another question.

ONE CONCEPT: the query side of the pipeline. ``--callers``, ``--refs`` and ``--function-at`` are not
three tools and not three code paths; they are three ROW TYPES in one query set, answered in ONE Ghidra
launch, printed with the same denominators as a decompile run.

WHY THIS IS A SEPARATE OWNER. The decompile path answers "what does this function say", and its report
already refuses a body that is not there. A reference query answers a different question -- "who
reaches this address" -- and its failure modes are different in kind: a query set that silently lost
its last address, an address that is not in memory answering "0 references" as though absence had been
established, and a count of sites with no statement of what those sites were drawn from. Sharing the
decompile report would mean weakening it, so this module owns its own report and its own audit.

THE CONTRACT, which is the whole reason this file exists:

  * every query is ANSWERED, in the order asked, or the run REFUSES. A short answer is the exact
    mechanism by which a probe manufactures its own result, so :func:`audit` compares the requested
    addresses against the answered ones rather than trusting the writer of the document;
  * every answer states the SCANNED RANGE and the SCANNED FUNCTION COUNT it was drawn from, because
    "0 references" means something different inside a 340 KB text window than inside a 24 B module;
  * an address OUTSIDE the program's memory is answered as such -- ``in_memory: false`` with the
    range printed -- and never as a bare zero, because no reference to it could exist;
  * a zero is printed as a zero, in words, with the denominator next to it.

THE PARSER IS MIRRORED IN ``queryscript.py``, which runs inside Ghidra's own interpreter and cannot
import a repository module (PyGhidra puts only the script's directory on ``sys.path``). That mirror is
PINNED BY TEST, exactly like :mod:`tools.decomp.structure` against ``postscript.py``: ``tests/
test_decomp_pipeline.py`` runs both parsers over a corpus of query files and requires them to agree, so
two copies cannot drift silently.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

# The three questions. These strings are the ON-WIRE format: they appear in the query file the
# post-script reads, so a spelling here is a spelling in a file another process parses.
CALLERS = "callers"
REFS = "refs"
FUNCTION_AT = "function_at"
KINDS = (CALLERS, REFS, FUNCTION_AT)
# The command-line spelling of each kind, which is not always the on-disk one: `--function-at` reads
# as one word to an agent and as two in the query file. Accepting both and WRITING the canonical
# token removes a trap where a hand-written or copied query file is silently refused for a hyphen.
ALIASES = {"function-at": FUNCTION_AT, "function_at": FUNCTION_AT,
           "callers": CALLERS, "refs": REFS}

# The document keys a well-formed answer carries. Listed here rather than checked inline so the
# refusal and the loader cannot disagree about what "present" means.
REQUIRED_QUERY_KEYS = ("mode", "address", "in_memory", "sites")
REQUIRED_TOP_KEYS = ("program", "memory_first", "memory_last", "functions_scanned",
                     "queries_requested", "queries")


class QueryRefusal(Exception):
    """The query set cannot be served, or an answer document cannot be believed. Never a short read."""


@dataclass(frozen=True)
class Query:
    """One question about one address. ``address`` is a guest address, never an offset."""

    kind: str
    address: int

    def line(self) -> str:
        return "%s %08X" % (self.kind, self.address)

    def label(self) -> str:
        return "%s 0x%08X" % (self.kind, self.address)


def parse_query_text(text: str, origin: str = "<queries>") -> list[Query]:
    """``kind address`` per line, ``#`` to end of line. Refuses anything it cannot serve.

    The rules are the ones ``postscript.read_targets`` already follows, for the same reason: a comment
    that runs past its line turns the words after it into addresses, and a malformed token that is
    skipped turns a typo into a silently smaller question. An EMPTY set is refused, because a query
    run that answers nothing reports success having established nothing.
    """
    body = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    queries: list[Query] = []
    seen: set[tuple[str, int]] = set()
    for number, raw in enumerate(body.splitlines(), start=1):
        fields = raw.replace(",", " ").split()
        if not fields:
            continue
        if len(fields) != 2:
            raise QueryRefusal(
                f"{origin} line {number}: expected `kind address`, got {raw.strip()!r}. Every line "
                "is one question, so a line that cannot be read is a question that would be dropped "
                "rather than answered."
            )
        kind = ALIASES.get(fields[0].lower(), fields[0].lower())
        if kind not in KINDS:
            raise QueryRefusal(
                f"{origin} line {number}: {kind!r} is not a question this tool answers. The kinds "
                f"are {', '.join(KINDS)} -- 'who calls it', 'what reads or writes it', and 'which "
                "function is at this PC'."
            )
        try:
            address = int(fields[1], 16)
        except ValueError as error:
            raise QueryRefusal(
                f"{origin} line {number}: {fields[1]!r} is not a hex guest address."
            ) from error
        if not 0 <= address <= 0xFFFFFFFF:
            raise QueryRefusal(
                f"{origin} line {number}: 0x{address:X} is not a 32-bit guest address.")
        if (kind, address) in seen:
            continue
        seen.add((kind, address))
        queries.append(Query(kind, address))
    if not queries:
        raise QueryRefusal(
            f"{origin} holds no questions. A run with no queries answers nothing and is not a "
            "result; name the addresses with --callers, --refs or --function-at."
        )
    return queries


def parse_query_file(path: Path) -> list[Query]:
    """Read and parse a query file, or REFUSE. An absent file is not an empty question set."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as error:
        raise QueryRefusal(
            f"cannot read the query list at {path}: {error}. The post-script reads the questions "
            "from this file, so a run that cannot read it answers whatever it was given."
        ) from error
    return parse_query_text(text, origin=str(path))


def write_query_file(path: Path, queries: list[Query]) -> Path:
    """Write the query set, one question per line, in the order asked."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(q.line() for q in queries) + "\n", encoding="utf-8")
    return path


# ---------------------------------------------------------------------------------------------
# The answer document.
# ---------------------------------------------------------------------------------------------

@dataclass
class QueryAnswer:
    mode: str
    address: str
    in_memory: bool
    sites: list[dict]
    # `refs`/`callers`
    references_found: int = 0
    code_references: int = 0
    data_references: int = 0
    functions_touched: int = 0
    unattributed_sites: int = 0
    # `function_at`
    function_found: bool = False
    entry: str | None = None
    name: str | None = None
    body_first: str | None = None
    body_last: str | None = None
    instruction_count: int = 0
    is_entry: bool = False
    offset_into_body: int | None = None
    # Anything the answerer wants on the record, printed after the counts.
    note: str = ""

    def summary(self) -> str:
        """The one line that says what was scanned, what matched, and what it means."""
        if not self.in_memory:
            return ("%s: this address is OUTSIDE the memory of the analyzed program, so 0 "
                    "references were returned and none could exist." % self.label())
        if self.mode == FUNCTION_AT:
            if not self.function_found:
                return ("%s: no function is defined here and the address is not inside another "
                        "function's body either. That establishes only that auto-analysis defined "
                        "no function containing it; it does NOT establish why." % self.label())
            where = "entry" if self.is_entry else "+%d bytes inside" % (self.offset_into_body or 0)
            return ("%s: %s %s, %s, body %s..%s, %d instruction(s)" % (
                self.label(), where, self.name or "-", self.entry, self.body_first or "?",
                self.body_last or "?", self.instruction_count))
        unattributed = ""
        if self.unattributed_sites:
            unattributed = "; %d of %d site(s) are inside NO function and are listed anyway" % (
                self.unattributed_sites, self.references_found)
        return ("%s: %d reference(s) to it (%d code, %d data) from %d function(s)%s%s" % (
            self.label(), self.references_found, self.code_references, self.data_references,
            self.functions_touched, unattributed, (" -- " + self.note) if self.note else ""))

    def label(self) -> str:
        return "%s %s" % (self.mode, self.address)

    def site_lines(self) -> list[str]:
        lines = []
        for site in self.sites:
            lines.append("    %-10s %-10s %-9s %-12s %-24s %s" % (
                site.get("from", "?"), site.get("type", "?"), site.get("access", "?"),
                site.get("function", "?"), site.get("function_name", "-"), site.get("text", "")))
        return lines


@dataclass
class QueryReport:
    program: str
    image_name: str
    image_kind: str
    memory_first: str
    memory_last: str
    functions_scanned: int
    queries_requested: int
    answers: list[QueryAnswer]
    # What the run actually asked, read back from the query FILE the post-script consumed. The audit
    # compares this against the answers, so a dropped question is caught here rather than by the
    # process that dropped it.
    asked: list[Query] | None = None

    def header(self) -> str:
        return ("[decomp] queries: %d asked, %d answered, against memory %s..%s of %s "
                "(%d function(s) in the program). A count is references the ANALYZER holds: this is "
                "static cross-reference scope, not runtime execution."
                % (self.queries_requested, len(self.answers), self.memory_first, self.memory_last,
                   self.program or "?", self.functions_scanned))

    def text(self) -> str:
        lines = []
        for answer in self.answers:
            lines.append("  " + answer.summary())
            lines.extend(answer.site_lines())
        return "\n".join(lines)


def _answer_from_document(entry: dict) -> QueryAnswer:
    for key in REQUIRED_QUERY_KEYS:
        if key not in entry:
            raise QueryRefusal(
                f"a query answer has no {key!r}, so it was not written by this query script. Refusing "
                "rather than reading a partial answer as a complete one. The offending answer: "
                "%r" % (entry,))
    sites = entry["sites"]
    if not isinstance(sites, list):
        raise QueryRefusal("a query answer's 'sites' is not a list: %r" % (entry,))
    answer = QueryAnswer(
        mode=entry["mode"],
        address=entry["address"],
        in_memory=bool(entry["in_memory"]),
        sites=sites,
    )
    if answer.mode in (CALLERS, REFS):
        # The DENOMINATOR check, and the reason this loader is not a json.load: an answer whose
        # `references_found` disagrees with the sites it carries has either dropped sites or
        # invented the count, and both read identically to a reader in a hurry.
        for key in ("references_found", "code_references", "data_references", "functions_touched",
                    "unattributed_sites"):
            if key not in entry:
                raise QueryRefusal(
                    f"a {answer.mode} answer for {answer.address} has no {key!r}, so the count it "
                    "reports cannot be checked against the sites it lists. Refusing.")
        answer.references_found = int(entry["references_found"])
        answer.data_references = int(entry["data_references"])
        answer.code_references = int(entry["code_references"])
        answer.functions_touched = int(entry["functions_touched"])
        answer.unattributed_sites = int(entry["unattributed_sites"])
        answer.note = entry.get("note", "")
        if answer.references_found != len(sites):
            raise QueryRefusal(
                f"the answer for {answer.label()} claims {answer.references_found} reference(s) but "
                f"carries {len(sites)} site(s). One of them is wrong, and a count that does not "
                "match what it counts is the number a reader would quote.")
        if answer.code_references + answer.data_references != answer.references_found:
            raise QueryRefusal(
                f"the answer for {answer.label()} splits {answer.references_found} reference(s) "
                f"into {answer.code_references} code and {answer.data_references} data, which do not "
                "add up. The split is what tells a caller from a datum, so it cannot be approximate.")
        if answer.unattributed_sites > len(sites):
            raise QueryRefusal(
                f"the answer for {answer.label()} claims {answer.unattributed_sites} unattributed "
                f"site(s) out of {len(sites)} listed.")
    else:
        for key in ("function_found", "instruction_count"):
            if key not in entry:
                raise QueryRefusal(f"a function_at answer for {answer.address} has no {key!r}.")
        answer.function_found = bool(entry["function_found"])
        answer.entry = entry.get("entry")
        answer.name = entry.get("name")
        answer.body_first = entry.get("body_first")
        answer.body_last = entry.get("body_last")
        answer.instruction_count = int(entry["instruction_count"])
        answer.is_entry = bool(entry.get("is_entry"))
        answer.offset_into_body = entry.get("offset_into_body")
        answer.note = entry.get("note", "")
    return answer


def load_query_report(path: Path, asked: list[Query] | None = None) -> QueryReport:
    """Read the answer document, or REFUSE. Never an empty report for an empty file."""
    path = Path(path)
    if not path.is_file():
        raise QueryRefusal(
            f"no query document at {path}. The run produced no answers, so nothing was established; "
            "reported as a refusal rather than as an empty result."
        )
    try:
        raw = json.loads(path.read_text())
    except json.JSONDecodeError as error:
        raise QueryRefusal(f"{path} is not valid JSON: {error}") from error
    for key in REQUIRED_TOP_KEYS:
        if key not in raw:
            raise QueryRefusal(
                f"{path} has no {key!r}, so it was not written by the query script. Refusing rather "
                "than reading a partial document as a result.")
    answers = [_answer_from_document(entry) for entry in raw["queries"]]
    return QueryReport(
        program=raw["program"],
        image_name=raw.get("image_name", ""),
        image_kind=raw.get("image_kind", ""),
        memory_first=raw["memory_first"],
        memory_last=raw["memory_last"],
        functions_scanned=int(raw["functions_scanned"]),
        queries_requested=int(raw["queries_requested"]),
        answers=answers,
        asked=list(asked) if asked is not None else None,
    )


def audit(report: QueryReport) -> list[str]:
    """The problems that make a query run untrustworthy. An EMPTY list is the pass condition."""
    problems: list[str] = []
    if report.functions_scanned == 0:
        problems.append(
            "the program holds 0 of 0 functions, so every reference count below is a count of "
            "nothing. That is a run whose script never saw an analyzed program (the import path "
            "with -noanalysis, or a failed analysis), and it looks exactly like an image with no "
            "cross-references."
        )
    if report.queries_requested == 0:
        problems.append("0 of 0 queries were requested, which answers nothing.")
    answered = [(a.mode, int(a.address, 16)) for a in report.answers]
    if report.asked is not None:
        for query in report.asked:
            key = (query.kind, query.address)
            if key not in answered:
                problems.append(
                    "query %s was asked and is absent from the answers (%d answered of %d asked). "
                    "A dropped question is indistinguishable from a question with no result, which "
                    "is why it is refused rather than printed as a zero."
                    % (query.label(), len(report.answers), len(report.asked)))
    if len(report.answers) != report.queries_requested:
        problems.append(
            "the document claims %d queries requested and carries %d answers."
            % (report.queries_requested, len(report.answers)))
    for answer in report.answers:
        if answer.mode in (CALLERS, REFS) and answer.in_memory and answer.references_found == 0:
            # Not a FAILURE -- a genuine zero is a result -- but it IS a sentence the reader must be
            # given, and it must not be the same sentence as "outside memory".
            if "0 reference" not in answer.summary():
                problems.append(
                    "the answer for %s is a zero that does not say so; a reader could take it for an "
                    "address with results that were dropped." % answer.label())
    return problems