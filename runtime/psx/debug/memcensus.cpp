// memcensus implementation — see memcensus.h for what this measures and what it structurally cannot.
#include "memcensus.h"

#include "cfg.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h> // atexit
#include <string.h>

extern "C" {
void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memmove(void *dst, const void *src, size_t n);
void *__wrap_memcpy(void *dst, const void *src, size_t n);
void *__wrap_memmove(void *dst, const void *src, size_t n);
}

namespace {

// Open-addressed, fixed size, never resized: this runs inside memcpy, so it must not allocate and
// must not itself copy. A full table is COUNTED and reported rather than silently dropping sites —
// a census that quietly discards its tail reads exactly like one whose tail is empty.
constexpr int SLOTS = 4096; // power of two

struct Slot {
  std::atomic<uint64_t> site{0}; // return address, 0 = empty
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> bytes{0};
};

Slot g_slots[SLOTS];
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_calls{0}, g_bytes{0}, g_dropped_calls{0}, g_dropped_bytes{0};
bool g_dumped = false;

// Relaxed throughout, and that is a deliberate accuracy/cost trade rather than an oversight: this is
// on the path of every copy in the program. Two threads racing the same slot can lose an increment,
// so treat the totals as a census, not an audit. The alternative — a lock inside memcpy — would
// change the thing being measured.
inline void record(void *ret, size_t n) {
  const uint64_t site = (uint64_t)(uintptr_t)ret;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(n, std::memory_order_relaxed);
  // Fibonacci hash of the return address; call sites are dense in a small address range, so the low
  // bits alone would collide heavily.
  uint64_t h = (site * 0x9E3779B97F4A7C15ull) >> 52;
  for (int probe = 0; probe < 32; probe++) {
    const int i = (int)((h + probe) & (SLOTS - 1));
    uint64_t cur = g_slots[i].site.load(std::memory_order_relaxed);
    if (cur == 0) {
      uint64_t expect = 0;
      if (!g_slots[i].site.compare_exchange_strong(expect, site, std::memory_order_relaxed)) {
        if (expect != site) {
          continue; // someone else claimed it for a different site
        }
      }
      cur = site;
    }
    if (cur == site) {
      g_slots[i].calls.fetch_add(1, std::memory_order_relaxed);
      g_slots[i].bytes.fetch_add(n, std::memory_order_relaxed);
      return;
    }
  }
  g_dropped_calls.fetch_add(1, std::memory_order_relaxed);
  g_dropped_bytes.fetch_add(n, std::memory_order_relaxed);
}

} // namespace

// The wrappers themselves. __builtin_return_address(0) is this frame's caller — the site that asked
// for the copy — which is the whole point: the profile already knew it was inside memmove.
extern "C" void *__wrap_memcpy(void *dst, const void *src, size_t n) {
  if (g_on.load(std::memory_order_relaxed)) {
    record(__builtin_return_address(0), n);
  }
  return __real_memcpy(dst, src, n);
}

extern "C" void *__wrap_memmove(void *dst, const void *src, size_t n) {
  if (g_on.load(std::memory_order_relaxed)) {
    record(__builtin_return_address(0), n);
  }
  return __real_memmove(dst, src, n);
}

void memcensus_init(void) {
  if (!cfg_on("PSXPORT_MEMCENSUS")) {
    return;
  }
  g_on.store(true, std::memory_order_relaxed);
  // Own exit hook so the report is emitted independently of optional diagnostics.
  // there would have made the census silently dump only when the sampling profiler happened to be on
  // too. A run killed by a SIGNAL still gets its dump through hostprof's on_term (which calls this),
  // but only when the profiler is armed — a census-only run that is SIGTERMed writes nothing, and
  // that gap is named here rather than discovered from an empty file.
  atexit(memcensus_dump);
}

void memcensus_dump(void) {
  if (!g_on.load(std::memory_order_relaxed) || g_dumped) {
    return;
  }
  g_dumped = true;
  g_on.store(false, std::memory_order_relaxed); // stop counting our own dump
  const char *out = cfg_str("PSXPORT_MEMCENSUS_OUT");
  if (!out) {
    out = "scratch/raw/memcensus.txt";
  }
  FILE *f = fopen(out, "w");
  if (!f) {
    return;
  }
  int used = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (g_slots[i].site.load(std::memory_order_relaxed)) {
      used++;
    }
  }
  fprintf(f,
          "# memcensus calls=%llu bytes=%llu sites=%d/%d dropped_calls=%llu dropped_bytes=%llu\n",
          (unsigned long long)g_calls.load(std::memory_order_relaxed),
          (unsigned long long)g_bytes.load(std::memory_order_relaxed),
          used,
          SLOTS,
          (unsigned long long)g_dropped_calls.load(std::memory_order_relaxed),
          (unsigned long long)g_dropped_bytes.load(std::memory_order_relaxed));
  fprintf(f,
          "# BLIND TO: compiler-inlined copies (no call to wrap), copies made INSIDE a shared object\n"
          "# (--wrap only rewrites references from this link), and anything outside the armed window.\n");
  for (int i = 0; i < SLOTS; i++) {
    const uint64_t site = g_slots[i].site.load(std::memory_order_relaxed);
    if (!site) {
      continue;
    }
    fprintf(f,
            "%016llx %llu %llu\n",
            (unsigned long long)site,
            (unsigned long long)g_slots[i].calls.load(std::memory_order_relaxed),
            (unsigned long long)g_slots[i].bytes.load(std::memory_order_relaxed));
  }
  // The module map, so a site inside a shared object can still be NAMED — same reason and same
  // format hostprof emits it, and prof_hot.py already reads it.
  if (FILE *m = fopen("/proc/self/maps", "r")) {
    char line[512];
    while (fgets(line, sizeof line, m)) {
      unsigned long long lo = 0, hi = 0;
      char perms[8] = {0}, path[384] = {0};
      if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %383[^\n]", &lo, &hi, perms, path) >= 3 && perms[2] == 'x' &&
          path[0]) {
        char *p = path;
        while (*p == ' ') {
          p++;
        }
        if (*p) {
          fprintf(f, "# map %llx %llx %s\n", lo, hi, p);
        }
      }
    }
    fclose(m);
  }
  fclose(f);
}
