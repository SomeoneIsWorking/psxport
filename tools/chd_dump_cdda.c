// chd_dump_cdda: list the CHD's CD tracks and dump every CD-DA (AUDIO) track to a WAV. Tomba!2's
// music is Red Book CD-DA (the sequencer = jingles, the Mode2 XA = voice). CHD stores CDDA as raw
// 2352-byte PCM sectors, big-endian; we byte-swap to little-endian WAV. Run: PSXPORT_TOMBA2_DISC=<chd>
// chd_dump_cdda <outdir>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>
#define RAW 2448u
static chd_file* C; static uint32_t fph,hc,hb,cached=0xFFFFFFFFu; static uint8_t* hbuf;
static int openc(const char* p){ if(chd_open(p,CHD_OPEN_READ,0,&C)!=CHDERR_NONE)return 0;
  const chd_header* h=chd_get_header(C); hb=h->hunkbytes; fph=hb/RAW; hc=h->totalhunks; hbuf=malloc(hb); return fph>0; }
static int rd(uint32_t frame,uint8_t* o){ uint32_t hk=frame/fph,off=(frame%fph)*RAW; if(hk>=hc)return 0;
  if(hk!=cached){ if(chd_read(C,hk,hbuf)!=CHDERR_NONE)return 0; cached=hk; } memcpy(o,hbuf+off,2352); return 1; }

int main(int argc,char**argv){
  const char* outdir=argc>1?argv[1]:"scratch/ref/cdda";
  const char* p=getenv("PSXPORT_TOMBA2_DISC");
  if(!p||!openc(p)){ fprintf(stderr,"set PSXPORT_TOMBA2_DISC\n"); return 1; }
  // walk track metadata; CHD lays tracks sequentially, each padded to a multiple of 4 frames.
  uint32_t frame_base=0;
  for(uint32_t ti=0; ti<99; ti++){
    char meta[512]; uint32_t rl=0,rt=0; uint8_t rf=0;
    int trk=0,frames=0; char type[64]="",sub[64]="";
    if(chd_get_metadata(C,CDROM_TRACK_METADATA2_TAG,ti,meta,sizeof meta,&rl,&rt,&rf)==CHDERR_NONE)
      sscanf(meta,"TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d",&trk,type,sub,&frames);
    else if(chd_get_metadata(C,CDROM_TRACK_METADATA_TAG,ti,meta,sizeof meta,&rl,&rt,&rf)==CHDERR_NONE)
      sscanf(meta,"TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d",&trk,type,sub,&frames);
    else break;
    int isaudio = strstr(type,"AUDIO")!=NULL;
    printf("TRACK %d  TYPE=%s  FRAMES=%d  chd_frame_base=%u  %s\n",
           trk,type,frames,frame_base, isaudio?"-> dumping CDDA":"(data)");
    if(isaudio){
      char path[600]; snprintf(path,sizeof path,"%s/track%02d.wav",outdir,trk);
      FILE* f=fopen(path,"wb"); if(!f){ fprintf(stderr,"open %s (mkdir -p %s)\n",path,outdir); return 1; }
      uint32_t datasz=(uint32_t)frames*2352, riff=36+datasz, srate=44100, byterate=44100*4, fmtlen=16;
      uint16_t fmt=1,ch=2,bps=16,blk=4;
      fwrite("RIFF",1,4,f);fwrite(&riff,4,1,f);fwrite("WAVE",1,4,f);
      fwrite("fmt ",1,4,f);fwrite(&fmtlen,4,1,f);fwrite(&fmt,2,1,f);fwrite(&ch,2,1,f);
      fwrite(&srate,4,1,f);fwrite(&byterate,4,1,f);fwrite(&blk,2,1,f);fwrite(&bps,2,1,f);
      fwrite("data",1,4,f);fwrite(&datasz,4,1,f);
      uint8_t raw[2352];
      for(int fr=0;fr<frames;fr++){ if(!rd(frame_base+fr,raw)) break;
        for(int i=0;i<2352;i+=2){ uint8_t t=raw[i]; raw[i]=raw[i+1]; raw[i+1]=t; }  // BE->LE
        fwrite(raw,1,2352,f); }
      fclose(f);
      printf("   wrote %s  (%.1fs)\n", path, frames/75.0);
    }
    // advance base: tracks padded to multiple of 4 frames in the CHD
    frame_base += ((frames+3)/4)*4;
  }
  return 0;
}
