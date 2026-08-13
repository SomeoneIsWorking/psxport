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
// ═══ THE REFUSALS ARE GATED, BY `--selftest` ════════════════════════════════════════════════════════
// Every refusal below is the reason a wrong constant does NOT reach a game_config.cpp, so a refusal that
// silently stopped refusing would be the worst failure this tool has: it would print a boot group of
// zeroes, read off an image it never decoded, in the tool's normal confident format. Until 2026-08-12
// nothing checked them — they had been confirmed by hand once, which gates nothing thereafter.
//
// `crt0_extract --selftest` now drives THE SHIPPING PATH (`extract_from_image`, the same function
// `main` calls) over synthesised images: two known-good crt0s whose every reported field is compared
// against the spec that assembled them, plus one image per refusal, plus a zeroed prologue that must
// come back INCOMPLETE rather than as eight measured zeroes. The bytes come from
// `tests/crt0_fixture.h` — the same encoder `tests/test_crt0_guest_audit.cpp` uses to gate the
// boot-time audit, so the extractor and the audit cannot pass against streams the other would not
// recognise. It exits 0 only if every check ran and passed, and prints its own blind spots.
//
//   crt0_extract <EXECUTABLE> [--entry 0xADDR]
//   crt0_extract --selftest
//
// Exit 0 = scanned and reported (or the selftest passed) · 1 = scanned but the boot group is incomplete
// (fields unresolved), or a selftest check failed · 2 = REFUSED, nothing was scanned (bad magic, short
// file, entry outside the mapped image).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <map>
#include <vector>
#include "../runtime/recomp/crt0_verify.h"
#include "../tests/crt0_fixture.h"

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

// WHICH refusal fired, so `--selftest` can assert the tool refused for the RIGHT reason. Asserting only
// "exit was nonzero" would let any refusal stand in for any other — including a short-file check that
// happened to catch a bad-magic image and left the magic check dead.
enum Crt0xRefusal { XR_NONE = 0, XR_SHORT_FILE, XR_BAD_MAGIC, XR_ENTRY_OUTSIDE };

struct Crt0xOutcome {
  int          exitCode = 0;
  Crt0xRefusal refusal  = XR_NONE;
  bool         scanned  = false;      // false whenever a refusal fired: NOTHING was decoded
  uint32_t     entry    = 0;
  int          resolved = 0, total = 0;
  Crt0Observed o;

  // What the SHIPPING arithmetic makes of the scan: `crt0_plan` from runtime/recomp/crt0_boot.h, run on
  // this image's own words. Carried out of `extract_from_image` so `--selftest` asserts on the same values
  // the human report prints, and so `tools/oracle/crossvalidate_crt0.py` can diff a1 (the heap SIZE)
  // against what the oracle MEASURED at the InitHeap call. Before this existed, a1 was the one field the
  // cross-check could not see — and it is the field that was actually wrong (every port passed size 0).
  bool     planOk = false;
  Crt0Plan plan;
};

