// xa_scan: walk every sector of the CHD and group XA-ADPCM AUDIO sectors by (file,channel) into
// contiguous LBA ranges. Maps all streamed-audio streams on the disc (the conversation/event music
// is XA, not sequenced). Run: PSXPORT_TOMBA2_DISC=<chd> xa_scan [chd]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>
#define RAW 2448u
static chd_file* C; static uint32_t fph,hc,hb,cached=0xFFFFFFFFu; static uint8_t* hbuf;
static int openc(const char* p){ if(chd_open(p,CHD_OPEN_READ,0,&C)!=CHDERR_NONE)return 0;
  const chd_header* h=chd_get_header(C); hb=h->hunkbytes; fph=hb/RAW; hc=h->totalhunks; hbuf=malloc(hb); return fph>0; }
static int rd(uint32_t lba,uint8_t* o){ uint32_t hk=lba/fph,off=(lba%fph)*RAW; if(hk>=hc)return 0;
  if(hk!=cached){ if(chd_read(C,hk,hbuf)!=CHDERR_NONE)return 0; cached=hk; } memcpy(o,hbuf+off,2352); return 1; }
int main(int argc,char**argv){
  const char* p = argc>1?argv[1]:getenv("PSXPORT_TOMBA2_DISC");
  if(!p||!openc(p)){ fprintf(stderr,"cannot open CHD (arg or PSXPORT_TOMBA2_DISC)\n"); return 1; }
  uint32_t total=hc*fph;
  // per-channel tally: count, min/max LBA, last code (freq/stereo)
  uint32_t cnt[256]={0},lo[256],hi[256]; int code[256];
  for(int i=0;i<256;i++){ lo[i]=0xFFFFFFFF; hi[i]=0; }
  uint8_t raw[2352];
  for(uint32_t lba=0; lba<total; lba++){
    if(!rd(lba,raw)) break;
    if(raw[15]!=2) continue;
    uint8_t chan=raw[17],sub=raw[18]; int cd=raw[19];
    if(!(sub&0x04)) continue;                 // audio only
    cnt[chan]++; if(lba<lo[chan])lo[chan]=lba; if(lba>hi[chan])hi[chan]=lba; code[chan]=cd;
  }
  printf("# XA AUDIO sectors per channel (the music is 32 interleaved channels):\n");
  printf("# chan  sectors   LBA[lo..hi]      freq  ch    approx-seconds (mono=224 smp/sec, /freq)\n");
  for(int ch=0; ch<256; ch++) if(cnt[ch]){
    int fr=(code[ch]&0x04)?18900:37800, st=code[ch]&1;
    double secs = cnt[ch]*(st?112.0:224.0)/fr;
    printf("  %-4d  %-8u  [%u..%u]  %dHz %-6s  ~%.1fs\n",
           ch,cnt[ch],lo[ch],hi[ch],fr,st?"stereo":"mono",secs);
  }
  return 0;
}
