// crt0_extract — report a PSX executable's crt0 boot group, using THE SHIPPING DECODER.
//
// WHY THIS IS A C++ TOOL AND NOT A PYTHON SCRIPT. Every value it prints becomes a constant in some
// game's derived runtime, and `psx::game::auditCrt0` (runtime/psx/boot/crt0_verify.h) re-derives those same
// values from the guest's own instruction stream at boot and REFUSES a disagreement. If this tool had
// its own MIPS decoder, the two would drift, and the drift would present as the audit refusing a boot
// over a constant this tool had just "measured" — the gate blaming the game for the tool's bug. So it
// calls `psx::game::scanCrt0` directly: one decoder, shared between the extractor and the gate that checks it.
//
// WHAT A NEGATIVE PRINTS. Never "(nothing found)". A refusal names what it could not do and exits
// non-zero; a partial scan prints its DENOMINATOR (instructions decoded, why it stopped, how many words
// in the window were zero) next to every field it did and did not resolve. A field this tool cannot see
// must reach `Crt0Image` as 0 with a TODO, never as a guess — zero is honest, and `psx::game::planCrt0`
// distinguishes ABSENT (a measured "this crt0 has no such global") from UNSET (nobody has RE'd it) by
// the explicit `declared` flag, not by the value.
//
//   crt0_extract <EXECUTABLE> [--entry 0xADDR]
//
// Exit 0 = scanned and reported · 1 = scanned but the boot group is incomplete
// (fields unresolved) · 2 = REFUSED, nothing was scanned (bad magic, short
// file, entry outside the mapped image).
#include "crt0_verify.h"
#include <map>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

// A PS-X EXE is a 0x800-byte header followed by the text image, loaded at t_addr.
struct PsxExe {
  uint32_t pc0 = 0, gp0 = 0, tAddr = 0, tSize = 0, spAddr = 0;
  std::vector<uint8_t> text;
  uint32_t r32(uint32_t a) const {
    if (a < tAddr || a + 4 > tAddr + (uint32_t)text.size()) {
      return 0;
    }
    const uint8_t *p = text.data() + (a - tAddr);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
};

uint32_t rd32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// WHICH refusal fired.
enum Crt0xRefusal : std::uint8_t { XR_NONE = 0, XR_SHORT_FILE, XR_BAD_MAGIC, XR_ENTRY_OUTSIDE };

struct Crt0xOutcome {
  int exitCode = 0;
  Crt0xRefusal refusal = XR_NONE;
  bool scanned = false; // false whenever a refusal fired: NOTHING was decoded
  uint32_t entry = 0;
  int resolved = 0, total = 0;
  Crt0Observed o;

