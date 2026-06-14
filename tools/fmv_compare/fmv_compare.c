// FMV decode compare harness — diff our VLC decoder (runtime/recomp/native_fmv.c
// bs_decode_frame) against an INDEPENDENT reference decoder, on a real STR frame
// read off the disc.
//
// Why this is the right "oracle" here: our FMV pipeline already runs the decoded
// run/level code stream through mednafen's MDEC (the project oracle) for IDCT/YCbCr,
// so IDCT/color is oracle-correct by construction. The ONLY code that is ours is the
// STR-demux + BS/MPEG-1 VLC decode. This harness reimplements that VLC independently
// (separate bit reader, a fresh transcription of the MPEG-1 Table B-14 AC VLC as bit
// STRINGS, independent EOB/escape/sign logic) and diffs the emitted MDEC code stream
// word-for-word, stopping at the first divergence and mapping it back to (block,
// coeff). A divergence is the VLC bug; an exact match means the VLC is correct and the
// remaining artifact is elsewhere (lossy source, or downstream).
//
// Build: tools/fmv_compare/build.sh    Run: see that script's usage.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- from the runtime ---------------------------------------------------------------
int disc_open(void);                                   // disc.c
int disc_read_sector(uint32_t lba, uint8_t* out2048);  // disc.c
int disc_read_raw(uint32_t lba, uint8_t* out, uint32_t n); // disc.c (raw 2352B sector)
int bs_decode_frame(const uint8_t* payload, uint32_t payload_size,
                    int width, int height, uint16_t* codes, int max_codes);  // native_fmv.c (OURS)
int mdec_decode_to_rgb555(const uint16_t* codes, int ncodes,
                          int width, int height, uint16_t* pixels);          // native_fmv.c (real MDEC)
void mdec_init(void);                                                        // mdec_beetle.c

// native_fmv.c references gpu/pad (present_rgb555 / fmv_frame_sync); the table test path
// (mdec_decode_to_rgb555) never calls them, but the linker needs the symbols. MDEC itself is
// the REAL mednafen path (mdec_beetle.c + mdec.c), so the synthetic test exercises the oracle.
static void die_stub(const char* w){ fprintf(stderr,"[harness] unexpected call: %s\n",w); abort(); }
void     gpu_gp0(uint32_t w){(void)w;die_stub("gpu_gp0");}
void     gpu_gp1(uint32_t w){(void)w;die_stub("gpu_gp1");}
void     gpu_present(void){die_stub("gpu_present");}
void     gpu_native_init(void){}
void     pad_poll_sdl(void){}
uint16_t pad_buttons(void){return 0xFFFF;}
int      MDFNSS_StateAction(void*a,int b,int c,void*d,const char*e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 1;}

// ====================================================================================
// Independent reference BS/VLC decoder
// ====================================================================================
#define SECTOR_USER 2048u
#define SUBHDR_LEN  32u

// Independent MSB-first bit reader over the BS payload (16-bit LE units, MSB-first).
typedef struct { const uint8_t* d; uint32_t n, bytep; uint32_t buf; int cnt; } RBR;
static void rbr_init(RBR* b, const uint8_t* d, uint32_t n){ b->d=d; b->n=n; b->bytep=0; b->buf=0; b->cnt=0; }
static void rbr_fill(RBR* b){
  while (b->cnt <= 16 && b->bytep + 1 < b->n) {
    uint16_t w = (uint16_t)(b->d[b->bytep] | (b->d[b->bytep+1] << 8));
    b->bytep += 2; b->buf |= (uint32_t)w << (16 - b->cnt); b->cnt += 16;
  }
}
static uint32_t rbr_peek(RBR* b, int k){ rbr_fill(b); if(!k) return 0; return (b->buf >> (32-k)) & ((1u<<k)-1); }
static void rbr_skip(RBR* b, int k){ b->buf <<= k; b->cnt -= k; }
static uint32_t rbr_get(RBR* b, int k){ uint32_t v = rbr_peek(b,k); rbr_skip(b,k); return v; }
static int rbr_eof(RBR* b){ return b->cnt <= 0 && b->bytep + 1 >= b->n; }

