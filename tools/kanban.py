#!/usr/bin/env python3
"""kanban.py — the project's LOCAL bug/issue tracker (replaces the old GitHub-Issues bugs.py).

Zero-dependency CLI over greppable Markdown cards in docs/kanban/cards/. Each card is one file
(`docs/kanban/cards/NNN-slug.md`) with a small frontmatter block; the board view
`docs/kanban/board.md` is regenerated from the cards. Columns: backlog | todo | doing | done.

  tools/kanban.py add "title"  [--body TEXT] [--label L]... [--col todo] [--evidence PATH]...
  tools/kanban.py list        [--col COL] [--label L]
  tools/kanban.py show  <id>
  tools/kanban.py move  <id> <col>            # backlog|todo|doing|done
  tools/kanban.py label <id> <+label|-label>...
  tools/kanban.py note  <id> "text"           # append a dated note to the card body
  tools/kanban.py evidence <id> <path>...      # attach evidence image path(s)
  tools/kanban.py search <words...>            # grep titles+bodies
  tools/kanban.py board                        # regenerate docs/kanban/board.md
  tools/kanban.py rm <id>

Evidence images live in docs/reference/issues/ (committed) — a card just records their paths.
"""
import sys, os, re, glob, argparse, datetime, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CARDS = ROOT / "docs" / "kanban" / "cards"
BOARD = ROOT / "docs" / "kanban" / "board.md"
COLS  = ["backlog", "todo", "doing", "done"]

def today(): return datetime.date.today().isoformat()  # date only; fine for a local tracker

def slug(s):
    s = re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")
    return s[:48] or "card"

def parse_card(path):
    txt = path.read_text()
    m = re.match(r"^---\n(.*?)\n---\n(.*)$", txt, re.S)
    fm, body = ({}, txt)
    if m:
        body = m.group(2)
        for line in m.group(1).splitlines():
            if ":" in line:
                k, v = line.split(":", 1); fm[k.strip()] = v.strip()
    fm["_path"] = path; fm["_body"] = body
    fm["labels"] = [x for x in re.findall(r"[\w-]+", fm.get("labels", "")) ]
    fm["evidence"] = [x for x in fm.get("evidence", "").split(",") if x.strip()]
    fm["id"] = int(fm.get("id", 0))
    return fm

def all_cards():
    return sorted((parse_card(pathlib.Path(p)) for p in glob.glob(str(CARDS / "*.md"))),
                  key=lambda c: c["id"])

def write_card(c):
    fm = ["---", f"id: {c['id']}", f"title: {c['title']}", f"status: {c['status']}",
          f"labels: [{', '.join(c['labels'])}]", f"created: {c.get('created', today())}",
          f"updated: {today()}"]
    if c.get("evidence"): fm.append(f"evidence: {', '.join(c['evidence'])}")
    fm += ["---", "", c["_body"].strip(), ""]
    c["_path"].write_text("\n".join(fm))

def next_id():
    cs = all_cards(); return (max((c["id"] for c in cs), default=0)) + 1

def card_by_id(i):
    for c in all_cards():
        if c["id"] == int(i): return c
    sys.exit(f"kanban: no card #{i}")

def path_for(i, title): return CARDS / f"{int(i):03d}-{slug(title)}.md"

def cmd_add(a):
    CARDS.mkdir(parents=True, exist_ok=True)
    i = next_id()
    c = {"id": i, "title": a.title, "status": a.col, "labels": a.label or [],
         "created": today(), "evidence": a.evidence or [], "_body": a.body or "",
         "_path": path_for(i, a.title)}
    if a.col not in COLS: sys.exit(f"kanban: bad column {a.col} (use {COLS})")
    write_card(c); regen_board()
    print(f"added #{i} [{a.col}] {a.title}")

def cmd_list(a):
    for c in all_cards():
        if a.col and c["status"] != a.col: continue
        if a.label and a.label not in c["labels"]: continue
        ev = f" 📎{len(c['evidence'])}" if c["evidence"] else ""
        print(f"  #{c['id']:3d} [{c['status']:7}] {c['title']}  {','.join(c['labels'])}{ev}")

