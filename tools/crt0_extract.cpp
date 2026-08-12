// crt0_extract — report a PSX executable's crt0 boot group, using THE SHIPPING DECODER.
//
// WHY THIS IS A C++ TOOL AND NOT A PYTHON SCRIPT. Every value it prints becomes a constant in some
// game's `game_config.cpp`, and `crt0_audit` (runtime/recomp/crt0_verify.h) re-derives those same
// values from the guest's own instruction stream at boot and REFUSES a disagreement. If this tool had
// its own MIPS decoder, the two would drift, and the drift would present as the audit refusing a boot
// over a constant this tool had just "measured" — the gate blaming the game for the tool's bug. So it
// calls `crt0_scan` directly: one decoder, shared between the extractor and the gate that checks it.
//
// WHAT A NEGATIVE PRINTS. Never "(nothing found)". A refusal names what it could not do and exits
// non-zero; a partial scan prints its DENOMINATOR (instructions decoded, why it stopped, how many words
// in the window were zero) next to every field it did and did not resolve. A field this tool cannot see
// must reach `game_config.cpp` as 0 with a TODO, never as a guess — zero is honest, and `crt0_plan`
// distinguishes ABSENT (a measured "this crt0 has no such global") from UNSET (nobody has RE'd it) by
// the explicit `declared` flag, not by the value.
//
//   crt0_extract <EXECUTABLE> [--entry 0xADDR]
//
// Exit 0 = scanned and reported · 1 = scanned but the boot group is incomplete (fields unresolved) ·
// 2 = REFUSED, nothing was scanned (bad magic, short file, entry outside the mapped image).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include "../runtime/recomp/crt0_verify.h"