// MPEG-1 Table B-14 AC VLC, transcribed as bit STRINGS (run, level). EOB = "10",
// escape = "000001" handled separately. Source: ISO/IEC 11172-2 Table B-14.
typedef struct { const char* bits; int run, level; } RVLC;
static const RVLC R_TAB[] = {
  {"11",0,1},{"011",1,1},{"0100",0,2},{"0101",2,1},{"00101",0,3},{"00111",3,1},{"00110",4,1},
  {"000110",1,2},{"000111",5,1},{"000101",6,1},{"000100",7,1},
  {"0000110",0,4},{"0000100",2,2},{"0000111",8,1},{"0000101",9,1},
  {"00100110",0,5},{"00100001",0,6},{"00100101",1,3},{"00100100",3,2},
  {"00100111",10,1},{"00100011",11,1},{"00100010",12,1},{"00100000",13,1},
  {"0000001010",0,7},{"0000001100",1,4},{"0000001011",2,3},{"0000001111",4,2},
  {"0000001001",5,2},{"0000001110",14,1},{"0000001101",15,1},{"0000001000",16,1},
  {"000000011101",0,8},{"000000011000",0,9},{"000000010011",0,10},{"000000010000",0,11},
  {"000000011011",1,5},{"000000010100",2,4},{"000000011100",3,3},{"000000010010",4,3},
  {"000000011110",6,2},{"000000010101",7,2},{"000000010001",8,2},{"000000011111",17,1},
  {"000000011010",18,1},{"000000011001",19,1},{"000000010111",20,1},{"000000010110",21,1},
  {"0000000011010",0,12},{"0000000011001",0,13},{"0000000011000",0,14},{"0000000010111",0,15},
  {"0000000010110",1,6},{"0000000010101",1,7},{"0000000010100",2,5},{"0000000010011",3,4},
  {"0000000010010",5,3},{"0000000010001",9,2},{"0000000010000",10,2},{"0000000011111",22,1},
  {"0000000011110",23,1},{"0000000011101",24,1},{"0000000011100",25,1},{"0000000011011",26,1},
  {"00000000011111",0,16},{"00000000011110",0,17},{"00000000011101",0,18},{"00000000011100",0,19},
  {"00000000011011",0,20},{"00000000011010",0,21},{"00000000011001",0,22},{"00000000011000",0,23},
  {"00000000010111",0,24},{"00000000010110",0,25},{"00000000010101",0,26},{"00000000010100",0,27},
  {"00000000010011",0,28},{"00000000010010",0,29},{"00000000010001",0,30},{"00000000010000",0,31},
  {"000000000011000",0,32},{"000000000010111",0,33},{"000000000010110",0,34},{"000000000010101",0,35},
  {"000000000010100",0,36},{"000000000010011",0,37},{"000000000010010",0,38},{"000000000010001",0,39},
  {"000000000010000",0,40},{"000000000011111",1,8},{"000000000011110",1,9},{"000000000011101",1,10},
  {"000000000011100",1,11},{"000000000011011",1,12},{"000000000011010",1,13},{"000000000011001",1,14},
  {"0000000000010011",1,15},{"0000000000010010",1,16},{"0000000000010001",1,17},{"0000000000010000",1,18},
  {"0000000000010100",6,3},{"0000000000011010",11,2},{"0000000000011001",12,2},{"0000000000011000",13,2},
  {"0000000000010111",14,2},{"0000000000010110",15,2},{"0000000000010101",16,2},{"0000000000011111",27,1},
  {"0000000000011110",28,1},{"0000000000011101",29,1},{"0000000000011100",30,1},{"0000000000011011",31,1},
};
enum { R_N = (int)(sizeof(R_TAB)/sizeof(R_TAB[0])) };

