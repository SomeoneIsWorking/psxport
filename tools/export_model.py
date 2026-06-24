#!/usr/bin/env python3
"""Offline Tomba!2 3D MODEL + TEXTURE exporter (no renderer needed).

Parses the game's GEOMBLK 3D-model format straight out of a RAM dump, and decodes each
prim's texture from a VRAM dump, writing a standard OBJ + MTL + PNG set you can open in
any 3D viewer (Blender, MeshLab, https://3dviewer.net). This is the verification artifact
for the asset subsystem RE: if the exported model looks right, the format is understood.

GEOMBLK format (RE'd from engine/engine_submit.cpp native GT3/GT4 submitters):
  header @geomblk+0: u32 counts -> low16 = #GT3 (textured tris), high16 = #GT4 (textured quads)
  records @geomblk+16: nGT3 GT3 records (36B) then nGT4 GT4 records (44B), model-space verts.
  GT3 (36B): +0 rgb0|op  +4 rgb1  +8 uv0|clut  +12 uv1|tpage  +16 XY0  +20 Z0|Z1
             +24 XY1  +28 XY2  +32 Z2(lo)|uv2(hi)
  GT4 (44B): +0 rgb0  +4 rgb2  +8 uv0|clut  +12 uv1|tpage  +16 uv2(lo)|uv3(hi)
             +20 XY0  +24 Z0|Z1  +28 XY1  +32 XY2  +36 Z2|Z3  +40 XY3
  XY = packed s16 x (low) | s16 y (high); Z = s16. tpage/clut = PSX texpage/CLUT ids.

Usage:
  tools/export_model.py <ram.bin> <vram.raw> <out_dir> <geomblk_hex> [geomblk_hex ...]
"""
import sys, os, struct

def s16(v): return v - 0x10000 if v & 0x8000 else v

class Mem:
    def __init__(self, path):
        with open(path, 'rb') as f: self.d = f.read()
    def off(self, a): return a & 0x1FFFFF          # KSEG -> 2MB RAM offset
    def u32(self, a): return struct.unpack_from('<I', self.d, self.off(a))[0]
    def u16(self, a): return struct.unpack_from('<H', self.d, self.off(a))[0]

class Vram:
    """1024x512 u16 PSX VRAM."""
    def __init__(self, path):
        with open(path, 'rb') as f: self.d = f.read()
        self.W = 1024
    def px(self, x, y):
        return struct.unpack_from('<H', self.d, (y * self.W + x) * 2)[0]

def bgr555_to_rgb(p):
    r = (p & 0x1F) << 3; g = ((p >> 5) & 0x1F) << 3; b = ((p >> 10) & 0x1F) << 3
    a = 0 if p == 0 else 255                          # PSX: 0x0000 = transparent
    return (r | r >> 5, g | g >> 5, b | b >> 5, a)

def decode_page(vram, tp, clut, w=256, h=256):
    """Decode a 256x256 texel window of the texpage `tp` through `clut` -> RGBA bytes."""
    tp_x = (tp & 0xF) * 64; tp_y = ((tp >> 4) & 1) * 256
    mode = (tp >> 7) & 3                              # 0=4bpp,1=8bpp,2/3=15bpp
    cx = (clut & 0x3F) * 16; cy = (clut >> 6) & 0x1FF
    out = bytearray(w * h * 4)
    for v in range(h):
        for u in range(w):
            if mode == 0:                            # 4bpp: 4 texels / u16
                word = vram.px(tp_x + (u >> 2), tp_y + v)
                idx = (word >> ((u & 3) * 4)) & 0xF
                rgba = bgr555_to_rgb(vram.px(cx + idx, cy))
            elif mode == 1:                          # 8bpp: 2 texels / u16
                word = vram.px(tp_x + (u >> 1), tp_y + v)
                idx = (word >> ((u & 1) * 8)) & 0xFF
                rgba = bgr555_to_rgb(vram.px(cx + idx, cy))
            else:                                    # 15bpp direct
                rgba = bgr555_to_rgb(vram.px(tp_x + u, tp_y + v))
            o = (v * w + u) * 4
            out[o:o+4] = bytes(rgba)
    return out

