#!/usr/bin/env python3
"""wide60 offline proof-of-concept: convert a 30fps GPU dump into a 60fps dump by
synthesizing an interpolated frame between each pair of presented frames.

For each frame pair (A, B):
  emit [B's non-draw packets (state/uploads) + B's draws with vertices lerped toward
  A (t=0.5, in absolute coords: GP0 E5 draw offsets differ between buffers) + flip to
  B's display start + 1 vsync], then [B's original packets + 1 vsync].
Matched draws lerp; B-only draws appear at their final position (snap); A-only draws
drop. The result replays through DuckStation's real renderer (regtest <file>.psxgpu),
so visual quality can be judged before any emulator render-loop work.

Matching (validated 99.2% correct against arena-slot ground truth on Crash Bash):
- Requires SRCADDR sideband comments in the dump (our fork records the DMA linked-list
  RAM address of every GP0 packet). Engines bump-allocate per-object prims from a
  double-buffered arena, so address order == entity iteration order, and the same slot
  two frames apart is the same prim (used as ground truth in tools' validation).
- Adjacent frames use different arenas, so draws are sequence-aligned (difflib) on
  position-independent fingerprints: command byte + texcoord low halves (mesh-stable).
  CLUT/texpage high halves are excluded — games double-buffer CLUTs with frame parity.
- A displacement gate (default 100px max per-vertex L1) snaps instead of lerping the
  rare residual mismatch; a wrong pair lerped halfway is a screen-crossing polygon.

NOTE (2026-06-12): this parser only inspects words[0] of each GP0 packet, so E5 draw
offsets bundled mid-packet are never seen. Games DO alternate E5 per double-buffer;
this tool compares raw (= buffer-relative) coords and works by accident. The real-time
implementation (fork gpu_wide60.cpp) gates in buffer-relative space explicitly.

Usage: interp_dump.py <in.psxgpu> <out.psxgpu> [gate_px]
"""

import difflib
import struct
import sys

MAGIC = b"PSXGPUDUMPv1\0\0"


