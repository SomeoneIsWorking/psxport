#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
int disc_open(void);
int disc_read_sector(uint32_t lba, uint8_t* out);
// Find which disc LBA a captured buffer's data came from, and the consecutive match length.
// args: file  file_data_off(hex)  nbytes(dec)  lba_lo  lba_hi
int main(int argc,char**argv){
  FILE* f=fopen(argv[1],"rb"); if(!f){perror(argv[1]);return 1;}
  uint32_t off=strtoul(argv[2],0,16), nb=strtoul(argv[3],0,0);
  uint32_t lo=strtoul(argv[4],0,0), hi=strtoul(argv[5],0,0);
  static uint8_t buf[0x200000]; size_t got=fread(buf,1,sizeof buf,f); fclose(f);
  if(!disc_open()){fprintf(stderr,"disc_open fail\n");return 1;}
  uint8_t sec[2048];
  // 1) find best single-sector start match for the first sector of data
  int best_lba=-1; uint32_t best_match=0;
  for(uint32_t lba=lo;lba<=hi;lba++){
    if(!disc_read_sector(lba,sec))continue;
    uint32_t m=0; for(int j=0;j<2048;j++) if(buf[off+j]==sec[j])m++;
    if(m>best_match){best_match=m;best_lba=lba;}
  }
  printf("best first-sector LBA=%d (%u/2048 match)\n",best_lba,best_match);
  if(best_lba<0)return 0;
  // 2) consecutive match from best_lba
  uint32_t done=0,same=0;
  for(uint32_t nsec=0;done<nb;nsec++){
    if(!disc_read_sector(best_lba+nsec,sec))break;
    uint32_t n=nb-done<2048?nb-done:2048;
    for(uint32_t j=0;j<n;j++) if(buf[off+done+j]==sec[j])same++;
    done+=n;
  }
  printf("consecutive from LBA %d: %u/%u = %.2f%%\n",best_lba,same,done,100.0*same/done);
  return 0;
}