def write_png(path, w, h, rgba):
    import zlib
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y*w*4:(y+1)*w*4]
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 9)))
        f.write(chunk(b'IEND', b''))

def parse_geomblk(mem, addr):
    """Return list of prims: dict(verts=[(x,y,z)..], uvs=[(u,v)..], rgb=[(r,g,b)..], tp, clut)."""
    counts = mem.u32(addr)
    n3, n4 = counts & 0xFFFF, (counts >> 16) & 0xFFFF
    prims = []
    rec = addr + 16
    for _ in range(n3):
        xy0, xy1, xy2 = mem.u32(rec+16), mem.u32(rec+24), mem.u32(rec+28)
        vz01 = mem.u32(rec+20); z2 = mem.u16(rec+32)
        verts = [(s16(xy0&0xFFFF), s16(xy0>>16), s16(vz01&0xFFFF)),
                 (s16(xy1&0xFFFF), s16(xy1>>16), s16(vz01>>16)),
                 (s16(xy2&0xFFFF), s16(xy2>>16), s16(z2))]
        uv0, uv1 = mem.u32(rec+8), mem.u32(rec+12); uv2 = mem.u16(rec+34)
        uvs = [(uv0&0xFF,(uv0>>8)&0xFF),(uv1&0xFF,(uv1>>8)&0xFF),(uv2&0xFF,(uv2>>8)&0xFF)]
        prims.append(dict(verts=verts, uvs=uvs, tp=uv1>>16, clut=uv0>>16))
        rec += 36
    for _ in range(n4):
        xy0, xy1, xy2, xy3 = mem.u32(rec+20), mem.u32(rec+28), mem.u32(rec+32), mem.u32(rec+40)
        vz01, vz23 = mem.u32(rec+24), mem.u32(rec+36)
        verts = [(s16(xy0&0xFFFF), s16(xy0>>16), s16(vz01&0xFFFF)),
                 (s16(xy1&0xFFFF), s16(xy1>>16), s16(vz01>>16)),
                 (s16(xy2&0xFFFF), s16(xy2>>16), s16(vz23&0xFFFF)),
                 (s16(xy3&0xFFFF), s16(xy3>>16), s16(vz23>>16))]
        uv0, uv1, uv23 = mem.u32(rec+8), mem.u32(rec+12), mem.u32(rec+16)
        uvs = [(uv0&0xFF,(uv0>>8)&0xFF),(uv1&0xFF,(uv1>>8)&0xFF),
               (uv23&0xFF,(uv23>>8)&0xFF),((uv23>>16)&0xFF,(uv23>>24)&0xFF)]
        prims.append(dict(verts=verts, uvs=uvs, tp=uv1>>16, clut=uv0>>16))
        rec += 44
    return prims, n3, n4

def parse_terrain(mem, addr):
    """Byte-packed GT4 terrain records (RE'd from engine/native_terrain.cpp). 36B stride from `addr`;
    X/Y are s8<<8 at +1C/1D/20/21 & +1E/1F/22/23; Z is the top byte of the RGB words (+0F/13/17/1B)<<8;
    loop continues while ctl=(s32)u32(+4) > 0 (last record has ctl<=0). uv0@+0, uv1 in ctl, uv2@+8."""
    XO=(0x1C,0x1D,0x20,0x21); YO=(0x1E,0x1F,0x22,0x23); ZO=(0x0F,0x13,0x17,0x1B)
    prims=[]; rec=addr
    for _ in range(20000):
        ctl = mem.u32(rec+4); ctls = ctl - 0x100000000 if ctl & 0x80000000 else ctl
        verts=[]
        for k in range(4):
            vx=(mem.d[mem.off(rec+XO[k])]); vx=(vx-256 if vx&0x80 else vx)<<8
            vy=(mem.d[mem.off(rec+YO[k])]); vy=(vy-256 if vy&0x80 else vy)<<8
            vz=(mem.d[mem.off(rec+ZO[k])]); vz=(vz-256 if vz&0x80 else vz)<<8
            verts.append((vx,vy,vz))
        uv0=mem.u32(rec+0); uv2=mem.u32(rec+8)
        uvs=[(uv0&0xFF,(uv0>>8)&0xFF),(ctl&0xFF,(ctl>>8)&0xFF),
             (uv2&0xFF,(uv2>>8)&0xFF),((uv2>>16)&0xFF,(uv2>>24)&0xFF)]
        prims.append(dict(verts=verts, uvs=uvs, tp=(ctl&0x7FFFFF)>>16, clut=uv0>>16))
        rec += 36
        if ctls <= 0: break
    return prims, 0, len(prims)