// Match one AC code from the bitstream independently. ret 0=EOB, 1=(run,level), -1=bad.
static int ref_ac(RBR* b, int* run, int* level) {
  if (rbr_peek(b,2) == 0x2) { rbr_skip(b,2); return 0; }          // EOB "10"
  if (rbr_peek(b,6) == 0x1) {                                     // escape "000001"
    rbr_skip(b,6);
    int r = (int)rbr_get(b,6);
    int l = (int)rbr_get(b,10); if (l & 0x200) l -= 0x400;        // 10-bit signed (PSX)
    *run = r; *level = l; return 1;
  }
  for (int i = 0; i < R_N; i++) {
    int len = (int)strlen(R_TAB[i].bits);
    uint32_t code = 0; for (int k=0;k<len;k++) code = (code<<1) | (R_TAB[i].bits[k]-'0');
    if (rbr_peek(b,len) == code) {
      rbr_skip(b,len);
      int s = (int)rbr_get(b,1);
      *run = R_TAB[i].run; *level = s ? -R_TAB[i].level : R_TAB[i].level;
      return 1;
    }
  }
  return -1;
}

static int ref_decode(const uint8_t* payload, uint32_t paylen, int w, int h,
                      uint16_t* codes, int maxc) {
  if (paylen < 8) return -1;
  int qscale = (payload[4] | (payload[5]<<8)) & 0x3F; if (!qscale) qscale = 1;
  RBR br; rbr_init(&br, payload+8, paylen-8);
  int nblocks = ((w+15)/16) * ((h+15)/16) * 6;
  int out = 0;
  for (int blk = 0; blk < nblocks; blk++) {
    if (rbr_eof(&br)) break;
    int dc = (int)rbr_get(&br,10); if (dc & 0x200) dc -= 0x400;
    if (out >= maxc) return out;
    codes[out++] = (uint16_t)(((qscale & 0x3F) << 10) | (dc & 0x3FF));
    for (;;) {
      int run, level, r = ref_ac(&br, &run, &level);
      if (r == 0) { if (out>=maxc) return out; codes[out++] = 0xFE00; break; }
      if (r < 0)  return (out>0)?out:-2;
      if (out>=maxc) return out;
      codes[out++] = (uint16_t)(((run & 0x3F) << 10) | (level & 0x3FF));
    }
  }
  return out;
}

// ====================================================================================
// STR demux: pull frame `want` (1-based as in the sub-header) BS payload off the disc.
// ====================================================================================
static int demux_frame(uint32_t lba, uint32_t size, int want,
                       uint8_t* payload, uint32_t maxpay, int* w, int* h) {
  uint32_t nsec = (size + SECTOR_USER - 1) / SECTOR_USER;
  uint8_t sec[SECTOR_USER];
  int cur = -1; uint32_t paylen = 0; int got = 0, expect = 0;
  for (uint32_t s = 0; s < nsec; s++) {
    if (!disc_read_sector(lba + s, sec)) break;
    if ((uint16_t)(sec[0] | (sec[1]<<8)) != 0x0160) continue;     // not a video sector
    int ci = sec[4] | (sec[5]<<8), nc = sec[6] | (sec[7]<<8);
    int fn = sec[8] | (sec[9]<<8) | (sec[10]<<16) | (sec[11]<<24);
    int fw = sec[16] | (sec[17]<<8), fh = sec[18] | (sec[19]<<8);
    if (ci == 0) { cur = fn; paylen = 0; expect = nc; got = 0; *w = fw?fw:320; *h = fh?fh:240; }
    if (cur != fn) continue;
    uint32_t pl = SECTOR_USER - SUBHDR_LEN;
    if (paylen + pl <= maxpay) { memcpy(payload + paylen, sec + SUBHDR_LEN, pl); paylen += pl; }
    got++;
    if (expect > 0 && got >= expect) {
      if (fn == want) return (int)paylen;
      cur = -1; paylen = 0; expect = 0; got = 0;
    }
  }
  return -1;
}

