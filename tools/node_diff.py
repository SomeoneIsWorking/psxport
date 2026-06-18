#!/usr/bin/env python3
# node_diff.py — entity-aware gameplay-vs-render diff of two 2 MB guest-RAM dumps (PSXPORT_RAMDUMP).
#
# ram_region_diff.py buckets by fixed address ranges and so can't tell a GAMEPLAY-field change from a
# render-derived one that happens to live inside a dynamically-allocated entity node. This walks the live
# entity list (both heads) in dump A and, per node, splits the diff into:
#   - GAMEPLAY fields   node+0x00 .. +0x98  (type/state/handler/prev/next/pos/rot/model/... — the logic)
#   - RENDER cache      node+0x98 .. +0xD0  (the rotation matrix +0x98.., translation, cmd-ptr array +0xc0)
# A change confined to the render cache is NOT a gameplay divergence (it's derived from pos/rot, which the
# game would compute anyway when the object is visible). Use to gate "gameplay 0-diff" for the widescreen
# margin (later-134): the +0x98.. cache differs for margin objects we render early, gameplay must not.
#
# Usage: tools/node_diff.py A.bin B.bin     (A is walked for the node list; both must be same scene/frame)
import sys

HEAD1, HEAD2 = 0x800fb168, 0x800f2624
STRIDE       = 0xD0
NEXT         = 0x24
GAMEPLAY_END = 0x98          # node+0..0x98 = gameplay/logic; node+0x98..0xD0 = render-matrix cache
PHYS = lambda a: a & 0x1FFFFF

def rd32(buf, addr):
    o = PHYS(addr); return int.from_bytes(buf[o:o+4], "little")

def walk(buf):
    nodes = []
    seen = set()
    for head_addr in (HEAD1, HEAD2):
        n = rd32(buf, head_addr)
        guard = 0
        while n and guard < 2000 and n not in seen:
            seen.add(n); nodes.append(n)
            n = rd32(buf, n + NEXT); guard += 1
    return nodes

def main():
    if len(sys.argv) != 3:
        print("usage: node_diff.py A.bin B.bin"); sys.exit(2)
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    nodes = walk(a)
    print(f"walked {len(nodes)} entity nodes from A")

    in_nodes = bytearray(len(a))                 # mark bytes that belong to a known node
    gp_nodes, rc_nodes = [], []                  # nodes with gameplay / render-cache diffs
    gp_bytes = rc_bytes = 0
    for nd in nodes:
        gp_d = rc_d = 0
        for off in range(STRIDE):
            o = PHYS(nd + off)
            if o + 1 > len(a) or o + 1 > len(b): continue
            in_nodes[o] = 1
            if a[o] != b[o]:
                if off < GAMEPLAY_END: gp_d += 1
                else:                  rc_d += 1
        if gp_d: gp_nodes.append((nd, gp_d)); gp_bytes += gp_d
        if rc_d: rc_nodes.append((nd, rc_d)); rc_bytes += rc_d

    # diffs OUTSIDE any node (render pools, globals, command structs, etc.)
    n = min(len(a), len(b))
    outside = sum(1 for i in range(n) if a[i] != b[i] and not in_nodes[i])

    print(f"\nGAMEPLAY-field diffs (node+0x00..0x{GAMEPLAY_END:02x}): {gp_bytes} bytes across {len(gp_nodes)} nodes")
    for nd, d in gp_nodes[:40]:
        t = a[PHYS(nd+0xc)]
        print(f"  node {nd:08x} type={t:02x}  {d} gameplay bytes differ")
    print(f"\nRENDER-cache diffs (node+0x{GAMEPLAY_END:02x}..0x{STRIDE:02x}): {rc_bytes} bytes across {len(rc_nodes)} nodes (benign)")
    print(f"diffs outside any known node (render pools / cmd structs / globals): {outside} bytes")
    print(f"\nVERDICT: gameplay {'0-DIFF (clean)' if gp_bytes == 0 else 'DIVERGED — ' + str(gp_bytes) + ' B'}")

if __name__ == "__main__":
    main()