// A PS-X EXE is a 0x800-byte header followed by the text image, loaded at t_addr.
struct PsxExe {
  uint32_t pc0 = 0, gp0 = 0, tAddr = 0, tSize = 0, spAddr = 0;
  std::vector<uint8_t> text;
  uint32_t r32(uint32_t a) const {
    if (a < tAddr || a + 4 > tAddr + (uint32_t)text.size()) return 0;
    const uint8_t* p = text.data() + (a - tAddr);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
};

static uint32_t rd32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char** argv) {
  const char* path = nullptr;
  uint32_t entryOverride = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--entry") && i + 1 < argc) entryOverride = (uint32_t)strtoul(argv[++i], nullptr, 0);
    else if (!path) path = argv[i];
  }
  if (!path) {
    fprintf(stderr, "crt0_extract: REFUSING — no executable given. Nothing was scanned.\n"
                    "  usage: crt0_extract <EXECUTABLE> [--entry 0xADDR]\n");
    return 2;
  }

  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "crt0_extract: REFUSING — cannot open %s. Nothing was scanned.\n", path); return 2; }
  std::vector<uint8_t> buf;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n > 0) { buf.resize((size_t)n); if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) buf.clear(); }
  fclose(f);
  if (buf.size() < 0x800) {
    fprintf(stderr, "crt0_extract: REFUSING — %s is %zu byte(s), shorter than the 0x800 PS-X EXE header.\n"
                    "  Nothing was scanned.\n", path, buf.size());
    return 2;
  }
  if (memcmp(buf.data(), "PS-X EXE", 8) != 0) {
    fprintf(stderr, "crt0_extract: REFUSING — %s does not begin with the PS-X EXE magic (got %.8s).\n"
                    "  This tool reports a boot group from a LOADED image; a packed or headerless file\n"
                    "  would be decoded at the wrong addresses and every constant would be wrong.\n"
                    "  Nothing was scanned.\n", path, (const char*)buf.data());
    return 2;
  }

  PsxExe exe;
  exe.pc0    = rd32le(buf.data() + 0x10);
  exe.gp0    = rd32le(buf.data() + 0x14);
  exe.tAddr  = rd32le(buf.data() + 0x18);
  exe.tSize  = rd32le(buf.data() + 0x1C);
  exe.spAddr = rd32le(buf.data() + 0x30);
  size_t avail = buf.size() - 0x800;
  size_t want  = exe.tSize <= avail ? exe.tSize : avail;
  exe.text.assign(buf.begin() + 0x800, buf.begin() + 0x800 + want);

  const uint32_t entry = entryOverride ? entryOverride : exe.pc0;
  printf("crt0_extract %s\n", path);
  printf("  PS-X EXE header: pc0=0x%08X gp0=0x%08X text=0x%08X..0x%08X (0x%X byte(s), 0x%zX present)"
         " header sp=0x%08X\n",
         exe.pc0, exe.gp0, exe.tAddr, exe.tAddr + exe.tSize, exe.tSize, exe.text.size(), exe.spAddr);
  if (want != exe.tSize)
    printf("  NOTE: the header declares 0x%X text byte(s) but only 0x%zX are present in the file; words\n"
           "        past the end read as 0 and are counted in `allZero` below.\n", exe.tSize, want);
  if (entry < exe.tAddr || entry >= exe.tAddr + (uint32_t)exe.text.size()) {
    fprintf(stderr, "crt0_extract: REFUSING — the entry 0x%08X is OUTSIDE the mapped image "
                    "0x%08X..0x%08X.\n  Every word would read as 0 and the scan would report a boot group "
                    "of zeroes as if it had\n  measured one. Nothing was scanned.\n",
            entry, exe.tAddr, exe.tAddr + (uint32_t)exe.text.size());
    return 2;
  }

  const Crt0Observed o = crt0_scan(entry, [&exe](uint32_t a) { return exe.r32(a); });

  // The denominator FIRST, so no field below can be read as more certain than the scan that produced it.
  printf("  scan from 0x%08X: %d instruction(s) decoded, stopped on \"%s\", %d zero word(s) in the "
         "window, prologue %s\n",
         entry, o.decoded, o.stopped, o.allZero, o.scanComplete ? "COMPLETE (reached the jal)"
                                                                : "INCOMPLETE (never reached the jal)");

  struct Row { const char* name; bool have; uint32_t val; bool isSigned; };
  const Row rows[] = {
    {"bssZeroLo",     o.haveBssLo,     o.bssLo,         false},
    {"bssZeroHi",     o.haveBssHi,     o.bssHi,         false},
    {"stackTopBase",  o.haveStackTop,  o.stackTopBase,  false},
    {"stackBias",     o.haveBias,      (uint32_t)o.bias, true},
    {"stackTopBase2", o.haveReserve,   o.stackTopBase2, false},
    {"heapBase",      o.haveHeapBase,  o.heapBase,      false},
    {"gp",            o.haveGp,        o.gp,            false},
    {"libcInit",      o.haveLibcInit,  o.libcInit,      false},
  };
  int resolved = 0;
  const int total = (int)(sizeof(rows) / sizeof(rows[0]));
  puts("  boot group:");
  for (const Row& r : rows) {
    if (r.have) resolved++;
    if (!r.have)          printf("    %-14s UNRESOLVED — the scan did not establish this\n", r.name);
    else if (r.isSigned)  printf("    %-14s %d\n", r.name, (int32_t)r.val);
    else                  printf("    %-14s 0x%08X\n", r.name, r.val);
  }
  printf("    %d of %d field(s) resolved\n", resolved, total);

  // The two OPTIONAL stores. "no store" is only a conclusion when the whole prologue was seen — that is
  // the difference between "this crt0 has no heap-size global" and "I stopped before I could tell".
  puts("  optional absolute stores (0 = ABSENT: this crt0 keeps the value in a register only):");
  if (!o.scanComplete)
    puts("    UNKNOWN for both — the scan never reached the jal, so an absent store is indistinguishable\n"
         "    from a store this scan did not reach. Do NOT record 0 for these from this run.");
  else {
    if (o.sawHeapSizeStore) printf("    heapSizePtr    0x%08X\n", o.heapSizePtr);
    else puts("    heapSizePtr    0   (ABSENT — no absolute store in a COMPLETE prologue)");
    if (o.sawHeapBaseStore) printf("    heapBasePtr    0x%08X\n", o.heapBasePtr);
    else puts("    heapBasePtr    0   (ABSENT — no absolute store in a COMPLETE prologue)");
  }
  printf("  libcInit is the A(39h) InitHeap BIOS thunk: %s · a1 live at the call: %s · delay slot is "
         "addi a0,a0,4: %s\n",
         o.libcInitIsInitHeap ? "YES" : "no", o.a1LiveAtCall ? "YES" : "no", o.a0PlusFour ? "YES" : "no");

  // The paste-ready declaration. Emitted only for what was RESOLVED; an unresolved field is emitted as
  // a TODO rather than a zero that would read as a measurement.
  puts("  --- for game_config.cpp (append at the END of the initialiser; GameConfig is positional) ---");
  if (o.haveBias)
    printf("    cfg.stackBias = {1, %d};   // measured by tools/crt0_extract from %s\n",
           (int32_t)o.bias, path);
  else
    puts("    // TODO stackBias: UNRESOLVED by crt0_extract — do NOT declare it until measured.\n"
         "    //   Leaving `declared` at 0 makes crt0_plan REFUSE the boot, which is the correct\n"
         "    //   outcome: a guessed bias moves sp and the heap length silently.");

  if (!o.scanComplete || resolved != total) {
    fprintf(stderr, "crt0_extract: the boot group is INCOMPLETE (%d of %d field(s) resolved, prologue %s).\n"
                    "  Exiting 1 so a script cannot treat a partial scan as a full one.\n",
            resolved, total, o.scanComplete ? "complete" : "INCOMPLETE");
    return 1;
  }
  return 0;
}
