#!/usr/bin/env python3
"""Frame-precise screen capture + contact sheet for wide60rt.

The core RE pain on psxport was being *screen-blind*: REing intro gates from RAM
diffs without seeing what was actually on screen at frame N. This drives the
runtime's PSXPORT_FRAMEDUMP (exact-frame framebuffer capture), converts each PPM
to PNG, and assembles a labeled contact sheet so a frame range can be eyeballed.

Because it can capture the same frame under different display configs (native /
widescreen / upscaled), it also answers "is widescreen a true wider FOV or just a
horizontal stretch?" — capture one frame native vs wide and compare edges.

Examples:
  tools/frames.py 0 100 200 300 400 500 600 700      # native, contact sheet
  tools/frames.py --range 380:700:20 --tag intro     # every 20th frame 380..700
  tools/frames.py 900 --config wide --config native --tag wstest  # same frame, 2 cfgs

Disc resolution mirrors scripts/run-tomba2.sh: $PSXPORT_TOMBA2_DISC > $PSXPORT_DISC
> .env (either key) > Tomba*2*.chd drop-in.
"""
import argparse, os, re, subprocess, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent

# Display configs: name -> extra env handed to wide60rt. Headless defaults to
# native (the -play gate disables enhancements); PSXPORT_WIDE/_INTERNAL_RES/_NOWIDE
# override that gate, so we can capture any config headless and deterministically.
CONFIGS = {
    "native": {"PSXPORT_NOWIDE": "1", "PSXPORT_INTERNAL_RES": "1x"},
    "wide":   {"PSXPORT_WIDE": "1",   "PSXPORT_INTERNAL_RES": "1x"},
    "wide4x": {"PSXPORT_WIDE": "1",   "PSXPORT_INTERNAL_RES": "4x"},
    "res4x":  {"PSXPORT_NOWIDE": "1", "PSXPORT_INTERNAL_RES": "4x"},
}


def resolve_disc():
    for k in ("PSXPORT_TOMBA2_DISC", "PSXPORT_DISC"):
        v = os.environ.get(k)
        if v and Path(v).is_file():
            return v
    env = REPO / ".env"
    if env.is_file():
        txt = env.read_text()
        for k in ("PSXPORT_TOMBA2_DISC", "PSXPORT_DISC"):
            m = re.search(rf"^{k}=(.*)$", txt, re.M)
            if m and Path(m.group(1).strip()).is_file():
                return m.group(1).strip()
    for f in sorted(REPO.glob("Tomba*2*.chd")):
        return str(f)
    sys.exit("disc not found: set PSXPORT_TOMBA2_DISC/PSXPORT_DISC or drop a CHD in repo root")


def ppm_to_png(ppm, png, scale):
    im = Image.open(ppm)
    if scale != 1:
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    im.save(png)
    return im.size  # native (pre-scale) size is im.size/scale


def contact_sheet(entries, out, cols):
    """entries: list of (label, png_path). Tiled grid with labels."""
    if not entries:
        return
    tiles = [(lab, Image.open(p)) for lab, p in entries]
    cw = max(im.width for _, im in tiles)
    ch = max(im.height for _, im in tiles)
    pad, labh = 6, 18
    cols = min(cols, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    W = cols * (cw + pad) + pad
    H = rows * (ch + labh + pad) + pad
    sheet = Image.new("RGB", (W, H), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", 13)
    except OSError:
        font = ImageFont.load_default()
    for i, (lab, im) in enumerate(tiles):
        r, c = divmod(i, cols)
        x = pad + c * (cw + pad)
        y = pad + r * (ch + labh + pad)
        d.text((x + 2, y), lab, fill=(230, 230, 90), font=font)
        sheet.paste(im, (x, y + labh))
    sheet.save(out)
    print(f"contact sheet: {out}  ({len(tiles)} frames, {cols}x{rows})")


def parse_frames(args):
    fs = set(int(x) for x in args.frames)
    for spec in args.range or []:
        a, b, *step = (int(x) for x in spec.split(":"))
        fs.update(range(a, b + 1, step[0] if step else 1))
    return sorted(fs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", nargs="*", help="frame numbers to capture")
    ap.add_argument("--range", action="append", help="start:end[:step] (inclusive)")
    ap.add_argument("--config", action="append", choices=list(CONFIGS), help="display config(s); default native")
    ap.add_argument("--tag", default="cap", help="subfolder under scratch/screenshots/")
    ap.add_argument("--scale", type=int, default=2, help="nearest-neighbor upscale for visibility (default 2)")
    ap.add_argument("--cols", type=int, default=4, help="contact-sheet columns")
    ap.add_argument("--disc", help="override disc path")
    args = ap.parse_args()

    frames = parse_frames(args)
    if not frames:
        ap.error("give frame numbers and/or --range")
    configs = args.config or ["native"]
    disc = args.disc or resolve_disc()
    outdir = REPO / "scratch" / "screenshots" / args.tag
    outdir.mkdir(parents=True, exist_ok=True)
    rt = REPO / "runtime" / "wide60rt"
    bios = REPO / "bios"
    total = max(frames) + 2

    entries = []
    for cfg in configs:
        ppmdir = outdir / cfg
        ppmdir.mkdir(exist_ok=True)
        fd = ";".join(f"{f}:{ppmdir}/f{f:06d}.ppm" for f in frames)
        env = dict(os.environ, PSXPORT_FRAMEDUMP=fd, **CONFIGS[cfg])
        print(f"[{cfg}] capturing {len(frames)} frames (run to {total})...")
        subprocess.run([str(rt), disc, "-bios", str(bios), "-frames", str(total)],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        for f in frames:
            ppm = ppmdir / f"f{f:06d}.ppm"
            if not ppm.exists():
                print(f"  WARN f{f}: no framebuffer captured", file=sys.stderr)
                continue
            png = ppmdir / f"f{f:06d}.png"
            w, h = ppm_to_png(ppm, png, args.scale)
            lab = f"f{f} [{cfg}] {w//args.scale}x{h//args.scale}" if len(configs) > 1 else f"f{f} {w//args.scale}x{h//args.scale}"
            entries.append((lab, str(png)))

    # Order the sheet by frame (then config) so a single-config run reads as a timeline.
    sheet = outdir / f"sheet_{'_'.join(configs)}.png"
    contact_sheet(entries, str(sheet), args.cols)
    print(f"pngs in: {outdir}")


if __name__ == "__main__":
    main()
