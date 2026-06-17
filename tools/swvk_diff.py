#!/usr/bin/env python3
# swvk_diff.py — automated Software-vs-Vulkan render diff for the live port.
#
# Spots EVERY SW/VK rendering difference for the current frame, the same way the puddle bugs were
# found by hand, but in one shot. Requires the port running with PSXPORT_DEBUG_SERVER=1 (and VK, i.e.
# a normal windowed run). Drive the game to a scene, then run this — repeat as you move to sweep scenes.
#
#   tools/swvk_diff.py [prefix] [--port N]
#
# How it works (aligned, same frame):
#   1. `swvkcap <prefix>` on the debug server arms, for ONE target frame, BOTH a GP0-stream capture
#      (<prefix>.gp0, gpu_differ format) and a raw VK-VRAM readback (<prefix>_vk.vram).
#   2. We wait for the VK raw file to land (the game must present that frame).
#   3. replay_ours replays the identical GP0 stream through OUR software rasterizer -> <prefix>_sw.vram.
#   4. diff.py compares VK vs SW over both display buffers; any difference is a VK-vs-SW divergence
#      (SW is the oracle-faithful reference). PPMs (vk/sw/absdiff) are written + converted to PNG.
import socket, sys, os, time, subprocess, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY = os.path.join(ROOT, "scratch/bin/replay_ours")
DIFF   = os.path.join(ROOT, "tools/gpu_differ/diff.py")

def send(port, line):
    s = socket.create_connection(("127.0.0.1", port)); s.sendall((line+"\n").encode())
    buf = b""
    while b"---END---\n" not in buf:
        c = s.recv(4096)
        if not c: break
        buf += c
    s.close()
    return buf.split(b"---END---\n")[0].decode(errors="replace")

def to_png(ppm):
    try:
        from PIL import Image
        png = ppm[:-4] + ".png"; Image.open(ppm).save(png); return png
    except Exception as e:
        return ppm

def main():
    args = [a for a in sys.argv[1:]]
    port = 5959
    if "--port" in args:
        i = args.index("--port"); port = int(args[i+1]); del args[i:i+2]
    prefix = args[0] if args else "scratch/swvk/cap"
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    gp0 = f"{prefix}.gp0"; vk = f"{prefix}_vk.vram"; sw = f"{prefix}_sw.vram"
    for f in (gp0, vk, sw):
        try: os.remove(f)
        except OSError: pass

    print(send(port, f"swvkcap {prefix}").strip())
    # wait for the VK raw VRAM (1024*512*2 bytes) to land
    want = 1024*512*2
    for _ in range(120):
        if os.path.exists(vk) and os.path.getsize(vk) == want and os.path.exists(gp0):
            time.sleep(0.05); break
        time.sleep(0.1)
    else:
        print("timeout waiting for capture (is the game running + advancing frames?)"); return 1

    subprocess.run([REPLAY, gp0, sw], check=True)
    # The frame's draws live in one of the two display buffers; diff both, VK first so ours.ppm=VK.
    worst = None
    for reg, tag in (("0,0,320,240","b0"), ("0,256,320,240","b256")):
        outdir = f"{prefix}_{tag}"
        r = subprocess.run([DIFF, vk, sw, "--region", reg, "--ppm", outdir, "--top", "8"],
                           capture_output=True, text=True)
        line0 = r.stdout.splitlines()[0] if r.stdout else ""
        # parse the differing percentage "(P%)"
        import re
        m = re.search(r"differing:.*?\(([\d.]+)%\)", line0)
        pct = float(m.group(1)) if m else 0.0
        print(f"[{tag} {reg}] {line0}")
        for ppm in ("ours.ppm","beetle.ppm","absdiff.ppm"):   # ours=VK, beetle=SW (arg order above)
            p = os.path.join(outdir, ppm)
            if os.path.exists(p): to_png(p)
        if worst is None or pct > worst[0]: worst = (pct, tag, outdir)
    if worst:
        pct, tag, outdir = worst
        print(f"\nLargest divergence: buffer {tag} = {pct:.3f}% differing")
        print(f"images: {outdir}/ours.png (VK)  {outdir}/beetle.png (SW)  {outdir}/absdiff.png")
    return 0

if __name__ == "__main__":
    sys.exit(main())
