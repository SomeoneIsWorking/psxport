import sys, numpy as np
from PIL import Image
def load(p):
    d=np.fromfile(p,dtype='<u2').reshape(512,1024)
    return d
def to_rgb(d):
    r=((d&31)<<3).astype(np.uint8); g=(((d>>5)&31)<<3).astype(np.uint8); b=(((d>>10)&31)<<3).astype(np.uint8)
    return np.dstack([r,g,b])
a=load(sys.argv[1]); b=load(sys.argv[2])
Image.fromarray(to_rgb(a)).save(sys.argv[1]+".png")
Image.fromarray(to_rgb(b)).save(sys.argv[2]+".png")
# full diff
diff=(a!=b)
print("full VRAM differ: %.2f%% (%d/%d px)"%(100*diff.mean(), diff.sum(), a.size))
# atlas region x>=320 (texpages), exclude framebuffer at x<320 (display) — well display is also somewhere
for (name,x0,y0,x1,y1) in [("atlas_right_top",320,0,1024,256),("atlas_right_bot",320,256,1024,512),
                            ("left",0,0,320,512),("clut_band",1008,191,1024,256)]:
    sub=diff[y0:y1,x0:x1]
    print("  %-16s [%d:%d,%d:%d] differ %.2f%% (%d/%d)"%(name,x0,x1,y0,y1,100*sub.mean(),sub.sum(),sub.size))
