// dump_all_seq: recursively extract EVERY file from the PSX CHD and report which contain libsnd
// sequences ('pQES') and sound banks ('pBAV'). Used to locate ALL sequenced audio on the disc (the
// conversation/event music is NOT in TOMBA2.SND's 10 sequences). Usage: dump_all_seq [disc.chd] [outdir]
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <optional>
#include <libchdr/chd.h>
#include "common/env.h"
namespace fs = std::filesystem;
constexpr uint32_t RAW = 2448, USR = 2048;
struct Chd {
  chd_file* c=nullptr; uint32_t hb=0,fph=0,hc=0,cached=UINT32_MAX; std::vector<uint8_t> buf;
  bool Open(const std::string& p){ if(chd_open(p.c_str(),CHD_OPEN_READ,nullptr,&c)!=CHDERR_NONE)return false;
    const chd_header*h=chd_get_header(c); hb=h->hunkbytes; fph=hb/RAW; hc=h->totalhunks; buf.resize(hb); return fph>0; }
  ~Chd(){ if(c)chd_close(c); }
  bool Sec(uint32_t lba,uint8_t*out){ uint32_t hk=lba/fph,off=(lba%fph)*RAW; if(hk>=hc)return false;
    if(hk!=cached){ if(chd_read(c,hk,buf.data())!=CHDERR_NONE)return false; cached=hk; }
    const uint8_t*raw=buf.data()+off; uint32_t d=(raw[15]==2)?24:16; memcpy(out,raw+d,USR); return true; }
};
static uint32_t LE32(const uint8_t*p){return p[0]|(p[1]<<8)|(p[2]<<16)|(uint32_t(p[3])<<24);}
static std::string Norm(std::string n){ if(auto s=n.find(';');s!=std::string::npos)n.resize(s);
  for(char&c:n)c=(char)toupper((unsigned char)c); return n; }
static long g_seq=0,g_vab=0;
static void Walk(Chd& disc, uint32_t lba, uint32_t size, const std::string& path, const fs::path& outdir){
  uint32_t nsec=(size+USR-1)/USR; std::vector<uint8_t> dir((size_t)nsec*USR);
  for(uint32_t i=0;i<nsec;i++) if(!disc.Sec(lba+i,dir.data()+(size_t)i*USR)) return;
  for(uint32_t s=0;s<dir.size();s+=USR){ uint32_t pos=s;
    while(pos<s+USR){ uint8_t len=dir[pos]; if(!len)break;
      uint8_t flags=dir[pos+25], nlen=dir[pos+32];
      std::string nm((const char*)&dir[pos+33],nlen);
      uint32_t e_lba=LE32(&dir[pos+2]), e_size=LE32(&dir[pos+10]); pos+=len;
      if(nlen==1&&(nm[0]==0||nm[0]==1)) continue;          // . and ..
      std::string name=Norm(nm), full=path+"/"+name;
      if(flags&0x02){ Walk(disc,e_lba,e_size,full,outdir); continue; }   // subdir
      // file: read it, scan for pQES/pBAV
      uint32_t fsec=(e_size+USR-1)/USR; std::vector<uint8_t> fb((size_t)fsec*USR);
      for(uint32_t i=0;i<fsec;i++) disc.Sec(e_lba+i,fb.data()+(size_t)i*USR);
      fb.resize(e_size);
      long nq=0,nv=0; for(size_t i=0;i+4<=fb.size();i++){ if(!memcmp(&fb[i],"pQES",4))nq++; else if(!memcmp(&fb[i],"pBAV",4))nv++; }
      if(nq||nv){ g_seq+=nq; g_vab+=nv;
        printf("%-40s size=%-8u pQES=%-3ld pBAV=%-3ld\n", full.c_str(), e_size, nq, nv);
        fs::path op=outdir/full.substr(1); fs::create_directories(op.parent_path());
        std::ofstream(op,std::ios::binary).write((const char*)fb.data(),(std::streamsize)fb.size());
      }
    } }
}
int main(int argc,char**argv){
  auto rp = psxport::ResolveDiscPath(argc>1?argv[1]:"");
  std::string disc = rp ? *rp : "";
  fs::path outdir = argc>2?argv[2]:"scratch/disc_files";
  Chd d; if(!d.Open(disc)){ fprintf(stderr,"cannot open %s\n",disc.c_str()); return 1; }
  uint8_t sec[USR]; if(!d.Sec(16,sec)||memcmp(sec+1,"CD001",5)){ fprintf(stderr,"no PVD\n"); return 1; }
  uint32_t rlba=LE32(sec+156+2), rsize=LE32(sec+156+10);
  printf("# files containing sequences (pQES) or banks (pBAV):\n");
  Walk(d,rlba,rsize,"",outdir);
  printf("# TOTAL pQES=%ld pBAV=%ld across disc\n", g_seq, g_vab);
  return 0;
}