  // What the SHIPPING arithmetic makes of the scan: `psx::game::planCrt0` from runtime/psx/game/crt0_boot.h, run on
  // this image's own words. Carried out of `extract_from_image` so the values match
  // the human report, and so `tools/oracle/crossvalidate_crt0.py` can diff a1 (the heap SIZE)
  // against what the oracle MEASURED at the InitHeap call. Before this existed, a1 was the one field the
  // cross-check could not see — and it is the field that was actually wrong (every port passed size 0).
  bool planOk = false;
  Crt0Plan plan;
};

// extract_from_image — THE shipping path, called by `main`. `report` may be null to suppress the
// human report; REFUSALS always print to stderr regardless, because a silent refusal is the failure
// mode this whole tool exists to avoid.
Crt0xOutcome
extract_from_image(const uint8_t *data, size_t n, const char *label, uint32_t entryOverride, FILE *report) {
  Crt0xOutcome r;
  if (n < 0x800) {
    fprintf(stderr,
            "crt0_extract: REFUSING — %s is %zu byte(s), shorter than the 0x800 PS-X EXE header.\n"
            "  Nothing was scanned.\n",
            label,
            n);
    r.exitCode = 2;
    r.refusal = XR_SHORT_FILE;
    return r;
  }
  if (memcmp(data, "PS-X EXE", 8) != 0) {
    fprintf(stderr,
            "crt0_extract: REFUSING — %s does not begin with the PS-X EXE magic (got %.8s).\n"
            "  This tool reports a boot group from a LOADED image; a packed or headerless file\n"
            "  would be decoded at the wrong addresses and every constant would be wrong.\n"
            "  Nothing was scanned.\n",
            label,
            (const char *)data);
    r.exitCode = 2;
    r.refusal = XR_BAD_MAGIC;
    return r;
  }

  PsxExe exe;
  exe.pc0 = rd32le(data + 0x10);
  exe.gp0 = rd32le(data + 0x14);
  exe.tAddr = rd32le(data + 0x18);
  exe.tSize = rd32le(data + 0x1C);
  exe.spAddr = rd32le(data + 0x30);
  size_t avail = n - 0x800;
  size_t want = exe.tSize <= avail ? exe.tSize : avail;
  exe.text.assign(data + 0x800, data + 0x800 + want);

  const uint32_t entry = entryOverride ? entryOverride : exe.pc0;
  r.entry = entry;
  if (report) {
    fprintf(report, "crt0_extract %s\n", label);
    fprintf(report,
            "  PS-X EXE header: pc0=0x%08X gp0=0x%08X text=0x%08X..0x%08X (0x%X byte(s), 0x%zX"
            " present) header sp=0x%08X\n",
            exe.pc0,
            exe.gp0,
            exe.tAddr,
            exe.tAddr + exe.tSize,
            exe.tSize,
            exe.text.size(),
            exe.spAddr);
    if (want != exe.tSize) {
      fprintf(report,
              "  NOTE: the header declares 0x%X text byte(s) but only 0x%zX are present in the"
              " file; words\n        past the end read as 0 and are counted in `allZero` below.\n",
              exe.tSize,
              want);
    }
  }
  if (entry < exe.tAddr || entry >= exe.tAddr + (uint32_t)exe.text.size()) {
    fprintf(stderr,
            "crt0_extract: REFUSING — the entry 0x%08X is OUTSIDE the mapped image "
            "0x%08X..0x%08X.\n  Every word would read as 0 and the scan would report a boot group "
            "of zeroes as if it had\n  measured one. Nothing was scanned.\n",
            entry,
            exe.tAddr,
            exe.tAddr + (uint32_t)exe.text.size());
    r.exitCode = 2;
    r.refusal = XR_ENTRY_OUTSIDE;
    return r;
  }

  r.o = psx::game::scanCrt0(entry, [&exe](uint32_t a) {
    return exe.r32(a);
  });
  r.scanned = true;
  const Crt0Observed &o = r.o;

  // The denominator FIRST, so no field below can be read as more certain than the scan that produced it.
  if (report) {
    fprintf(report,
            "  scan from 0x%08X: %d instruction(s) decoded, stopped on \"%s\", %d zero word(s) in"
            " the window, prologue %s\n",
            entry,
            o.decoded,
            o.stopped,
            o.allZero,
            o.scanComplete ? "COMPLETE (reached the jal)" : "INCOMPLETE (never reached the jal)");
  }

  struct Row {
    const char *name;
    uint32_t val;
    bool have;
    bool isSigned;
  };
  const Row rows[] = {
      {"bssZeroLo", o.bssLo, o.haveBssLo, false},
      {"bssZeroHi", o.bssHi, o.haveBssHi, false},
      {"stackTopBase", o.stackTopBase, o.haveStackTop, false},
      {"stackBias", (uint32_t)o.bias, o.haveBias, true},
      {"stackTopBase2", o.stackTopBase2, o.haveReserve, false},
      {"heapBase", o.heapBase, o.haveHeapBase, false},
      {"gp", o.gp, o.haveGp, false},
      {"libcInit", o.libcInit, o.haveLibcInit, false},
  };
  r.total = (int)(sizeof(rows) / sizeof(rows[0]));
  if (report) {
    fputs("  boot group:\n", report);
  }
  for (const Row &row : rows) {
    if (row.have) {
      r.resolved++;
    }
    if (!report) {
      continue;
    }
    if (!row.have) {
      fprintf(report, "    %-14s UNRESOLVED — the scan did not establish this\n", row.name);
    } else if (row.isSigned) {
      fprintf(report, "    %-14s %d\n", row.name, (int32_t)row.val);
    } else {
      fprintf(report, "    %-14s 0x%08X\n", row.name, row.val);
    }
  }
  if (report) {
    fprintf(report, "    %d of %d field(s) resolved\n", r.resolved, r.total);
  }

  // ── what the SHIPPING arithmetic computes from this scan ────────────────────────────────────────
  // `psx::game::planCrt0` is called, not reimplemented: the extractor and the boot path must not be able to disagree
  // about the InitHeap arguments, for the same reason they already share `psx::game::scanCrt0`. It is pure — the two
  // words the guest crt0 loads are passed in — so it can run here against the image's own bytes.
  if (r.resolved == r.total && o.scanComplete) {
    psx::game::Crt0Image image{};
    image.bss.begin = o.bssLo;
    image.bss.end = o.bssHi;
    image.stackTopWordAddress = o.stackTopBase;
    image.stackReserveWordAddress = o.stackTopBase2;
    image.heapBase = o.heapBase;
    image.heapSizeStoreAddress = o.sawHeapSizeStore ? o.heapSizePtr : 0u;
    image.heapBaseStoreAddress = o.sawHeapBaseStore ? o.heapBasePtr : 0u;
    image.globalPointer = o.gp;
    image.libcInitEntry = o.libcInit;
    image.stackBias = {true, o.bias};
    const uint32_t stackTopWord = exe.r32(o.stackTopBase);
    const uint32_t stackReserveWord = exe.r32(o.stackTopBase2);
    r.plan = psx::game::planCrt0(&image, stackTopWord, stackReserveWord, "crt0_extract");
    r.planOk = r.plan.ok;
    if (report) {
      if (r.planOk) {
        fprintf(report,
                "  crt0_plan (THE shipping arithmetic, run on this image): sp=0x%08X gp=0x%08X"
                " InitHeap(a0=0x%08X, a1=0x%X)\n"
                "    a1 = ((mem[0x%08X]=0x%08X %c %d) - mem[0x%08X]=0x%08X) - 0x%08X = %u byte(s)\n",
                r.plan.sp,
                r.plan.gp,
                r.plan.a0,
                r.plan.a1,
                o.stackTopBase,
                stackTopWord,
                o.bias < 0 ? '-' : '+',
                o.bias < 0 ? -(int)o.bias : (int)o.bias,
                o.stackTopBase2,
                stackReserveWord,
                r.plan.heapBaseMasked,
                r.plan.a1);
      } else {
        fputs("  crt0_plan REFUSED on this image's own words — see its error above. The boot group scanned"
              " cleanly\n    but the arithmetic over it does not produce a usable plan, so nothing here"
              " should be shipped.\n",
              report);
      }
    }
  } else if (report) {
    fprintf(report,
            "  crt0_plan NOT RUN: %d of %d field(s) resolved and the prologue is %s. Running it on"
            " an\n    incomplete group would print an InitHeap size derived from zeroes.\n",
            r.resolved,
            r.total,
            o.scanComplete ? "complete" : "INCOMPLETE");
  }

  // The two OPTIONAL stores. "no store" is only a conclusion when the whole prologue was seen — that is
  // the difference between "this crt0 has no heap-size global" and "I stopped before I could tell".
  if (report) {
    fputs("  optional absolute stores (0 = ABSENT: this crt0 keeps the value in a register only):\n", report);
    if (!o.scanComplete) {
      fputs("    UNKNOWN for both — the scan never reached the jal, so an absent store is indistinguishable\n"
            "    from a store this scan did not reach. Do NOT record 0 for these from this run.\n",
            report);
    } else {
      if (o.sawHeapSizeStore) {
        fprintf(report, "    heapSizePtr    0x%08X\n", o.heapSizePtr);
      } else {
        fputs("    heapSizePtr    0   (ABSENT — no absolute store in a COMPLETE prologue)\n", report);
      }
      if (o.sawHeapBaseStore) {
        fprintf(report, "    heapBasePtr    0x%08X\n", o.heapBasePtr);
      } else {
        fputs("    heapBasePtr    0   (ABSENT — no absolute store in a COMPLETE prologue)\n", report);
      }
    }
    fprintf(report,
            "  libcInit is the A(39h) InitHeap BIOS thunk: %s · a1 live at the call: %s · delay"
            " slot is addi a0,a0,4: %s\n",
            o.libcInitIsInitHeap ? "YES" : "no",
            o.a1LiveAtCall ? "YES" : "no",
            o.a0PlusFour ? "YES" : "no");

    // The paste-ready declaration. Emitted only for what was RESOLVED; an unresolved field is emitted as
    // a TODO rather than a zero that would read as a measurement.
    fputs("  --- for the derived runtime's Crt0Image ---\n", report);
    if (o.haveBias) {
      fprintf(report,
              "    image.stackBias = {true, %d};   // measured by tools/crt0_extract from %s\n",
              (int32_t)o.bias,
              label);
    } else {
      fputs("    // TODO stackBias: UNRESOLVED by crt0_extract — do NOT declare it until measured.\n"
            "    //   Leaving `declared` at 0 makes crt0_plan REFUSE the boot, which is the correct\n"
            "    //   outcome: a guessed bias moves sp and the heap length silently.\n",
            report);
    }
  }

  if (!o.scanComplete || r.resolved != r.total) {
    fprintf(stderr,
            "crt0_extract: the boot group is INCOMPLETE (%d of %d field(s) resolved, prologue %s).\n"
            "  Exiting 1 so a script cannot treat a partial scan as a full one.\n",
            r.resolved,
            r.total,
            o.scanComplete ? "complete" : "INCOMPLETE");
    r.exitCode = 1;
  }
  return r;
}

} // namespace

int main(int argc, char **argv) {
  const char *path = nullptr;
  uint32_t entryOverride = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--entry") && i + 1 < argc) {
      entryOverride = (uint32_t)strtoul(argv[++i], nullptr, 0);
    } else if (!path) {
      path = argv[i];
    }
  }
  if (!path) {
    fprintf(stderr,
            "crt0_extract: REFUSING — no executable given. Nothing was scanned.\n"
            "  usage: crt0_extract <EXECUTABLE> [--entry 0xADDR]\n");
    return 2;
  }

  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "crt0_extract: REFUSING — cannot open %s. Nothing was scanned.\n", path);
    return 2;
  }
  std::vector<uint8_t> buf;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    buf.resize((size_t)n);
    if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) {
      buf.clear();
    }
  }
  fclose(f);

  return extract_from_image(buf.data(), buf.size(), path, entryOverride, stdout).exitCode;
}
