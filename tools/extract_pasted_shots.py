#!/usr/bin/env python3
"""Extract images the user pasted into the current Claude Code session transcript
to docs/reference/issues/, so screenshots survive a session respawn (pasted images
live ONLY in the .jsonl transcript, not on disk, and a fresh context loses them).

Usage:
  tools/extract_pasted_shots.py [--prefix NAME] [--out DIR] [JSONL]

With no JSONL, picks the most-recently-modified transcript for THIS project under
<local-notes>/projects/<slug>/. Writes shot_NN.<ext>; pass --prefix to name them.
"""
import json, base64, os, sys, glob, argparse

def find_transcript():
    base = os.path.expanduser("<local-notes>/projects")
    # project slug is the cwd with / -> -
    slug = os.path.abspath(os.getcwd()).replace("/", "-")
    d = os.path.join(base, slug)
    cands = glob.glob(os.path.join(d, "*.jsonl"))
    if not cands:
        sys.exit(f"no transcript .jsonl under {d}")
    return max(cands, key=os.path.getmtime)

def collect(jsonl):
    imgs = []
    def walk(o):
        if isinstance(o, dict):
            if o.get("type") == "image":
                s = o.get("source", {})
                if s.get("type") == "base64" and s.get("data"):
                    imgs.append((s.get("media_type", "image/png"), s["data"]))
            for v in o.values(): walk(v)
        elif isinstance(o, list):
            for v in o: walk(v)
    with open(jsonl) as f:
        for line in f:
            try: walk(json.loads(line))
            except Exception: pass
    return imgs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl", nargs="?")
    ap.add_argument("--prefix", default="shot")
    ap.add_argument("--out", default="docs/reference/issues")
    a = ap.parse_args()
    jsonl = a.jsonl or find_transcript()
    imgs = collect(jsonl)
    os.makedirs(a.out, exist_ok=True)
    ext = {"image/png": "png", "image/jpeg": "jpg", "image/webp": "webp"}
    print(f"{jsonl}: {len(imgs)} image(s)")
    for i, (mt, d) in enumerate(imgs):
        p = os.path.join(a.out, f"{a.prefix}_{i:02d}.{ext.get(mt,'png')}")
        with open(p, "wb") as o: o.write(base64.b64decode(d))
        print(p, os.path.getsize(p), "bytes")

if __name__ == "__main__":
    main()