// Append a block: DC word then optional (run,level) AC then EOB. qscale=1.
static int put_block(uint16_t* c, int o, int dc, int run, int level, int have_ac) {
  c[o++] = (uint16_t)((1<<10) | (dc & 0x3FF));
  if (have_ac) c[o++] = (uint16_t)(((run & 0x3F)<<10) | (level & 0x3FF));
  c[o++] = 0xFE00;
  return o;
}

// Synthetic table self-test: decode a single 16x16 macroblock through the REAL mednafen MDEC
// with our uploaded quant+IDCT tables. (1) DC-only must be a flat block. (2) one low AC coeff
// (scan pos 1) must be a clean single-frequency cosine ramp, not noise/stripes. This isolates
// quant/IDCT-table correctness from the (already-proven) VLC.
#define MBN 16                       // 16 macroblocks tall (16x256) so the MDEC FIFO drains fully
static void idct_test(void) {
  static uint16_t codes[64*MBN]; static uint16_t px[16*16*MBN];
  const int H = 16*MBN;
  // (1) DC-only: every MB = Cr,Cb (DC0), Y0..Y3 (DC=200). Expect a flat block.
  int o = 0;
  for (int m=0;m<MBN;m++){ o=put_block(codes,o,0,0,0,0); o=put_block(codes,o,0,0,0,0);
    for(int y=0;y<4;y++) o=put_block(codes,o,200,0,0,0); }
  mdec_decode_to_rgb555(codes, o, 16, H, px);
  int base = (8*16)*16;               // sample MB #8 (row 128) Y0 top-left (past FIFO warm-up)
  int mn=0x7fff,mx=-1; for(int i=base;i<base+256;i++){int v=px[i];if(v<mn)mn=v;if(v>mx)mx=v;}
  fprintf(stderr,"[idcttest] DC-only MB#8: green range [%d,%d] (flat => equal)\n",
          (mn>>5)&31, (mx>>5)&31);
  // (2) single AC at scan pos 1 (run0,level) in every MB's Y0. Expect a clean single-frequency
  // cosine ramp across the 8x8 (scan pos 1 -> raster (1,0) -> varies top->bottom, flat l-r).
  o = 0;
  for (int m=0;m<MBN;m++){ o=put_block(codes,o,0,0,0,0); o=put_block(codes,o,0,0,0,0);
    o=put_block(codes,o,0,0,200,1); for(int y=0;y<3;y++) o=put_block(codes,o,0,0,0,0); }
  mdec_decode_to_rgb555(codes, o, 16, H, px);
  fprintf(stderr,"[idcttest] 1-AC(scanpos1=raster(1,0)) MB#8 Y0 8x8 green ((v>>5)&31):\n");
  for (int y=0;y<8;y++){ fprintf(stderr,"  ");
    for(int x=0;x<8;x++){ int v=px[(8*16)*16+y*16+x]; fprintf(stderr,"%3d ",((v>>5)&31)); } fprintf(stderr,"\n"); }
  // (3) macroblock placement, WIDE frame: 320x16 = 20 MBs in ONE row, MB k DC=20+k*2.
  // Read each MB center; must increase monotonically LEFT->RIGHT. (The old 16-wide test only
  // exercised vertical order and missed horizontal-stride bugs — this catches the FMV spread.)
  static uint16_t codesW[64*20]; static uint16_t pxW[320*16];
  o = 0;
  for (int m=0;m<20;m++){ int dc=20+m*2; o=put_block(codesW,o,0,0,0,0); o=put_block(codesW,o,0,0,0,0);
    for(int y=0;y<4;y++) o=put_block(codesW,o,dc,0,0,0); }
  mdec_decode_to_rgb555(codesW, o, 320, 16, pxW);
  fprintf(stderr,"[idcttest] MB-placement WIDE (320x16): per-MB center green L->R (expect monotonic):\n  ");
  for (int m=0;m<20;m++){ int v=pxW[(8)*320 + m*16+8]; fprintf(stderr,"%d ",((v>>5)&31)); }
  fprintf(stderr,"\n");
  // (4) MULTI-ROW placement: 320x48 = 3 rows x 20 cols = 60 MBs, MB k (emit/raster order)
  // DC=20+k. Print the 3x20 grid of per-MB center green. Must read as a raster ramp
  // (row0: cols 0..19 lowest, row2 highest). Catches a row-stride / row-order bug.
  static uint16_t codesG[64*60]; static uint16_t pxG[320*48];
  o = 0;
  for (int m=0;m<60;m++){ int dc=12+m; o=put_block(codesG,o,0,0,0,0); o=put_block(codesG,o,0,0,0,0);
    for(int y=0;y<4;y++) o=put_block(codesG,o,dc,0,0,0); }
  mdec_decode_to_rgb555(codesG, o, 320, 48, pxG);
  fprintf(stderr,"[idcttest] MB-placement GRID (320x48, 3x20, DC ramps in raster order):\n");
  for (int ry=0;ry<3;ry++){ fprintf(stderr,"  ");
    for (int rx=0;rx<20;rx++){ int v=pxG[(ry*16+8)*320 + rx*16+8]; fprintf(stderr,"%2d ",((v>>5)&31)); }
    fprintf(stderr,"\n"); }
}

