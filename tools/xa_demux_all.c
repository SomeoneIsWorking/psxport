// xa_demux_all: ONE pass over the CHD, split EVERY XA-ADPCM audio channel (file==1, chan 0..31) into
// its own WAV. Fast (single disc scan). The decoder is the same as xa_wavdump/native_fmv. The Tomba!2
// music/voice is 32 interleaved channels; this dumps them all so a track (e.g. the conversation BGM)
// can be identified. Run: PSXPORT_TOMBA2_DISC=<chd> xa_demux_all <outdir>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>

#define RAW_FRAME 2448u
static chd_file* s_chd; static uint32_t s_fph,s_hcount,s_hbytes,s_cached=0xFFFFFFFFu; static uint8_t* s_hbuf;
static int disc_open(const char* p){ if(chd_open(p,CHD_OPEN_READ,0,&s_chd)!=CHDERR_NONE)return 0;
  const chd_header* h=chd_get_header(s_chd); s_hbytes=h->hunkbytes; s_fph=h->hunkbytes/RAW_FRAME; s_hcount=h->totalhunks; s_hbuf=malloc(s_hbytes); return s_fph>0; }
static int disc_read_raw(uint32_t lba,uint8_t* o,uint32_t n){ if(n>2352u)n=2352u; uint32_t hk=lba/s_fph,off=(lba%s_fph)*RAW_FRAME;
  if(hk>=s_hcount)return 0; if(hk!=s_cached){ if(chd_read(s_chd,hk,s_hbuf)!=CHDERR_NONE)return 0; s_cached=hk; } memcpy(o,s_hbuf+off,n); return 1; }

static const int32_t XA_W[16][2]={{0,0},{60,0},{115,-52},{98,-55},{122,-60}};
static void xa_decode_unit(const uint8_t* in,int16_t* out,int shift,int filter){
  for(int i=0;i<28;i++){ int32_t s=(int16_t)(in[i]<<8); s>>=shift;
    s+=(out[i-1]*XA_W[filter][0]+out[i-2]*XA_W[filter][1])>>6;
    if(s<-32768)s=-32768; else if(s>32767)s=32767; out[i]=(int16_t)s; } }
static int xa_decode_sector(const uint8_t* raw,int16_t* out,int16_t hist[2][2],int* freq){
  int coding=raw[19],ishift=(coding&0x10)?0:1,stereo=coding&0x01,units=4<<ishift;
  if(freq)*freq=(coding&0x04)?18900:37800;
  static int16_t ch[2][4032+8]; int cp[2]={0,0};
  for(int group=0;group<18;group++){ const uint8_t* sg=raw+24+group*128;
    for(int unit=0;unit<units;unit++){
      int param=sg[(unit&3)|((unit&4)<<1)],pcopy=sg[4|(unit&3)|((unit&4)<<1)];
      uint8_t ib[28]; for(int i=0;i<28;i++){ uint8_t t=sg[16+i*4+(unit>>ishift)]; if(ishift){ t<<=(unit&1)?0:4; t&=0xf0; } ib[i]=t; }
      int ocn=(unit&1)&&stereo; int16_t ob[2+28]; ob[0]=hist[ocn][0]; ob[1]=hist[ocn][1];
      xa_decode_unit(ib,ob+2,param&0x0F,param>>4); hist[ocn][0]=ob[28]; hist[ocn][1]=ob[29];
      if(param!=pcopy) memset(ob,0,sizeof ob);
      if(stereo){ for(int s=0;s<28;s++) ch[ocn][cp[ocn]++]=ob[2+s]; }
      else { for(int s=0;s<28;s++){ ch[0][cp[0]++]=ob[2+s]; ch[1][cp[1]++]=ob[2+s]; } } } }
  int n=cp[0]; for(int i=0;i<n;i++){ out[2*i]=ch[0][i]; out[2*i+1]=ch[1][i]; } return n; }

#define NCH 32
int main(int argc,char**argv){
  const char* outdir=argc>1?argv[1]:"scratch/ref/xa";
  const char* disc=getenv("PSXPORT_TOMBA2_DISC");
  if(!disc||!disc_open(disc)){ fprintf(stderr,"set PSXPORT_TOMBA2_DISC\n"); return 1; }
  FILE* f[NCH]={0}; long frames[NCH]={0}; int freq[NCH]={0}; int16_t hist[NCH][2][2];
  memset(hist,0,sizeof hist);
  char path[512];
  for(int c=0;c<NCH;c++){ snprintf(path,sizeof path,"%s/ch%02d.wav",outdir,c); f[c]=fopen(path,"wb");
    if(!f[c]){ fprintf(stderr,"mkdir -p %s first\n",outdir); return 1; }
    for(int i=0;i<44;i++) fputc(0,f[c]); }   // header placeholder
  uint32_t total=s_hcount*s_fph; uint8_t raw[2352]; static int16_t pcm[4096*2];
  for(uint32_t lba=0;lba<total;lba++){
    if(!disc_read_raw(lba,raw,2352)) break;
    if(raw[15]!=2) continue;
    uint8_t file=raw[16],c=raw[17],sub=raw[18];
    if(file!=1||c>=NCH) continue;
    if(!(sub&0x04)) continue;
    int fr; int n=xa_decode_sector(raw,pcm,hist[c],&fr); freq[c]=fr;
    fwrite(pcm,sizeof(int16_t)*2,n,f[c]); frames[c]+=n;
  }
  printf("# channel : seconds : freq\n");
  for(int c=0;c<NCH;c++){
    uint32_t datasz=(uint32_t)frames[c]*4, riff=36+datasz, srate=freq[c]?freq[c]:37800, byterate=srate*4, fmtlen=16;
    uint16_t fmt=1,ch=2,bps=16,blk=4;
    fseek(f[c],0,SEEK_SET);
    fwrite("RIFF",1,4,f[c]); fwrite(&riff,4,1,f[c]); fwrite("WAVE",1,4,f[c]);
    fwrite("fmt ",1,4,f[c]); fwrite(&fmtlen,4,1,f[c]); fwrite(&fmt,2,1,f[c]); fwrite(&ch,2,1,f[c]);
    fwrite(&srate,4,1,f[c]); fwrite(&byterate,4,1,f[c]); fwrite(&blk,2,1,f[c]); fwrite(&bps,2,1,f[c]);
    fwrite("data",1,4,f[c]); fwrite(&datasz,4,1,f[c]); fclose(f[c]);
    if(frames[c]) printf("  ch%02d : %6.1fs : %dHz\n", c, frames[c]/(double)(freq[c]?freq[c]:37800), freq[c]);
  }
  return 0;
}
