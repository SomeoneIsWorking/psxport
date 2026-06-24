#!/usr/bin/env python3
"""Offline textured preview of an exported OBJ (verification only — NOT the game renderer).

Z-buffered orthographic software rasterizer: parses the OBJ+MTL written by export_model.py,
samples each face's texture, and renders N rotated views to a PNG so the exported geometry +
UVs + textures can be eyeballed without opening a 3D tool.

Usage: tools/preview_model.py <model.obj> <out.png> [size]
"""
import sys, os, struct, math
import numpy as np
from PIL import Image

def load_obj(path):
    V, VT, faces = [], [], []   # faces: (matname, [(vi,vti),..])
    mtl = {}
    cur = None
    d = os.path.dirname(path)
    for ln in open(path):
        t = ln.split()
        if not t: continue
        if t[0] == 'mtllib':
            for ml in open(os.path.join(d, t[1])):
                mt = ml.split()
                if not mt: continue
                if mt[0] == 'newmtl': cm = mt[1]
                elif mt[0] == 'map_Kd': mtl[cm] = os.path.join(d, mt[1])
        elif t[0] == 'v':  V.append((float(t[1]), float(t[2]), float(t[3])))
        elif t[0] == 'vt': VT.append((float(t[1]), float(t[2])))
        elif t[0] == 'usemtl': cur = t[1]
        elif t[0] == 'f':
            fv = [(int(p.split('/')[0]) - 1, int(p.split('/')[1]) - 1) for p in t[1:]]
            faces.append((cur, fv))
    return np.array(V, float), np.array(VT, float), faces, mtl

def render(V, VT, faces, mtl, size, yaw, pitch):
    tex = {m: np.asarray(Image.open(p).convert('RGBA')) for m, p in mtl.items()}
    c = V.mean(0); R = V - c
    cy, sy = math.cos(yaw), math.sin(yaw); cp, sp = math.cos(pitch), math.sin(pitch)
    x = R[:,0]*cy + R[:,2]*sy; z = -R[:,0]*sy + R[:,2]*cy; y = R[:,1]*cp - z*sp; z = R[:,1]*sp + z*cp
    P = np.stack([x, y, z], 1)
    mn, mx = P.min(0), P.max(0); ext = (mx - mn).max() or 1
    s = (size * 0.9) / ext
    sx = ((P[:,0] - (mn[0]+mx[0])/2) * s + size/2)
    syv = (-(P[:,1] - (mn[1]+mx[1])/2) * s + size/2)
    img = np.zeros((size, size, 3), np.uint8)
    zb = np.full((size, size), 1e30)
    def tri(ia, ib, ic, mat):
        a, b, cc = (sx[ia], syv[ia], P[ia,2]), (sx[ib], syv[ib], P[ib,2]), (sx[ic], syv[ic], P[ic,2])
        ta, tb, tc = VT[ia], VT[ib], VT[ic]
        T = tex.get(mat)
        minx, maxx = int(max(0,min(a[0],b[0],cc[0]))), int(min(size-1,max(a[0],b[0],cc[0])))
        miny, maxy = int(max(0,min(a[1],b[1],cc[1]))), int(min(size-1,max(a[1],b[1],cc[1])))
        if maxx<minx or maxy<miny: return
        det = (b[1]-cc[1])*(a[0]-cc[0])+(cc[0]-b[0])*(a[1]-cc[1])
        if abs(det) < 1e-6: return
        ys, xs = np.mgrid[miny:maxy+1, minx:maxx+1]
        l1 = ((b[1]-cc[1])*(xs-cc[0])+(cc[0]-b[0])*(ys-cc[1]))/det
        l2 = ((cc[1]-a[1])*(xs-cc[0])+(a[0]-cc[0])*(ys-cc[1]))/det
        l3 = 1-l1-l2
        m = (l1>=0)&(l2>=0)&(l3>=0)
        if not m.any(): return
        zz = l1*a[2]+l2*b[2]+l3*cc[2]
        sub = zb[miny:maxy+1, minx:maxx+1]
        win = m & (zz < sub)
        if not win.any(): return
        u = (l1*ta[0]+l2*tb[0]+l3*tc[0]); v = (l1*ta[1]+l2*tb[1]+l3*tc[1])
        if T is not None:
            th, tw = T.shape[:2]
            tu = np.clip((u*tw).astype(int), 0, tw-1); tv = np.clip(((1-v)*th).astype(int), 0, th-1)
            col = T[tv, tu]
            alpha = col[...,3] > 0
            win = win & alpha
            if not win.any(): return
            px = col[...,:3]
        else:
            px = np.full((*m.shape,3), 180, np.uint8)
        reg = img[miny:maxy+1, minx:maxx+1]
        reg[win] = px[win]; sub[win] = zz[win]
    for mat, fv in faces:
        for k in range(1, len(fv)-1):
            tri(fv[0][0], fv[k][0], fv[k+1][0], mat)
    return img

def main():
    if len(sys.argv) < 3: print(__doc__); sys.exit(1)
    obj, outp = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    V, VT, faces, mtl = load_obj(obj)
    views = [(0.6,0.4),(2.2,0.4),(3.9,0.4),(0.6,1.4)]
    tiles = [render(V, VT, faces, mtl, size, y, p) for (y,p) in views]
    strip = np.concatenate(tiles, 1)
    Image.fromarray(strip).save(outp)
    print("wrote %s (%d faces, %d views)" % (outp, sum(len(f[1])-2 for f in faces), len(views)))

if __name__ == '__main__':
    main()
