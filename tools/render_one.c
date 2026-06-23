// render_one: render ONE sequence from <seqfile>@<seqoff> using a VAB from <vabfile>@<vaboff>.
// For the area conversation music: seq is in A0x.BIN, the instrument bank (ps=18) is the area bundle's
// bank1. Build: gcc -O2 -w -o scratch/bin/render_one tools/render_one.c engine/audio/native_audio.c -lm
#include "engine/audio/native_audio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t* load(const char* p,long* n){FILE* f=fopen(p,"rb");if(!f){fprintf(stderr,"open %s\n",p);exit(1);}fseek(f,0,SEEK_END);*n=ftell(f);fseek(f,0,SEEK_SET);uint8_t* b=malloc(*n);fread(b,1,*n,f);fclose(f);return b;}
int main(int argc,char**argv){
  if(argc<6){fprintf(stderr,"usage: %s seqfile seqoff_hex vabfile vaboff_hex out.wav [secs]\n",argv[0]);return 1;}
  long sn,vn; uint8_t* sb=load(argv[1],&sn); uint8_t* vb=load(argv[3],&vn);
  long soff=strtol(argv[2],0,16), voff=strtol(argv[4],0,16);
  int secs=argc>6?atoi(argv[6]):30; int RATE=44100;
  NaVab vab; NaSeq s;
  if(na_vab_open_at(&vab,vb,voff)){fprintf(stderr,"vab open fail @%lx\n",voff);return 1;}
  if(na_seq_open(&s,sb,&vab,soff)){fprintf(stderr,"seq open fail @%lx\n",soff);return 1;}
  fprintf(stderr,"vab ps=%d ts=%d  rendering %ds\n",vab.ps,vab.ts,secs);
  long tot=(long)RATE*secs; int16_t* out=calloc(tot*2,2);
  long done=0; while(done<tot){int got=na_seq_render(&s,out+done*2,(int)(tot-done),RATE); if(got<=0)break; done+=got;}
  FILE* w=fopen(argv[5],"wb"); uint32_t ds=done*4,riff=36+ds,sr=RATE,br=RATE*4,fl=16; uint16_t fmt=1,ch=2,bp=16,bl=4;
  fwrite("RIFF",1,4,w);fwrite(&riff,4,1,w);fwrite("WAVE",1,4,w);fwrite("fmt ",1,4,w);fwrite(&fl,4,1,w);
  fwrite(&fmt,2,1,w);fwrite(&ch,2,1,w);fwrite(&sr,4,1,w);fwrite(&br,4,1,w);fwrite(&bl,2,1,w);fwrite(&bp,2,1,w);
  fwrite("data",1,4,w);fwrite(&ds,4,1,w);fwrite(out,1,ds,w);fclose(w);
  fprintf(stderr,"wrote %s (%.1fs)\n",argv[5],done/(double)RATE);
  return 0;
}
