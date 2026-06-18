#!/usr/bin/env python3
# Walk Tomba!2's two active-entity lists in a 2 MB guest-RAM dump (PSXPORT_RAMDUMP) and report, per
# entity TYPE (+0xc), the set of handler fn-pointers (+0x1c) and node positions. Purpose: classify
# entity types as static-world vs dynamic for the native widescreen static-margin render (journal
# later-127 / approach B) WITHOUT relying on a one-frame position diff — the handler address per type
# is the thing to disassemble. Node layout: docs/engine_re.md "Object / entity model".
#   heads: DAT_800fb168 (list1), DAT_800f2624 (list2); stride 0xD0; next @ +0x24; prev @ +0x20.
# Usage: tools/entity_walk.py RAM.bin [--nodes]
import sys, collections

BASE = 0x80000000
HEADS = [("list1", 0x800FB168), ("list2", 0x800F2624)]

def main():
    if len(sys.argv) < 2:
        print("usage: entity_walk.py RAM.bin [--nodes]"); sys.exit(2)
    ram = open(sys.argv[1], "rb").read()
    show_nodes = "--nodes" in sys.argv[2:]

    def r8(a):  return ram[a & 0x1FFFFF]
    def r16(a): return r8(a) | (r8(a + 1) << 8)
    def s16(a): v = r16(a); return v - 0x10000 if v & 0x8000 else v
    def r32(a): return r8(a) | (r8(a+1) << 8) | (r8(a+2) << 16) | (r8(a+3) << 24)

    # type -> Counter(handler) ; type -> list of (list, addr, pos)
    by_type_handler = collections.defaultdict(collections.Counter)
    by_type_nodes = collections.defaultdict(list)
    for lname, head_addr in HEADS:
        n = r32(head_addr)
        seen = set()
        count = 0
        while n and (n & 0x1FFFFF) and n not in seen:
            seen.add(n); count += 1
            typ = r8(n + 0x0c)
            hnd = r32(n + 0x1c)
            pos = (s16(n + 0x2e), s16(n + 0x32), s16(n + 0x36))
            by_type_handler[typ][hnd] += 1
            by_type_nodes[typ].append((lname, n, pos))
            n = r32(n + 0x24)
        print(f"# {lname}: head={head_addr:#010x} -> {count} nodes")

    print("\n=== type -> handler(s) ===")
    for typ in sorted(by_type_handler):
        total = sum(by_type_handler[typ].values())
        print(f"type 0x{typ:02x}  ({total} nodes):")
        for hnd, c in by_type_handler[typ].most_common():
            print(f"    handler 0x{hnd:08x}  x{c}")

    if show_nodes:
        print("\n=== nodes (type | node | model+0xe | mdata+0x38 | state+0x28 | pos) ===")
        for typ in sorted(by_type_nodes):
            for lname, n, pos in by_type_nodes[typ]:
                model = r16(n + 0x0e) & 0x3fff
                mdata = r32(n + 0x38)
                state = r16(n + 0x28)
                print(f"  type 0x{typ:02x} node=0x{n:08x} model=0x{model:04x} mdata=0x{mdata:08x} "
                      f"state=0x{state:04x} pos={pos}")

if __name__ == "__main__":
    main()
