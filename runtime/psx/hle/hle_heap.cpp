// hle_heap.cpp - BIOS A0:0x33-0x39 malloc family: a native first-fit arena over guest RAM.
#include "game.h"
#include "hle.h"

#include <lucent/log.h>

enum { HEAP_MAX_BLOCKS = 4096 };

void Hle::heapInit(uint32_t addr, uint32_t size) {
  heap_base = addr;
  heap_size = size;
  nblk = 1;
  blk[0].addr = addr;
  blk[0].size = size;
  blk[0].used = 0;
  heap_ok = 1;
}

uint32_t Hle::heapAlloc(uint32_t size) {
  if (!heap_ok || size == 0) {
    return 0;
  }
  size = (size + 7u) & ~7u;
  for (int i = 0; i < nblk; i++) {
    if (blk[i].used || blk[i].size < size) {
      continue;
    }
    if (blk[i].size > size && nblk < HEAP_MAX_BLOCKS) {
      for (int j = nblk; j > i + 1; j--) {
        blk[j] = blk[j - 1];
      }
      blk[i + 1].addr = blk[i].addr + size;
      blk[i + 1].size = blk[i].size - size;
      blk[i + 1].used = 0;
      blk[i].size = size;
      nblk++;
    }
    blk[i].used = 1;
    return blk[i].addr;
  }
  // NO BLOCK COULD SATISFY THE REQUEST. Reported, not silently NULL: a heap whose size came from a
  // wrong InitHeap argument refuses everything, and the guest's own null check then routes around the
  // failure so the symptom surfaces arbitrarily far away. Novelty-capped (first refusal, then powers
  // of two) so it names the condition without becoming a per-call firehose.
  ++heap_refused;
  if ((heap_refused & (heap_refused - 1)) == 0) {
    lucent::warn("hle",
                 "malloc({}) REFUSED — heap base=0x{:08X} size=0x{:X} ({} block(s)); "
                 "refusal #{}. A size of 0 here means InitHeap was called with a wrong a1.",
                 size,
                 heap_base,
                 heap_size,
                 nblk,
                 heap_refused);
  }
  return 0;
}

void Hle::heapCoalesce() {
  for (int i = 0; i + 1 < nblk;) {
    if (!blk[i].used && !blk[i + 1].used) {
      blk[i].size += blk[i + 1].size;
      for (int j = i + 1; j + 1 < nblk; j++) {
        blk[j] = blk[j + 1];
      }
      nblk--;
    } else {
      i++;
    }
  }
}

void Hle::heapFree(uint32_t addr) {
  if (!addr) {
    return;
  }
  for (int i = 0; i < nblk; i++) {
    if (blk[i].addr == addr && blk[i].used) {
      blk[i].used = 0;
      heapCoalesce();
      return;
    }
  }
}

uint32_t Hle::heapBlockSize(uint32_t addr) const {
  for (int i = 0; i < nblk; i++) {
    if (blk[i].addr == addr && blk[i].used) {
      return blk[i].size;
    }
  }
  return 0;
}