def main():
    if len(sys.argv) < 5:
        print(__doc__); sys.exit(1)
    ram, vram, out = Mem(sys.argv[1]), Vram(sys.argv[2]), sys.argv[3]
    os.makedirs(out, exist_ok=True)
    # args: geomblk hex addresses, or "node:HEX" to export a whole OBJECT (all its render-cmd geomblks:
    # cmd-ptr array @node+0xC0, count @node+8, geomblk @cmd+0x40 — RE'd in the `ents` REPL tool).
    addrs = []
    terrain_addr = None
    for a in sys.argv[4:]:
        if a.startswith('terrain'):
            terrain_addr = int(a.split(':')[1], 16) if ':' in a else 0x8009FAE8
        elif a.startswith('node:'):
            node = int(a[5:], 16); ncmd = ram.d[ram.off(node + 8)]
            for i in range(ncmd):
                cmd = ram.u32(node + 0xC0 + i * 4)
                if cmd: addrs.append(ram.u32(cmd + 0x40))
        else:
            addrs.append(int(a, 16))
    addrs = [a for a in addrs if 0x80000000 <= a < 0x80200000]   # valid RAM geomblks only
    jobs = [('geomblk', a) for a in addrs]
    if terrain_addr is not None: jobs.append(('terrain', terrain_addr))
    if not jobs: print("no valid geomblk/terrain addresses"); sys.exit(1)
    name = "terrain_%08x" % terrain_addr if (terrain_addr is not None and not addrs) else "model_%08x" % addrs[0]
    obj = open(os.path.join(out, name + ".obj"), 'w')
    mtl = open(os.path.join(out, name + ".mtl"), 'w')
    obj.write("mtllib %s.mtl\n" % name)
    vbase = 1; mats = {}
    for kind, addr in jobs:
        prims, n3, n4 = (parse_terrain if kind == 'terrain' else parse_geomblk)(ram, addr)
        print("%s %08X: %d tris + %d quads = %d prims" % (kind, addr, n3, n4, len(prims)))
        for pr in prims:
            key = (pr['tp'], pr['clut'])
            if key not in mats:
                mid = "tp%04x_cl%04x" % key
                mats[key] = mid
                rgba = decode_page(vram, pr['tp'], pr['clut'])
                write_png(os.path.join(out, mid + ".png"), 256, 256, rgba)
                mtl.write("newmtl %s\nmap_Kd %s.png\nd 1\n" % (mid, mid))
            obj.write("usemtl %s\n" % mats[key])
            for (x, y, z) in pr['verts']:
                obj.write("v %d %d %d\n" % (x, -y, -z))        # flip Y/Z for OBJ viewers
            for (u, v) in pr['uvs']:
                obj.write("vt %.5f %.5f\n" % (u / 256.0, 1.0 - v / 256.0))
            nv = len(pr['verts'])
            idx = list(range(vbase, vbase + nv))
            if nv == 3:
                obj.write("f %d/%d %d/%d %d/%d\n" % (idx[0],idx[0],idx[1],idx[1],idx[2],idx[2]))
            else:  # PSX quad vertex order is 0,1,2,3 = TL,TR,BL,BR -> faces (0,1,3),(0,3,2)
                obj.write("f %d/%d %d/%d %d/%d\n" % (idx[0],idx[0],idx[1],idx[1],idx[3],idx[3]))
                obj.write("f %d/%d %d/%d %d/%d\n" % (idx[0],idx[0],idx[3],idx[3],idx[2],idx[2]))
            vbase += nv
    obj.close(); mtl.close()
    print("wrote %s.obj (+ .mtl, %d textures) -> %s" % (name, len(mats), out))

if __name__ == '__main__':
    main()