// Decode a real STR frame through our full pipeline (VLC + mdec_decode_to_rgb555 = same tiling
// the player uses) and print a coarse ASCII brightness map of the resulting `pixels`. This
// shows DIRECTLY whether content macroblocks land contiguously or are spread across the frame.
static void framemap(uint32_t lba, uint32_t size, int want) {
  if (!disc_open()) { fprintf(stderr,"disc_open failed\n"); return; }
  static uint8_t payload[1<<20]; int w=320,h=240;
  int paylen = demux_frame(lba,size,want,payload,sizeof payload,&w,&h);
  if (paylen<0){ fprintf(stderr,"frame %d not found\n",want); return; }
  static uint16_t codes[1<<20]; static uint16_t px[1024*512];
  int n = bs_decode_frame(payload,(uint32_t)paylen,w,h,codes,(int)(sizeof codes/2));
  mdec_decode_to_rgb555(codes,n,w,h,px);
  fprintf(stderr,"frame %d: %dx%d, %d codes -> ASCII luma map (each cell = 8x8 px avg):\n",want,w,h,n);
  // Downsample to (w/8) x (h/8). chars: ' '=bright(white bg) ... '#'=dark(content).
  const char* ramp = " .:-=+*#%@";
  for (int cy=0; cy<h; cy+=8){ fprintf(stderr,"  ");
    for (int cx=0; cx<w; cx+=8){ long s=0,c=0;
      for(int y=cy;y<cy+8&&y<h;y++)for(int x=cx;x<cx+8&&x<w;x++){int v=px[y*w+x];
        int g=(v>>5)&31; s+=g; c++; }
      int avg = c? (int)(s/c):0; int idx = 9 - (avg*9/31); if(idx<0)idx=0; if(idx>9)idx=9;
      fputc(ramp[idx], stderr); }
    fputc('\n', stderr); }
}

// Our XA decoder under test (native_fmv.c).
int xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq);

