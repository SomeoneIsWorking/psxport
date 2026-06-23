// render_all_seq: render EVERY sequenced song on the disc (TOMBA2.SND's 10 + the area-file
// sequences) into ONE WAV, each segment a fixed length, with a printed index — so the conversation/
// event track can be identified by ear. Area-file sequences carry no VAB (pBAV=0), so they borrow a
// TOMBA2.SND bank; identify by MELODY/rhythm (timbre/pitch may be off). The audio engine is shared
// with the game (engine/audio/native_audio.c). Build line at bottom.
#include "engine/audio/native_audio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* load(const char* p, long* n){ FILE* f=fopen(p,"rb"); if(!f){fprintf(stderr,"open %s\n",p);exit(1);}
  fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET); uint8_t* b=malloc(*n); fread(b,1,*n,f); fclose(f); return b; }
static long vab_file_off(const uint8_t* snd, int vi){ return (long)(snd[vi*2]|(snd[vi*2+1]<<8))*0x800; }

#define RATE 44100
#define SECS 14
static int16_t seg[RATE*SECS*2];

int main(int argc, char** argv){
  const char* sndpath = argc>1?argv[1]:"scratch/bin/snd/TOMBA2.SND";
  const char* out     = argc>2?argv[2]:"scratch/ref/all_disc_seqs.wav";
  long sndlen; uint8_t* snd = load(sndpath, &sndlen);

  // (label, file, seqOff, vabIdx) — TOMBA2.SND's 10 (S2SV vabs) then the area-file sequences (vab 7).
  struct Item { const char* label; const char* file; long seqOff; int vab; } items[] = {
    {"SND song0","",0x370,7},{"SND song1","",0x304,7},{"SND song2","",0x240,7},{"SND song3","",0x30,7},
    {"SND song4","",0x44c,3},{"SND song5","",0x66c,21},{"SND song6","",0x9c0,21},{"SND song7","",0x102c,21},
    {"SND song8","",0x1f58,21},{"SND song9","",0x25b0,7},
    {"A05 #0","scratch/disc_files/BIN/A05.BIN",0x3756c,7},{"A05 #1","scratch/disc_files/BIN/A05.BIN",0x37710,7},
    {"A0A","scratch/disc_files/BIN/A0A.BIN",0x1e258,7},
    {"A0B","scratch/disc_files/BIN/A0B.BIN",0x1b710,7},
    {"A0C","scratch/disc_files/BIN/A0C.BIN",0x1d244,7},
    {"A0D","scratch/disc_files/BIN/A0D.BIN",0x1c1c4,7},
    {"A0E","scratch/disc_files/BIN/A0E.BIN",0x1c4d8,7},
    {"A0F","scratch/disc_files/BIN/A0F.BIN",0x20198,7},
    {"A0K #0","scratch/disc_files/BIN/A0K.BIN",0x14634,7},{"A0K #1","scratch/disc_files/BIN/A0K.BIN",0x147d8,7},
    {"A0L #0","scratch/disc_files/BIN/A0L.BIN",0xea44,7},{"A0L #1","scratch/disc_files/BIN/A0L.BIN",0xfb0c,7},
    {"A0L #2","scratch/disc_files/BIN/A0L.BIN",0x10b04,7},
  };
  int N = sizeof(items)/sizeof(items[0]);

  FILE* w = fopen(out,"wb");
  uint32_t total=0; // frames; patch header later
  // WAV header placeholder
  uint8_t hdr[44]={0}; fwrite(hdr,1,44,w);

  printf("# index (each segment %ds):\n", SECS);
  int t=0;
  for(int i=0;i<N;i++){
    long flen; uint8_t* fb = items[i].file[0] ? load(items[i].file,&flen) : snd;
    NaVab vab; NaSeq s;
    int okv = na_vab_open_at(&vab, snd, vab_file_off(snd, items[i].vab));
    int oks = na_seq_open(&s, fb, &vab, items[i].seqOff);
    printf("  %3d:%02d  %-12s  vab=%-2d  open=%s\n", t/60, t%60, items[i].label, items[i].vab,
           (okv==0&&oks==0)?"ok":"FAIL");
    memset(seg,0,sizeof(seg));
    if(okv==0&&oks==0){
      int done=0; while(done<RATE*SECS){ int got=na_seq_render(&s, seg+done*2, RATE*SECS-done, RATE); if(got<=0)break; done+=got; }
      na_seq_free(&s);
    }
    fwrite(seg,1,sizeof(seg),w);
    if(items[i].file[0]) free(fb);
    t+=SECS;
  }
  uint32_t nframes=(uint32_t)N*RATE*SECS, datasz=nframes*4;
  // patch WAV header (PCM 16-bit stereo 44100)
  fseek(w,0,SEEK_SET);
  uint32_t riff=36+datasz, fmtlen=16, srate=RATE, byterate=RATE*4; uint16_t fmt=1,ch=2,bps=16,blk=4;
  fwrite("RIFF",1,4,w); fwrite(&riff,4,1,w); fwrite("WAVE",1,4,w);
  fwrite("fmt ",1,4,w); fwrite(&fmtlen,4,1,w); fwrite(&fmt,2,1,w); fwrite(&ch,2,1,w);
  fwrite(&srate,4,1,w); fwrite(&byterate,4,1,w); fwrite(&blk,2,1,w); fwrite(&bps,2,1,w);
  fwrite("data",1,4,w); fwrite(&datasz,4,1,w);
  fclose(w);
  printf("# wrote %s (%u segments, %ds total)\n", out, N, N*SECS);
  return 0;
}