// extract_from_image — THE shipping path. `main` and `--selftest` both call this and nothing else, so
// the selftest cannot pass against a private copy of the logic. `report` may be null to suppress the
// human report; REFUSALS always print to stderr regardless, because a silent refusal is the failure
// mode this whole tool exists to avoid.
static Crt0xOutcome extract_from_image(const uint8_t* data, size_t n, const char* label,
                                       uint32_t entryOverride, FILE* report) {
  Crt0xOutcome r;
  if (n < 0x800) {
    fprintf(stderr, "crt0_extract: REFUSING — %s is %zu byte(s), shorter than the 0x800 PS-X EXE header.\n"
                    "  Nothing was scanned.\n", label, n);
    r.exitCode = 2; r.refusal = XR_SHORT_FILE; return r;
  }
  if (memcmp(data, "PS-X EXE", 8) != 0) {
    fprintf(stderr, "crt0_extract: REFUSING — %s does not begin with the PS-X EXE magic (got %.8s).\n"
                    "  This tool reports a boot group from a LOADED image; a packed or headerless file\n"
                    "  would be decoded at the wrong addresses and every constant would be wrong.\n"
                    "  Nothing was scanned.\n", label, (const char*)data);
    r.exitCode = 2; r.refusal = XR_BAD_MAGIC; return r;
  }

  PsxExe exe;
  exe.pc0    = rd32le(data + 0x10);
  exe.gp0    = rd32le(data + 0x14);
  exe.tAddr  = rd32le(data + 0x18);
  exe.tSize  = rd32le(data + 0x1C);
  exe.spAddr = rd32le(data + 0x30);
  size_t avail = n - 0x800;
  size_t want  = exe.tSize <= avail ? exe.tSize : avail;
  exe.text.assign(data + 0x800, data + 0x800 + want);

  const uint32_t entry = entryOverride ? entryOverride : exe.pc0;
  r.entry = entry;
  if (report) {
    fprintf(report, "crt0_extract %s\n", label);
    fprintf(report, "  PS-X EXE header: pc0=0x%08X gp0=0x%08X text=0x%08X..0x%08X (0x%X byte(s), 0x%zX"
                    " present) header sp=0x%08X\n",
            exe.pc0, exe.gp0, exe.tAddr, exe.tAddr + exe.tSize, exe.tSize, exe.text.size(), exe.spAddr);
    if (want != exe.tSize)
      fprintf(report, "  NOTE: the header declares 0x%X text byte(s) but only 0x%zX are present in the"
                      " file; words\n        past the end read as 0 and are counted in `allZero` below.\n",
              exe.tSize, want);
  }
  if (entry < exe.tAddr || entry >= exe.tAddr + (uint32_t)exe.text.size()) {
    fprintf(stderr, "crt0_extract: REFUSING — the entry 0x%08X is OUTSIDE the mapped image "
                    "0x%08X..0x%08X.\n  Every word would read as 0 and the scan would report a boot group "
                    "of zeroes as if it had\n  measured one. Nothing was scanned.\n",
            entry, exe.tAddr, exe.tAddr + (uint32_t)exe.text.size());
    r.exitCode = 2; r.refusal = XR_ENTRY_OUTSIDE; return r;
  }

  r.o = crt0_scan(entry, [&exe](uint32_t a) { return exe.r32(a); });
  r.scanned = true;
  const Crt0Observed& o = r.o;

  // The denominator FIRST, so no field below can be read as more certain than the scan that produced it.
  if (report)
    fprintf(report, "  scan from 0x%08X: %d instruction(s) decoded, stopped on \"%s\", %d zero word(s) in"
                    " the window, prologue %s\n",
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
  r.total = (int)(sizeof(rows) / sizeof(rows[0]));
  if (report) fputs("  boot group:\n", report);
  for (const Row& row : rows) {
    if (row.have) r.resolved++;
    if (!report)               continue;
    if (!row.have)             fprintf(report, "    %-14s UNRESOLVED — the scan did not establish this\n", row.name);
    else if (row.isSigned)     fprintf(report, "    %-14s %d\n", row.name, (int32_t)row.val);
    else                       fprintf(report, "    %-14s 0x%08X\n", row.name, row.val);
  }
  if (report) fprintf(report, "    %d of %d field(s) resolved\n", r.resolved, r.total);

  // ── what the SHIPPING arithmetic computes from this scan ────────────────────────────────────────
  // `crt0_plan` is called, not reimplemented: the extractor and the boot path must not be able to disagree
  // about the InitHeap arguments, for the same reason they already share `crt0_scan`. It is pure — the two
  // words the guest crt0 loads are passed in — so it can run here against the image's own bytes.
  if (r.resolved == r.total && o.scanComplete) {
    GameConfig gc{};
    gc.bssZeroLo     = o.bssLo;
    gc.bssZeroHi     = o.bssHi;
    gc.stackTopBase  = o.stackTopBase;
    gc.stackTopBase2 = o.stackTopBase2;
    gc.heapBase      = o.heapBase;
    gc.heapSizePtr   = o.sawHeapSizeStore ? o.heapSizePtr : 0u;
    gc.heapBasePtr   = o.sawHeapBaseStore ? o.heapBasePtr : 0u;
    gc.gp            = o.gp;
    gc.libcInit      = o.libcInit;
    gc.stackBias     = {1u, o.bias};
    const uint32_t stackTopWord    = exe.r32(o.stackTopBase);
    const uint32_t stackReserveWord = exe.r32(o.stackTopBase2);
    r.plan   = crt0_plan(&gc, stackTopWord, stackReserveWord, "crt0_extract");
    r.planOk = r.plan.ok;
    if (report) {
      if (r.planOk)
        fprintf(report, "  crt0_plan (THE shipping arithmetic, run on this image): sp=0x%08X gp=0x%08X"
                        " InitHeap(a0=0x%08X, a1=0x%X)\n"
                        "    a1 = ((mem[0x%08X]=0x%08X %c %d) - mem[0x%08X]=0x%08X) - 0x%08X = %u byte(s)\n",
                r.plan.sp, r.plan.gp, r.plan.a0, r.plan.a1,
                o.stackTopBase, stackTopWord, o.bias < 0 ? '-' : '+',
                o.bias < 0 ? -(int)o.bias : (int)o.bias,
                o.stackTopBase2, stackReserveWord, r.plan.heapBaseMasked, r.plan.a1);
      else
        fputs("  crt0_plan REFUSED on this image's own words — see its error above. The boot group scanned"
              " cleanly\n    but the arithmetic over it does not produce a usable plan, so nothing here"
              " should be shipped.\n", report);
    }
  } else if (report) {
    fprintf(report, "  crt0_plan NOT RUN: %d of %d field(s) resolved and the prologue is %s. Running it on"
                    " an\n    incomplete group would print an InitHeap size derived from zeroes.\n",
            r.resolved, r.total, o.scanComplete ? "complete" : "INCOMPLETE");
  }

  // The two OPTIONAL stores. "no store" is only a conclusion when the whole prologue was seen — that is
  // the difference between "this crt0 has no heap-size global" and "I stopped before I could tell".
  if (report) {
    fputs("  optional absolute stores (0 = ABSENT: this crt0 keeps the value in a register only):\n", report);
    if (!o.scanComplete)
      fputs("    UNKNOWN for both — the scan never reached the jal, so an absent store is indistinguishable\n"
            "    from a store this scan did not reach. Do NOT record 0 for these from this run.\n", report);
    else {
      if (o.sawHeapSizeStore) fprintf(report, "    heapSizePtr    0x%08X\n", o.heapSizePtr);
      else fputs("    heapSizePtr    0   (ABSENT — no absolute store in a COMPLETE prologue)\n", report);
      if (o.sawHeapBaseStore) fprintf(report, "    heapBasePtr    0x%08X\n", o.heapBasePtr);
      else fputs("    heapBasePtr    0   (ABSENT — no absolute store in a COMPLETE prologue)\n", report);
    }
    fprintf(report, "  libcInit is the A(39h) InitHeap BIOS thunk: %s · a1 live at the call: %s · delay"
                    " slot is addi a0,a0,4: %s\n",
            o.libcInitIsInitHeap ? "YES" : "no", o.a1LiveAtCall ? "YES" : "no", o.a0PlusFour ? "YES" : "no");

    // The paste-ready declaration. Emitted only for what was RESOLVED; an unresolved field is emitted as
    // a TODO rather than a zero that would read as a measurement.
    fputs("  --- for game_config.cpp (append at the END of the initialiser; GameConfig is positional) ---\n", report);
    if (o.haveBias)
      fprintf(report, "    cfg.stackBias = {1, %d};   // measured by tools/crt0_extract from %s\n",
              (int32_t)o.bias, label);
    else
      fputs("    // TODO stackBias: UNRESOLVED by crt0_extract — do NOT declare it until measured.\n"
            "    //   Leaving `declared` at 0 makes crt0_plan REFUSE the boot, which is the correct\n"
            "    //   outcome: a guessed bias moves sp and the heap length silently.\n", report);
  }

  if (!o.scanComplete || r.resolved != r.total) {
    fprintf(stderr, "crt0_extract: the boot group is INCOMPLETE (%d of %d field(s) resolved, prologue %s).\n"
                    "  Exiting 1 so a script cannot treat a partial scan as a full one.\n",
            r.resolved, r.total, o.scanComplete ? "complete" : "INCOMPLETE");
    r.exitCode = 1;
  }
  return r;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════════
// --selftest
// ══════════════════════════════════════════════════════════════════════════════════════════════════════
static int g_ran = 0, g_failed = 0;
static void sx(bool ok, const char* what) {
  g_ran++;
  if (!ok) g_failed++;
  fprintf(stderr, "  [%d] %-58s ... %s\n", g_ran, what, ok ? "PASS" : "FAIL");
}
static void sx_eq32(uint32_t got, uint32_t want, const char* what) {
  const bool ok = got == want;
  g_ran++;
  if (!ok) g_failed++;
  if (ok) fprintf(stderr, "  [%d] %-58s ... PASS (0x%08X)\n", g_ran, what, got);
  else    fprintf(stderr, "  [%d] %-58s ... FAIL got 0x%08X want 0x%08X\n", g_ran, what, got, want);
}

// A PS-X EXE image whose text is exactly the fixture's assembled prologue plus the InitHeap thunk it
// calls. Built from the SAME emitter the audit test uses, so neither gate can pass against bytes the
// other would not recognise.
static std::vector<uint8_t> image_from_template(const Crt0Template& t) {
  std::map<uint32_t, uint32_t> words;
  crt0_fixture_emit(t, [&words](uint32_t a, uint32_t w) { words[a] = w; });
  const uint32_t lo = words.begin()->first;
  const uint32_t hi = words.rbegin()->first + 4u;
  const uint32_t tAddr = lo, tSize = hi - lo;

  std::vector<uint8_t> img(0x800u + tSize, 0u);
  memcpy(img.data(), "PS-X EXE", 8);
  auto put = [&img](size_t off, uint32_t v) {
    img[off] = (uint8_t)(v & 0xFF);         img[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    img[off + 2] = (uint8_t)((v >> 16) & 0xFF); img[off + 3] = (uint8_t)((v >> 24) & 0xFF);
  };
  put(0x10, t.entry);   // pc0
  put(0x14, t.gp);      // gp0
  put(0x18, tAddr);
  put(0x1C, tSize);
  put(0x30, 0x801FFF00u);
  for (const auto& kv : words) put(0x800u + (kv.first - tAddr), kv.second);
  return img;
}

// crt0_plan on a FIXTURE image must REFUSE, and that is asserted rather than left silent.
//
// `crt0_fixture_emit` writes the prologue and the InitHeap thunk — code only, no DATA. So the words at
// `stackTopBase` / `stackTopBase2` read as 0 in a fixture image, the heap-size subtraction underflows, and
// `crt0_plan` correctly refuses (`hsz >= CRT0_MAX_PLAUSIBLE_HEAP`). Asserting that refusal is what stops
// the plan block from being a code path nothing ever checks: if it started silently succeeding on zeroed
// words, it would be printing an InitHeap capacity derived from nothing and this would catch it.
//
// The POSITIVE side of the plan block is exercised where the data actually exists — on real executables,
// by `tools/oracle/crossvalidate_crt0.py`, which diffs its `sp`/`a0`/`a1` against what the reference
// emulator MEASURED at the InitHeap call across 7 images. That is stronger evidence than a fixture could
// give, because a fixture's expected value would be computed by this same arithmetic.
static void selftest_plan_refuses_on_fixture(const Crt0xOutcome& r) {
  sx(!r.planOk, "crt0_plan REFUSED on a fixture image (its stack-top words are 0: code only, no data)");
}

// One known-good shape: every field the tool REPORTS is compared against the spec that assembled the
// bytes. This is the check that makes the tool's output trustworthy as a source of shipped constants —
// "it exited 0" would not.
static void selftest_shape(const char* name, const Crt0Template& t, bool expectHeapGlobals) {
  fprintf(stderr, "\n-- shape %s (bias %d, heap globals %s, %s stack-top load) --\n",
          name, t.bias, t.heapSizePtr || t.heapBasePtr ? "present" : "ABSENT",
          t.indirectStackTop ? "INDIRECT" : "direct");
  const std::vector<uint8_t> img = image_from_template(t);
  const Crt0xOutcome r = extract_from_image(img.data(), img.size(), name, 0, stderr);

  sx(r.exitCode == 0, "exit 0 (a complete, fully resolved boot group)");
  sx(r.scanned, "the image was actually scanned");
  sx(r.o.scanComplete, "prologue COMPLETE (the scan reached the jal)");
  sx(r.resolved == r.total && r.total == 8, "8 of 8 fields resolved");
  sx_eq32(r.o.bssLo, t.bssLo, "bssZeroLo == the spec");
  sx_eq32(r.o.bssHi, t.bssHi, "bssZeroHi == the spec");
  sx_eq32(r.o.stackTopBase, t.stackTopBase, "stackTopBase == the spec");
  sx_eq32(r.o.stackTopBase2, t.stackTopBase2, "stackTopBase2 == the spec");
  sx_eq32(r.o.heapBase, t.heapBase, "heapBase == the spec");
  sx_eq32(r.o.gp, t.gp, "gp == the spec");
  sx_eq32(r.o.libcInit, t.libcInit, "libcInit == the spec");
  // The bias is the field the pre-fix framework got wrong for two of six games, and 0 is a REAL answer
  // (Mega Man X4 / Toy Story 2 have no bias instruction at all), so `haveBias` is asserted separately
  // from the value: "resolved as 0" and "never resolved" must not look alike here either.
  sx(r.o.haveBias, "stackBias RESOLVED (not merely defaulted to 0)");
  sx(r.o.bias == t.bias, "stackBias == the spec");
  sx(r.o.libcInitIsInitHeap, "libcInit recognised as the A(39h) InitHeap thunk");
  sx(r.o.a1LiveAtCall, "a1 (InitHeap's size) live at the guest's own jal");
  sx(r.o.a0PlusFour, "the jal's delay slot is addi a0,a0,4");
  selftest_plan_refuses_on_fixture(r);
  // ABSENT vs PRESENT for the two optional stores — the distinction that stopped the ports writing to
  // guest address 0.
  sx(r.o.sawHeapSizeStore == expectHeapGlobals, "heapSizePtr store PRESENT/ABSENT matches the spec");
  sx(r.o.sawHeapBaseStore == expectHeapGlobals, "heapBasePtr store PRESENT/ABSENT matches the spec");
  if (expectHeapGlobals) {
    sx_eq32(r.o.heapSizePtr, t.heapSizePtr, "heapSizePtr == the spec");
    sx_eq32(r.o.heapBasePtr, t.heapBasePtr, "heapBasePtr == the spec");
  }
}

static void selftest_refusal(const char* what, const std::vector<uint8_t>& img, uint32_t entryOverride,
                             Crt0xRefusal expect) {
  fprintf(stderr, "\n-- refusal: %s (the refusal text below is the EXPECTED output) --\n", what);
  const Crt0xOutcome r = extract_from_image(img.data(), img.size(), what, entryOverride, nullptr);
  sx(r.exitCode == 2, "exit 2 (REFUSED)");
  sx(r.refusal == expect, "refused for the RIGHT reason");
  sx(!r.scanned, "nothing was scanned");
  sx(r.resolved == 0, "no field was reported");
}

static int selftest(void) {
  fputs("crt0_extract --selftest: driving extract_from_image (THE path `main` calls) over synthesised\n"
        "  images. PLAN: 1 encoder-pin sweep · 2 known-good crt0 shapes, every reported field compared to\n"
        "  the spec that assembled the bytes · 3 refusals, each asserted to fire for its OWN reason ·\n"
        "  1 zeroed prologue that must come back INCOMPLETE rather than as eight measured zeroes.\n", stderr);

  // FIRST: the encoder. If it drifts, every image below is an instruction stream no PSX would execute and
  // the shape checks would be comparing a decoder against nothing.
  const std::vector<Crt0PinRow> pins = crt0_fixture_pins();
  fprintf(stderr, "\n-- encoder pins: %zu row(s) vs bytes disassembled from real crt0s (floor %d) --\n",
          pins.size(), CRT0_FIXTURE_PIN_FLOOR);
  sx(pins.size() >= (size_t)CRT0_FIXTURE_PIN_FLOOR, "the pin list is present and at least the floor");
  int badPins = 0;
  for (const Crt0PinRow& p : pins) {
    if (p.got == p.want) continue;
    badPins++;
    fprintf(stderr, "  [pins] MISMATCH %s: encoder produced 0x%08X, the executable contains 0x%08X\n",
            p.what, p.got, p.want);
  }
  fprintf(stderr, "  [pins] %zu row(s) checked, %d mismatch(es)\n", pins.size(), badPins);
  sx(badPins == 0, "every pinned instruction encodes to the measured word");

  selftest_shape("fixture:psyq-tomba2", crt0_fixture_psyq_tomba2(), true);
  selftest_shape("fixture:psyq-mmx4",   crt0_fixture_psyq_mmx4(),   false);

  // The three refusals, each from an image that trips ONLY it.
  selftest_refusal("a 4-byte file", std::vector<uint8_t>(4, 0), 0, XR_SHORT_FILE);
  std::vector<uint8_t> notAnExe(0x900, 0u);
  memcpy(notAnExe.data(), "NOT-AN-E", 8);
  selftest_refusal("0x900 bytes with the wrong magic", notAnExe, 0, XR_BAD_MAGIC);
  const std::vector<uint8_t> good = image_from_template(crt0_fixture_psyq_tomba2());
  selftest_refusal("--entry outside the mapped image", good, 0x1000u, XR_ENTRY_OUTSIDE);

  // THE NEGATIVE THAT MUST NOT LOOK LIKE A MEASUREMENT: a valid image whose prologue is all zeroes. The
  // tool must come back INCOMPLETE with fields unresolved, never with a boot group of zeroes.
  fprintf(stderr, "\n-- a valid image whose prologue is ZEROED (the dangerous negative) --\n");
  std::vector<uint8_t> zeroed = good;
  for (size_t i = 0x800; i < zeroed.size(); i++) zeroed[i] = 0;
  const Crt0xOutcome z = extract_from_image(zeroed.data(), zeroed.size(), "zeroed-prologue", 0, nullptr);
  sx(z.exitCode == 1, "exit 1 (scanned, but the boot group is INCOMPLETE)");
  sx(z.scanned, "it did scan (this is not a refusal)");
  sx(!z.o.scanComplete, "prologue reported INCOMPLETE");
  sx(z.resolved < z.total, "fields left UNRESOLVED rather than reported as 0");
  sx(z.o.allZero > 0, "the zero-word count in the window is reported non-zero");

  fprintf(stderr, "\ncrt0_extract --selftest: %s — %d check(s) ran, %d failed.\n",
          g_failed ? "FAIL" : "PASS", g_ran, g_failed);
  fputs("what this selftest does NOT cover: it never opens a file (the fopen refusal and the\n"
        "  header-declares-more-text-than-present NOTE are exercised only by a real run), the images are\n"
        "  assembled from tests/crt0_fixture.h rather than read out of a commercial executable, so a real\n"
        "  crt0 shape outside the five measured ones is invisible to it, and it says nothing about\n"
        "  whether any game's game_config.cpp actually carries the values this tool printed — that is\n"
        "  crt0_audit's job, at every boot.\n", stderr);
  return g_failed ? 1 : 0;
}

int main(int argc, char** argv) {
  const char* path = nullptr;
  uint32_t entryOverride = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--selftest")) return selftest();
    else if (!strcmp(argv[i], "--entry") && i + 1 < argc) entryOverride = (uint32_t)strtoul(argv[++i], nullptr, 0);
    else if (!path) path = argv[i];
  }
  if (!path) {
    fprintf(stderr, "crt0_extract: REFUSING — no executable given. Nothing was scanned.\n"
                    "  usage: crt0_extract <EXECUTABLE> [--entry 0xADDR]\n"
                    "         crt0_extract --selftest\n");
    return 2;
  }

  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "crt0_extract: REFUSING — cannot open %s. Nothing was scanned.\n", path); return 2; }
  std::vector<uint8_t> buf;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n > 0) { buf.resize((size_t)n); if (fread(buf.data(), 1, (size_t)n, f) != (size_t)n) buf.clear(); }
  fclose(f);

  return extract_from_image(buf.data(), buf.size(), path, entryOverride, stdout).exitCode;
}