def sext11(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def sextN(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Draw:
    """A GP0 draw command with positions of its vertex-xy words."""

    def __init__(self, words, offset, src_addr):
        self.words = list(words)
        self.offset = offset
        self.src_addr = src_addr
        self.lerp_from = None  # abs verts of the matched previous-frame draw
        cmd = words[0]
        self.cmd_byte = cmd >> 24
        op = cmd >> 29
        self.xy_indices = []
        uv_lo = ()

        if op == 1:  # polygon
            gouraud = bool(cmd & (1 << 28))
            quad = bool(cmd & (1 << 27))
            textured = bool(cmd & (1 << 26))
            nverts = 4 if quad else 3
            i = 0
            for v in range(nverts):
                if v == 0 or gouraud:
                    i += 1  # color word
                self.xy_indices.append(i)
                i += 1
                if textured:
                    # texcoord low halves are mesh-stable; high halves (clut/texpage)
                    # alternate with frame parity in CLUT-double-buffering games
                    uv_lo += (self.words[i] & 0xFFFF,)
                    i += 1
        elif op == 3:  # rectangle
            textured = bool(cmd & (1 << 26))
            self.xy_indices = [1]
            if textured:
                uv_lo = (self.words[2] & 0xFFFF,)
        elif op == 2:  # line
            gouraud = bool(cmd & (1 << 28))
            poly = bool(cmd & (1 << 27))
            if poly:
                self.xy_indices = []  # polyline: not lerped
            else:
                self.xy_indices = [1, 3] if gouraud else [1, 2]

        self.token = (self.cmd_byte, len(self.words)) + uv_lo

        # absolute vertex positions (draw-offset applied)
        self.abs_verts = []
        for i in self.xy_indices:
            xy = self.words[i]
            self.abs_verts.append((sext11(xy) + offset[0], sext11(xy >> 16) + offset[1]))

    def lerped_packet(self, other_abs):
        """Rebuild the GP0 packet with vertices at midpoint(self, other), relative
        to this draw's own E5 offset (its stream position is preserved, so the same
        offset state applies on replay). other_abs = matched prev-frame abs verts."""
        words = list(self.words)
        for idx, i in enumerate(self.xy_indices):
            bx, by = self.abs_verts[idx]
            ax, ay = other_abs[idx]
            mx = (ax + bx + (1 if bx > ax else 0)) // 2 - self.offset[0]
            my = (ay + by + (1 if by > ay else 0)) // 2 - self.offset[1]
            words[i] = (mx & 0xFFFF) | ((my & 0xFFFF) << 16)
        return words


def packet_bytes(ptype, words):
    hdr = (len(words) & 0xFFFFFF) | (ptype << 24)
    return struct.pack(f"<{len(words)+1}I", hdr, *words)


def main():
    data = open(sys.argv[1], "rb").read()
    if not data.startswith(MAGIC):
        sys.exit("bad magic")

    pos = len(MAGIC)
    prefix_end = None
    frames = []  # each: list of (ptype, words, draw_or_none)
    cur = []
    offset = (0, 0)
    src_addr = None
    last_display_start = None

    while pos + 4 <= len(data):
        (hdr,) = struct.unpack_from("<I", data, pos)
        lw = hdr & 0xFFFFFF
        ptype = hdr >> 24
        words = list(struct.unpack_from(f"<{lw}I", data, pos + 4))
        raw = data[pos + 4 : pos + 4 + lw * 4]
        pos += 4 + lw * 4

        if prefix_end is None:
            if ptype == 0x05:  # TraceBegin: everything up to here is the preamble
                prefix_end = pos
            continue

        if ptype == 0x12:
            s = raw.split(b"\0")[0]
            if s.startswith(b"SRCADDR"):
                src_addr = int(s.split()[1], 16)
            continue  # sideband: don't propagate to output

        draw = None
        if ptype == 0x00 and lw > 0:
            top = words[0] >> 24
            if top == 0xE5:
                offset = (sextN(words[0], 11), sextN(words[0] >> 11, 11))
            if (words[0] >> 29) in (1, 2, 3):
                draw = Draw(words, offset, src_addr)
        cur.append((ptype, words, draw))
        if ptype == 0x01 and lw >= 1 and (words[0] >> 24) == 0x05:
            start = words[0] & 0x7FFFF
            if start != last_display_start:
                last_display_start = start
                frames.append({"packets": cur, "display_start": words[0], "offset": offset})
                cur = []

    gate = int(sys.argv[3]) if len(sys.argv) > 3 else 48
    out = bytearray(data[:prefix_end])
    vsync_words = next(w for f in frames for (t, w, _) in f["packets"] if t == 0x02)

    def emit_frame_packets(packets, n_vsyncs):
        for ptype, words, _ in packets:
            if ptype == 0x02:
                continue
            out.extend(packet_bytes(ptype, words))
        for _ in range(n_vsyncs):
            out.extend(packet_bytes(0x02, vsync_words))

    # frame 0 verbatim (1 vsync; its pair partner doesn't exist)
    emit_frame_packets(frames[0]["packets"], 1)

    total_draws = matched_draws = 0
    for fi in range(1, len(frames)):
        a, b = frames[fi - 1], frames[fi]

        # match A->B: sort draws by arena address (= allocation/entity-iteration
        # order), sequence-align on mesh fingerprints
        a_draws = sorted((d for _, _, d in a["packets"] if d and d.xy_indices),
                         key=lambda d: d.src_addr or 0)
        b_draws = sorted((d for _, _, d in b["packets"] if d and d.xy_indices),
                         key=lambda d: d.src_addr or 0)
        sm = difflib.SequenceMatcher(None, [d.token for d in a_draws],
                                     [d.token for d in b_draws], autojunk=False)
        for ai, bi, n in sm.get_matching_blocks():
            for k in range(n):
                prev, curd = a_draws[ai + k], b_draws[bi + k]
                disp = max(abs(ax - bx) + abs(ay - by)
                           for (ax, ay), (bx, by) in zip(prev.abs_verts, curd.abs_verts))
                if disp <= gate:
                    curd.lerp_from = prev.abs_verts

        # interpolated frame: B's stream with matched draws lerped toward A
        for ptype, words, d in b["packets"]:
            if ptype == 0x02:
                continue
            if d and d.xy_indices:
                total_draws += 1
                if d.lerp_from is not None:
                    matched_draws += 1
                    words = d.lerped_packet(d.lerp_from)
            if ptype == 0x01 and (words[0] >> 24) == 0x05:
                continue  # drop B's flip from the interp pass; we emit our own
            out.extend(packet_bytes(ptype, words))
        out.extend(packet_bytes(0x01, [b["display_start"]]))
        out.extend(packet_bytes(0x02, vsync_words))

        # the real frame B
        emit_frame_packets(b["packets"], 1)

    open(sys.argv[2], "wb").write(out)
    print(f"{len(frames)} input frames -> {2*len(frames)-1} output frames; "
          f"lerped {matched_draws}/{total_draws} draws "
          f"({100*matched_draws/max(total_draws,1):.1f}%)")


if __name__ == "__main__":
    main()
