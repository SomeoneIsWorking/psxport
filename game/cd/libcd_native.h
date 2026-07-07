// libcd_native.h — native-code wrapper around the substrate's libcd chain. Used by the
// pc_faithful (pc_skip=false) branch of file-table build; each method is a real C++ call the port
// makes, which happens to delegate to the recompiled substrate leaf underneath. Byte-exact by
// construction: the writes to guest RAM (0x800AC2D4 dir-cache, 0x80102768 file-table, task stack
// scratch) are the SAME writes the substrate emits — because it is literally that code running.
//
// This is the "two native CD readers" model (memory: pc-skip-two-cd-readers):
//   - pc_skip=true (shortcut)   → cdlibcd_new_media / cdlibcd_cache_file in engine.cpp
//                                 (native ISO9660, bypasses libcd, no stack scratch).
//   - pc_skip=false (faithful)  → this class (LibcdNative). Same guest RAM as substrate B.
//
// Each method can later be swapped for hand-written C++ that reproduces the substrate's byte
// pattern independently; the wrapper form gives us byte-exactness today with room to iterate.
#pragma once
#include <cstdint>
struct Core;

class LibcdNative {
public:
  explicit LibcdNative(Core* c) : core(c) {}

  // FUN_8008BBE8 — CdNewMedia. Reads PVD sector 16, walks the path-table, populates the guest
  // in-memory directory table at 0x80102D68 (0x2C stride, up to 128 entries). Returns 1 on
  // success, 0 on disc/read error. Zero-arg, no output pointer.
  uint32_t newMedia();

  // FUN_8008BF50 — CdCacheFile. Given a directory INDEX (into the dir table populated by
  // newMedia), reads that directory's file records into the guest in-memory file table at
  // 0x80102768 (0x18 stride, up to 64 entries). Returns 1 on success, 0xFFFFFFFF on error.
  uint32_t cacheFile(uint32_t dir_index);

  // FUN_8008B8F0 — CdSearchFile. Walks path, calls cacheFile for the innermost dir, memcmp-
  // searches the file table for a matching name. On match: copies the 24-byte CdlFILE record
  // into the caller-owned output buffer at `out_cdlfile_guest_addr` (6 words @ that guest
  // address). Returns the guest address of the match (or 0 on no-match).
  //
  // The 24-byte output write into caller-owned guest memory is what byte-exactness requires —
  // that address must equal what the substrate caller would have used, i.e. the port on core A
  // has to allocate its `CdlFILE` at the same guest-RAM address the substrate caller would.
  uint32_t searchFile(uint32_t out_cdlfile_guest_addr, uint32_t path_guest_addr);

private:
  Core* core;
};
