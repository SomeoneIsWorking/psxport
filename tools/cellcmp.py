import numpy as np
def load(p): return np.fromfile(p,dtype='<u2').reshape(512,1024)
a=load("scratch/raw/ours_vram_3360.bin"); b=load("scratch/raw/oracle_vram_7000.bin")
# nonzero (15-bit color, ignore mask bit 0x8000)
an=(a&0x7fff)!=0; bn=(b&0x7fff)!=0
print("cells where OURS is mostly-black but ORACLE is mostly-textured (64x64 cells, x>=320):")
for cy in range(0,512,64):
  for cx in range(320,1024,64):
    ao=an[cy:cy+64,cx:cx+64].mean(); bo=bn[cy:cy+64,cx:cx+64].mean()
    if ao<0.1 and bo>0.5:
      print("  cell (%d,%d): ours nonzero=%.0f%% oracle nonzero=%.0f%%"%(cx,cy,100*ao,100*bo))
print("---")
print("overall nonzero ours=%.1f%% oracle=%.1f%% (x>=320)"%(100*an[:,320:].mean(),100*bn[:,320:].mean()))
