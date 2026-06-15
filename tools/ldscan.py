import capstone
data=open("scratch/bin/tomba2/MAIN.EXE","rb").read()
LOAD=0x80010000; HDR=0x800
text=data[HDR:]
import struct
def w(i): return struct.unpack_from("<I", text, i*4)[0]
n=len(text)//4
def ld_target(ins):
    op=ins>>26
    if 0x20<=op<=0x26: return (ins>>16)&31  # lb..lwr
    if op==0x10 and ((ins>>21)&31)==0: return (ins>>16)&31  # mfc0
    if op==0x12 and ((ins>>21)&31) in (0,2): return (ins>>16)&31  # mfc2/cfc2
    return 0
def reads(ins,r):
    if r==0: return False
    op=ins>>26; rs=(ins>>21)&31; rt=(ins>>16)&31; f=ins&63
    if op==0x00:
        if f in (0x00,0x02,0x03): return rt==r
        if f in (0x08,): return rs==r
        if f in (0x09,): return rs==r
        if f in (0x10,0x12): return False
        if f in (0x11,0x13): return rs==r
        return rs==r or rt==r
    if op==0x0F: return False
    if op in (0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E): return rs==r
    if op in (0x20,0x21,0x23,0x24,0x25): return rs==r
    if op in (0x22,0x26): return rs==r or rt==r
    if op in (0x28,0x29,0x2B,0x2A,0x2E): return rs==r or rt==r
    if op in (0x04,0x05): return rs==r or rt==r
    if op in (0x06,0x07,0x01): return rs==r
    if op==0x10: return rs==0x04 and rt==r
    if op==0x12: return rs in (0x04,0x06) and rt==r
    if op in (0x32,0x3A): return rs==r
    return False
haz=0; samples=[]
for i in range(n-1):
    a=w(i); b=w(i+1)
    t=ld_target(a)
    if not t: continue
    # skip lwl/lwr merge pair
    if (a>>26)==0x22 and (b>>26)==0x26 and ((a>>16)&31)==((b>>16)&31): continue
    if (a>>26)==0x26 and (b>>26)==0x22 and ((a>>16)&31)==((b>>16)&31): continue
    if reads(b,t):
        haz+=1
        if len(samples)<25: samples.append((LOAD+i*4,a,b,t))
print("MAIN.EXE genuine load-delay hazards: %d (of %d instr pairs)"%(haz,n-1))
for addr,a,b,t in samples:
    print("  @%08X load r%d (%08X) -> next reads (%08X)"%(addr,t,a,b))
