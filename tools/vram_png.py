#!/usr/bin/env python3
# Convert a raw 1024x512 16bpp (PSX 5-5-5) VRAM dump (PSXPORT_VRAMDUMP_AT) to a PNG.
# Optionally overlay a rectangle marking the displayed region so we can see how much
# world geometry the game actually rasterized vs. what the 320-wide window shows.
#   vram_png.py in.bin out.png [disp_x disp_y disp_w disp_h]
import sys, struct, zlib

def png(w, h, rgb):
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y*w*3:(y+1)*w*3]
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))

def main():
    inp, outp = sys.argv[1], sys.argv[2]
    W, H = 1024, 512
    data = open(inp, "rb").read()
    rgb = bytearray(W*H*3)
    for i in range(W*H):
        p = data[i*2] | (data[i*2+1] << 8)
        rgb[i*3]   = (p & 31) << 3
        rgb[i*3+1] = ((p >> 5) & 31) << 3
        rgb[i*3+2] = ((p >> 10) & 31) << 3
    # Optional displayed-region outline (bright magenta).
    if len(sys.argv) >= 7:
        dx, dy, dw, dh = (int(x) for x in sys.argv[3:7])
        def setpx(x, y):
            if 0 <= x < W and 0 <= y < H:
                o = (y*W+x)*3; rgb[o]=255; rgb[o+1]=0; rgb[o+2]=255
        for x in range(dx, dx+dw):
            setpx(x, dy); setpx(x, dy+dh-1)
        for y in range(dy, dy+dh):
            setpx(dx, y); setpx(dx+dw-1, y)
    open(outp, "wb").write(png(W, H, rgb))
    print(f"wrote {outp} ({W}x{H})")

if __name__ == "__main__":
    main()