// INDEPENDENT reference XA-ADPCM decode (separate transcription of the CD-XA spec), to diff
// against our xa_decode_sector. Decodes one 2352B sector -> interleaved S16 stereo (4-bit
// stereo path). Returns stereo frame count. `h` = per-channel 2-sample history (persists).
static int xa_ref(const uint8_t* raw, int16_t* out, int h[2][2]) {
  static const int W[5][2] = {{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
  int coding = raw[19]; if (coding & 0x10) return -1;       // ref only handles 4-bit
  int stereo = coding & 1;
  int16_t L[2016], R[2016]; int nl=0, nr=0;
  for (int g = 0; g < 18; g++) {
    const uint8_t* sg = raw + 24 + g*128;
    for (int u = 0; u < 8; u++) {
      int pidx = (u & 3) | ((u & 4) << 1);
      int p = sg[pidx], pc = sg[4 + pidx];
      int sh = p & 0x0F, fl = p >> 4;
      int ch = (u & 1) && stereo;          // 0=L,1=R
      int s1 = h[ch][0], s2 = h[ch][1];
      int16_t smp[28];
      for (int i = 0; i < 28; i++) {
        int nib = sg[16 + i*4 + (u >> 1)];
        nib = (u & 1) ? (nib & 0xF0) : ((nib << 4) & 0xF0);  // high/low nibble -> hi 4 bits
        int v = (int16_t)(nib << 8); v >>= sh;
        v += (s1 * W[fl][0] + s2 * W[fl][1]) >> 6;
        if (v < -32768) v = -32768; else if (v > 32767) v = 32767;
        smp[i] = v; s2 = s1; s1 = v;
      }
      h[ch][0] = s1; h[ch][1] = s2;
      if (p != pc) { memset(smp, 0, sizeof smp); /* note: history already advanced, matches ours? */ }
      if (stereo) { if (ch) for(int i=0;i<28;i++) R[nr++]=smp[i]; else for(int i=0;i<28;i++) L[nl++]=smp[i]; }
      else        { for(int i=0;i<28;i++){ L[nl++]=smp[i]; R[nr++]=smp[i]; } }
    }
  }
  for (int i = 0; i < nl; i++) { out[2*i]=L[i]; out[2*i+1]=R[i]; }
  return nl;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1],"idcttest")) { idct_test(); return 0; }
  if (argc >= 4 && !strcmp(argv[1],"xacmp")) {     // xacmp <lba> <nsectors>: sound oracle
    if (!disc_open()) return 1;
    uint32_t lba=(uint32_t)strtoul(argv[2],0,0), n=(uint32_t)strtoul(argv[3],0,0);
    uint8_t raw[2352]; int16_t ours[2016*2], ref[2016*2];
    int16_t ho[2][2]={{0,0},{0,0}}; int hr[2][2]={{0,0},{0,0}};
    int asec=0, maxdiff=0; long diffs=0, samples=0;
    for (uint32_t s=0;s<n;s++){ if(!disc_read_raw(lba+s,raw,2352)) break;
      if (!(raw[18]&0x04)) continue;                // audio sectors only
      int fq; int no=xa_decode_sector(raw,ours,ho,&fq);
      int nr=xa_ref(raw,ref,hr); asec++;
      if (no!=nr){ fprintf(stderr,"sector %u: count mismatch ours=%d ref=%d\n",lba+s,no,nr); continue; }
      for (int i=0;i<no*2;i++){ int d=ours[i]-ref[i]; if(d<0)d=-d; if(d>maxdiff)maxdiff=d; if(d)diffs++; samples++; }
    }
    fprintf(stderr,"xacmp: %d audio sectors, %ld samples, %ld differing, max|diff|=%d\n",
            asec, samples, diffs, maxdiff);
    fprintf(stderr, diffs? "MISMATCH (our XA decode diverges from the reference)\n"
                         : "MATCH (XA decode bit-exact vs independent reference)\n");
    return diffs?3:0; }
  if (argc >= 5 && !strcmp(argv[1],"framemap")) {
    framemap((uint32_t)strtoul(argv[2],0,0),(uint32_t)strtoul(argv[3],0,0),atoi(argv[4])); return 0; }
  if (argc >= 4 && !strcmp(argv[1],"strscan")) {     // strscan <lba> <nsectors>: dump CD-XA framing
    if (!disc_open()) return 1;
    uint32_t lba=(uint32_t)strtoul(argv[2],0,0), n=(uint32_t)strtoul(argv[3],0,0);
    uint8_t raw[2352]; int vid=0,aud=0,other=0;
    for (uint32_t s=0;s<n;s++){ if(!disc_read_raw(lba+s,raw,2352)) break;
      int mode=raw[15], sm=raw[18], coding=raw[19];
      uint16_t vmagic = raw[24]|(raw[25]<<8);   // STR video magic in user data
      const char* kind = (sm&0x04)?"AUDIO":(sm&0x08)?"DATA":(vmagic==0x0160)?"VIDEO":"?";
      if (sm&0x04) aud++; else if (vmagic==0x0160) vid++; else other++;
      if (s<24) fprintf(stderr,"  sec %u: mode%d submode=%02x coding=%02x ch=%d vmagic=%04x -> %s\n",
                        lba+s, mode, sm, coding, raw[17], vmagic, kind);
    }
    fprintf(stderr,"totals over %u: video=%d audio=%d other=%d\n", n, vid, aud, other);
    // decode the coding byte of the first audio sector
    for (uint32_t s=0;s<n;s++){ if(!disc_read_raw(lba+s,raw,2352)) break;
      if (raw[18]&0x04){ int c=raw[19];
        fprintf(stderr,"first AUDIO sec %u: coding=%02x -> %s, %s, %d-bit\n", lba+s, c,
          (c&1)?"stereo":"mono", (c&4)?"18900Hz":"37800Hz", (c&0x10)?"8":"4"); break; } }
    return 0; }
  if (argc < 4) { fprintf(stderr, "usage: %s <lba> <size> <frame#> | %s idcttest | %s framemap <lba> <size> <frame#>\n", argv[0], argv[0], argv[0]); return 2; }
  uint32_t lba = (uint32_t)strtoul(argv[1],0,0), size = (uint32_t)strtoul(argv[2],0,0);
  int want = atoi(argv[3]);
  if (!disc_open()) { fprintf(stderr,"disc_open failed (set PSXPORT_TOMBA2_DISC/.env)\n"); return 1; }

  static uint8_t payload[1<<20];
  int w=320, h=240;
  int paylen = demux_frame(lba, size, want, payload, sizeof payload, &w, &h);
  if (paylen < 0) { fprintf(stderr,"frame %d not found\n", want); return 1; }
  fprintf(stderr, "frame %d: %dx%d, payload %d bytes, qscale=%d\n",
          want, w, h, paylen, (payload[4]|(payload[5]<<8))&0x3F);

  static uint16_t ours[1<<20], ref[1<<20];
  int no = bs_decode_frame(payload, (uint32_t)paylen, w, h, ours, (int)(sizeof ours/2));
  int nr = ref_decode      (payload, (uint32_t)paylen, w, h, ref,  (int)(sizeof ref /2));
  fprintf(stderr, "codes: ours=%d ref=%d\n", no, nr);

  // Walk both as blocks (DC word, AC words..., 0xFE00) and report first divergence.
  int n = no < nr ? no : nr, blk = 0, inblk = 0, diffs = 0;
  for (int i = 0; i < n; i++) {
    if (ours[i] != ref[i]) {
      if (diffs == 0)
        fprintf(stderr, "FIRST DIVERGENCE @code %d (block %d, coeff %d): ours=%04x ref=%04x\n"
                        "  ours: run=%d lvl=%d   ref: run=%d lvl=%d\n",
                i, blk, inblk, ours[i], ref[i],
                ours[i]>>10, (ours[i]&0x3FF)>=0x200?(ours[i]&0x3FF)-0x400:(ours[i]&0x3FF),
                ref[i]>>10,  (ref[i]&0x3FF) >=0x200?(ref[i]&0x3FF) -0x400:(ref[i]&0x3FF));
      diffs++;
    }
    if (ours[i] == 0xFE00) { blk++; inblk = 0; } else inblk++;
  }
  if (diffs == 0 && no == nr) fprintf(stderr, "MATCH: %d codes identical (VLC is correct)\n", no);
  else fprintf(stderr, "TOTAL: %d differing codes (of %d compared); ours=%d ref=%d\n", diffs, n, no, nr);
  return diffs ? 3 : 0;
}