def cmd_show(a):
    c = card_by_id(a.id); print(c["_path"].read_text())

def cmd_move(a):
    if a.col not in COLS: sys.exit(f"kanban: bad column {a.col} (use {COLS})")
    c = card_by_id(a.id); c["status"] = a.col; write_card(c); regen_board()
    print(f"#{c['id']} -> {a.col}")

def cmd_label(a):
    c = card_by_id(a.id)
    for t in a.labels:
        if t.startswith("-"): c["labels"] = [l for l in c["labels"] if l != t[1:]]
        else: c["labels"].append(t.lstrip("+")) if t.lstrip("+") not in c["labels"] else None
    write_card(c); regen_board(); print(f"#{c['id']} labels: {c['labels']}")

def cmd_note(a):
    c = card_by_id(a.id); c["_body"] = c["_body"].rstrip() + f"\n\n**{today()}:** {a.text}\n"
    write_card(c); print(f"#{c['id']} note added")

def cmd_evidence(a):
    c = card_by_id(a.id)
    for p in a.paths:
        if p not in c["evidence"]: c["evidence"].append(p)
    write_card(c); regen_board(); print(f"#{c['id']} evidence: {c['evidence']}")

def cmd_search(a):
    q = [w.lower() for w in a.words]
    for c in all_cards():
        hay = (c["title"] + " " + c["_body"]).lower()
        if all(w in hay for w in q):
            print(f"  #{c['id']:3d} [{c['status']:7}] {c['title']}")

def cmd_rm(a):
    c = card_by_id(a.id); c["_path"].unlink(); regen_board(); print(f"removed #{c['id']}")

def cmd_board(a): regen_board(); print(f"wrote {BOARD}")

def regen_board():
    cs = all_cards()
    out = ["# Kanban board", "",
           "> The project bug/issue tracker. Managed by `tools/kanban.py` (see skill `bug-tracker`).",
           "> Cards live in `docs/kanban/cards/`; evidence images in `docs/reference/issues/`.", ""]
    for col in COLS:
        items = [c for c in cs if c["status"] == col]
        out.append(f"## {col.upper()} ({len(items)})")
        for c in items:
            ev = f" — 📎 {', '.join(c['evidence'])}" if c["evidence"] else ""
            out.append(f"- **#{c['id']} {c['title']}**  `{','.join(c['labels'])}`{ev}")
        out.append("")
    BOARD.parent.mkdir(parents=True, exist_ok=True)
    BOARD.write_text("\n".join(out))

def main():
    p = argparse.ArgumentParser(prog="kanban.py"); sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("add"); s.add_argument("title"); s.add_argument("--body", default="")
    s.add_argument("--label", action="append"); s.add_argument("--col", default="todo")
    s.add_argument("--evidence", action="append"); s.set_defaults(fn=cmd_add)
    s = sub.add_parser("list"); s.add_argument("--col"); s.add_argument("--label"); s.set_defaults(fn=cmd_list)
    s = sub.add_parser("show"); s.add_argument("id"); s.set_defaults(fn=cmd_show)
    s = sub.add_parser("move"); s.add_argument("id"); s.add_argument("col"); s.set_defaults(fn=cmd_move)
    s = sub.add_parser("label"); s.add_argument("id"); s.add_argument("labels", nargs="+"); s.set_defaults(fn=cmd_label)
    s = sub.add_parser("note"); s.add_argument("id"); s.add_argument("text"); s.set_defaults(fn=cmd_note)
    s = sub.add_parser("evidence"); s.add_argument("id"); s.add_argument("paths", nargs="+"); s.set_defaults(fn=cmd_evidence)
    s = sub.add_parser("search"); s.add_argument("words", nargs="+"); s.set_defaults(fn=cmd_search)
    s = sub.add_parser("board"); s.set_defaults(fn=cmd_board)
    s = sub.add_parser("rm"); s.add_argument("id"); s.set_defaults(fn=cmd_rm)
    a = p.parse_args(); a.fn(a)

if __name__ == "__main__":
    main()
