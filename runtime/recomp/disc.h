// disc.h — DiscState: the native by-LBA CHD disc backend's per-instance state (disc.c).
// Owned by Game (`game->disc`); disc_* functions take the instance explicitly. Plain C struct
// (like cdc_state.h / xa_state.h) so disc.c stays C; C++ callers reach it via game->disc.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DiscState {
  struct _chd_file* chd;         // open CHD handle (libchdr chd_file; NULL until disc_open)
  uint32_t frames_per_hunk;
  uint32_t hunk_count;
  uint32_t hunk_bytes;
  uint8_t* hunk_buf;             // one-hunk read cache
  uint32_t cached_hunk;          // 0xFFFFFFFF = cache empty
} DiscState;

void disc_state_init(DiscState* d);
int  disc_open(DiscState* d);
int  disc_read_sector(DiscState* d, uint32_t lba, uint8_t* out);           // 2048-byte data sector
int  disc_read_raw(DiscState* d, uint32_t lba, uint8_t* out, uint32_t n);  // raw 2352-byte sector
int  disc_find_file(DiscState* d, const char* path, uint32_t* out_lba, uint32_t* out_size);
int  disc_extract_file(DiscState* d, const char* iso_path, const char* out_path);  // self-provisioning: ISO9660 file -> host file (disc_provision.cpp)
int  disc_dropin_scan(char* out, unsigned out_cap);  // first *.chd in cwd, NUL-terminated into out (disc_provision.cpp)

#ifdef __cplusplus
}
#endif
