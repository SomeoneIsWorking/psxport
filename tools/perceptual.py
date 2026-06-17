#!/usr/bin/env python3
# perceptual.py — "does it LOOK different" comparison for SW-vs-VK frames, lenient to pixel drift.
#
# A pixel-exact diff over-reports during fades/dither (every pixel off by +/-1 from rounding inflates
# the count even when the frames look identical). This metric downscales both images by 4x first — a
# box/bilinear average that washes out ordered dither and 1px subpixel edge drift — then measures:
#   mae:   mean absolute RGB difference, normalized 0..1 (overall "how different it looks")
#   gross: fraction of downscaled cells whose max channel delta exceeds GROSS (a real, visible block
#          — missing water, a fade/brightness flash, a dropped object — survives the downscale)
# Flag a frame when mae>=MAE_THR or gross>=GROSS_THR. Used by tools/swvk_mon.py and tools/swvk_diff.py.
import numpy as np
from PIL import Image

VW, VH = 1024, 512
DOWN = (80, 60)     # /4 of 320x240 — averages out dither + subpixel edge drift
GROSS = 40          # per-channel delta (0..255) that counts as a visible block in the downscaled image
MAE_THR = 0.030     # mean abs diff (0..1) — overall look divergence
GROSS_THR = 0.010   # fraction of downscaled cells visibly different

def vram_to_rgb(raw, y0):
    """raw = bytes of a 1024x512 u16 VRAM; return the 240x320x3 uint8 display buffer at (0,y0)."""
    v = np.frombuffer(raw, dtype="<u2").reshape(VH, VW)[y0:y0+240, 0:320]
    r = ((v & 31) << 3).astype(np.uint8)
    g = (((v >> 5) & 31) << 3).astype(np.uint8)
    b = (((v >> 10) & 31) << 3).astype(np.uint8)
    return np.dstack([r, g, b])

def _small(rgb):
    return np.asarray(Image.fromarray(rgb).resize(DOWN, Image.BILINEAR), dtype=np.float32)

def pdiff(rgb_a, rgb_b):
    """Return (mae 0..1, gross 0..1) — perceptual difference between two 240x320x3 uint8 images."""
    A, B = _small(rgb_a), _small(rgb_b)
    d = np.abs(A - B)
    mae = float(d.mean()) / 255.0
    gross = float((d.max(axis=2) > GROSS).mean())
    return mae, gross

def flagged(mae, gross):
    return mae >= MAE_THR or gross >= GROSS_THR

def live_buffer(sw_raw):
    """Pick the display buffer the SW replay actually drew into (most non-black): 0 or 256."""
    v = np.frombuffer(sw_raw, dtype="<u2").reshape(VH, VW)
    nz0 = int(np.count_nonzero(v[0:240, 0:320] & 0x7FFF))
    nz256 = int(np.count_nonzero(v[256:496, 0:320] & 0x7FFF))
    return 0 if nz0 >= nz256 else 256

if __name__ == "__main__":
    import sys
    vk, sw = sys.argv[1], sys.argv[2]
    a, b = open(vk, "rb").read(), open(sw, "rb").read()
    y0 = int(sys.argv[3]) if len(sys.argv) > 3 else live_buffer(b)
    mae, gross = pdiff(vram_to_rgb(a, y0), vram_to_rgb(b, y0))
    print(f"buf{y0}  mae={mae:.4f}  gross={gross:.4f}  {'FLAG' if flagged(mae,gross) else 'ok'}")
