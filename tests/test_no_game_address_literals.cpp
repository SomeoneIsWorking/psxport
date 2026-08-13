// test_no_game_address_literals.cpp — THE GATE that keeps psxport game-agnostic.
//
// WHY THIS EXISTS. psxport is supposed to carry no game code: a game supplies GameConfig +
// GameHooks (runtime/recomp/game_iface.h) plus its recompiled substrate, and the framework reads the
// game's layout out of GameConfig. That rule was decaying silently. runtime/recomp/ot_attr.cpp held
// Tomba!2's packet-pool range as two file-scope constants (0x800BFE68/0x800E7E68), so on spyro and
// spider1 the entire OT attribution table matched nothing and reported no spans — which is
// indistinguishable from "the guest submitted no packets". Nothing failed; nobody could have known.
// A rule nobody can check is a rule that decays, so this is a test rather than a paragraph in a doc.
//
// WHAT IT DOES. It scans the framework's OWN sources (runtime/, common/) for hex literals in LIVE
// CODE that name a particular game's guest memory, compares the hit set against a recorded BASELINE
// of today's debt, and fails on anything NEW. The baseline is SHRINK-ONLY (see "THE RATCHET").
//
// ---------------------------------------------------------------------------------------------
// WHAT COUNTS AS A GAME ADDRESS (is_game_addr() below is the whole definition, and test_classifier
// asserts it in BOTH directions — every rule here has a live assertion, none is comment-only):
//
//   GAME (fails):
//     * 0x80010000-0x801FFFFF — main RAM above the kernel, i.e. a KSEG0 pointer into a particular
//       game's globals/tables/pools/functions. This is the whole game-owned RAM window.
//     * 0xA0010000-0xA01FFFFF — the same addresses through the uncached KSEG1 mirror.
//     * 0x1F800001-0x1F8003FF — a SCRATCHPAD OFFSET. This is the subtlety worth stating outright:
//       the scratchpad ADDRESS RANGE is console-generic, but `0x1F800138` in framework code does not
//       mean "scratchpad" — it means "THIS GAME's current-task pointer, which this game happens to
//       keep in scratchpad". That is game knowledge wearing a console address, so it is a violation.
//       The base 0x1F800000 alone is generic (it names the region, not a field in it) and is allowed.
//
//   CONSOLE (must NOT fail — these are the machine, not the game):
//     * 0x1F801000-0x1F801FFF hardware registers, 0x1F800000 scratchpad base.
//     * The BIOS/kernel region 0x80000000-0x8000FFFF: exception vectors (0x80000080), the A0/B0/C0
//       call tables, kernel scratch. Every PSX title shares these, so they are excluded by range.
//     * Main-RAM size 0x200000, VRAM geometry 1024x512, segment masks 0x1FFFFFFF / 0x80000000 /
//       0xA0000000 / 0xBFC00000, GP0/GP1 command encodings, R3000A opcode masks, PS-EXE header
//       offsets — all outside the ranges above, so no special-casing is needed for them.
//     * Two named exceptions INSIDE the game window, in kConsoleExceptions: 0x80010000, the standard
//       PS-EXE load base, and 0x801FFFF0, the conventional initial stack pointer. Those two values
//       are PS-EXE conventions shared by every title, not one game's objects.
//
//   NOT A VIOLATION AT ALL: comments, RE documentation, and string literals. A comment naming a
//   guest address is how this project records reverse engineering, and the audits this gate came out
//   of deliberately did not count them. Only live code counts, so the scanner strips // and /* */
//   comments and "..."/'...' literals before looking for numbers. Consequence to be aware of: a
//   game address passed as text (a format-string default, a getenv fallback) is INVISIBLE here.
//
//   OUT OF SCOPE: tools/ (offline RE and dev utilities, not linked into a game's binary) and
//   vendor/. Also this file: tests/ is not scanned, which is why the baseline table below can name
//   Tomba addresses without tripping itself.
//
// ---------------------------------------------------------------------------------------------
// THE RATCHET — why the baseline cannot be used to silence a violation.
//
// The repo is full of existing violations (391 of them today), so a test that just fails would stop
// anyone landing anything. Hence a baseline. But a baseline you can append to is worse than no gate,
// so this one is checked in BOTH directions, per (file, value) pair with an exact COUNT:
//
//   found > baselined   -> FAIL "NEW"    : a literal was added. This is the gate doing its job.
//   found < baselined   -> FAIL "STALE"  : someone removed a literal without shrinking the baseline.
//                                          The fix and the baseline edit must land together, which is
//                                          what makes the number monotonically decrease.
//   baselined, found 0  -> FAIL "FIXED"  : delete the row. A fixed violation cannot be re-introduced
//                                          later under cover of an old row.
//
// So the ONLY edit that makes this test green is one that lowers a count, and an edit that RAISES a
// count is a hand-written +1 in this file, in the same diff as the code that needs it — visible to a
// reviewer, next to the rule it is breaking, with no way to be mistaken for incidental churn.
// The total is printed on every run so "going down" is legible progress.
//
// FIXING one means: add (or reuse!) a GameConfig field in runtime/recomp/game_iface.h, have each
// game fill it in game/core/game_config.cpp (spyro/spider1 leave un-RE'd fields 0 WITH a TODO), and
// obey the honest-zero rule — a consumer reading 0 must fail fast or announce its blindness once,
// loudly, naming what it cannot see. runtime/recomp/ot_attr.cpp's pool_range() is the worked example.

#include "testutil.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

/* ---- the classifier -------------------------------------------------------------------------- */

/* Values inside the game window that are PS-EXE conventions rather than one game's data. */
static const uint32_t kConsoleExceptions[] = {
    0x80010000u, /* standard PS-EXE load base                */
    0x801ffff0u, /* conventional initial stack pointer        */
};

static bool is_console_exception(uint32_t v) {
  for (uint32_t e : kConsoleExceptions)
    if (v == e) return true;
  return false;
}

static bool is_game_addr(uint32_t v) {
  if (is_console_exception(v)) return false;
  if (v >= 0x80010000u && v <= 0x801fffffu) return true; /* game-owned main RAM, KSEG0        */
  if (v >= 0xa0010000u && v <= 0xa01fffffu) return true; /* same, uncached mirror             */
  if (v > 0x1f800000u && v <= 0x1f8003ffu) return true;  /* a field in scratchpad = game field */
  return false;
}

/* ---- the scanner ----------------------------------------------------------------------------- */

/* Comments and string/char literals are documentation, not behaviour (see the header comment), so
 * they are blanked out before the number scan. Newlines are preserved so line numbers stay true. */
static std::string strip_comments_and_strings(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const char c = s[i];
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') ++i;
    } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') out += '\n';
        ++i;
      }
      i = (i + 1 < n) ? i + 2 : n;
    } else if (c == '"' || c == '\'') {
      const char q = c;
      ++i;
      while (i < n && s[i] != q) {
        if (s[i] == '\\') ++i;
        if (i < n && s[i] == '\n') out += '\n'; /* unterminated literal: do not eat the file */
        ++i;
      }
      if (i < n) ++i;
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

struct Hit {
  std::string file; /* repo-relative */
  int line;
  uint32_t value;
};

struct Scan {
  std::vector<Hit> hits;
  int files = 0;    /* source files read                   */
  long numbers = 0; /* hex literals examined (denominator) */
  bool corpus_ok = false;
};

static bool is_source(const std::string& f) {
  const char* ext[] = {".c", ".cpp", ".h", ".hpp"};
  for (const char* e : ext) {
    const size_t k = strlen(e);
    if (f.size() > k && f.compare(f.size() - k, k, e) == 0) return true;
  }
  return false;
}

static void scan_text(const std::string& rel, const std::string& text, Scan& sc) {
  const std::string code = strip_comments_and_strings(text);
  int line = 1;
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '\n') {
      ++line;
      continue;
    }
    /* A hex literal, not a suffix of an identifier like `foo0x1`. */
    if (code[i] != '0' || i + 1 >= code.size() || (code[i + 1] != 'x' && code[i + 1] != 'X'))
      continue;
    if (i > 0 && (isalnum((unsigned char)code[i - 1]) || code[i - 1] == '_')) continue;
    size_t j = i + 2, digits = 0;
    uint64_t v = 0;
    while (j < code.size() && isxdigit((unsigned char)code[j])) {
      const char d = code[j];
      v = v * 16 + (uint64_t)(d <= '9' ? d - '0' : (tolower(d) - 'a' + 10));
      ++digits;
      ++j;
    }
    i = j - 1;
    if (digits < 4 || digits > 8) continue; /* <4 digits cannot be a 32-bit guest address */
    ++sc.numbers;
    if (is_game_addr((uint32_t)v)) sc.hits.push_back(Hit{rel, line, (uint32_t)v});
  }
}

static std::string read_file(const std::string& path, bool* ok) {
  FILE* f = fopen(path.c_str(), "rb");
  *ok = false;
  if (!f) return "";
  std::string s;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  *ok = true;
  return s;
}

static void walk(const std::string& abs, const std::string& rel, Scan& sc) {
  DIR* d = opendir(abs.c_str());
  if (!d) return;
  std::vector<std::string> subdirs;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    const std::string name = e->d_name;
    if (name == "." || name == ".." || name == "vendor" || name == "__pycache__") continue;
    const std::string a = abs + "/" + name, r = rel.empty() ? name : rel + "/" + name;
    struct stat st;
    if (stat(a.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      subdirs.push_back(name);
      continue;
    }
    if (!S_ISREG(st.st_mode) || !is_source(name)) continue;
    bool ok = false;
    const std::string text = read_file(a, &ok);
    if (!ok) {
      /* A file we cannot read is a hole in the scan, not a clean result. */
      fprintf(stderr, "    SCAN ERROR: cannot read %s\n", r.c_str());
      continue;
    }
    ++sc.files;
    scan_text(r, text, sc);
  }
  closedir(d);
  for (const std::string& s : subdirs) walk(abs + "/" + s, rel.empty() ? s : rel + "/" + s, sc);
}

/* <this file>/../.. — the psxport checkout under test, not some other copy. tests/CMakeLists.txt
 * globs absolute paths, so __FILE__ is absolute. */
static std::string repo_root(void) {
  std::string f = __FILE__;
  const size_t a = f.rfind('/');
  if (a == std::string::npos) return "";
  const size_t b = f.rfind('/', a - 1);
  if (b == std::string::npos) return "";
  return f.substr(0, b);
}

/* The scanned corpus. Anything the framework static library compiles lives under these. */
static const char* kScanDirs[] = {"runtime", "common"};

static Scan scan_framework(void) {
  Scan sc;
  const std::string root = repo_root();
  if (root.empty()) {
    fprintf(stderr, "    REFUSING: cannot derive repo root from __FILE__=%s\n", __FILE__);
    return sc;
  }
  int dirs_found = 0;
  for (const char* d : kScanDirs) {
    struct stat st;
    const std::string a = root + "/" + d;
    if (stat(a.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      /* Refuse rather than return empty: a scan of a directory that does not exist has searched
       * NOTHING, and "0 violations" out of that would be a lie. */
      fprintf(stderr, "    REFUSING: scan dir does not exist: %s\n", a.c_str());
      continue;
    }
    ++dirs_found;
    walk(a, d, sc);
  }
  sc.corpus_ok = (dirs_found == (int)(sizeof kScanDirs / sizeof kScanDirs[0])) && sc.files > 0;
  return sc;
}

/* ---- THE BASELINE ---------------------------------------------------------------------------- */
/* Today's recorded debt: every (file, game address, occurrences-in-live-code) in the framework as of
 * 2026-08-11. Read the RATCHET section at the top before touching a number. Per-file notes say what
 * the addresses are and why they are not fixed yet; the rows are machine-checked.
 *
 *   sbs.cpp (126)          the side-by-side differential harness: Tomba's OT/packet-pool/task-table/
 *                          scratchpad layout, hardwired region names and watch addresses. Biggest
 *                          single block; needs a GameConfig-driven region table.
 *   pc_scheduler.cpp (62)  Tomba's task/scheduler guest FUNCTION addresses and task-state fields.
 *   selftest.cpp (38)      built-in selftest asserts written against Tomba's RAM map.
 *   native_boot.cpp (35)   boot spine: Tomba's scratchpad fields + init function addresses.
 *   interp.cpp (36)        interpreter trace/watch hooks pinned to Tomba functions.
 *   cd_override.cpp (20)   CD override state pinned to Tomba's CD globals + scratchpad fields.
 *   repl.cpp (16)          debug REPL default watch/inspect targets.
 *   pad_input.cpp (11)     Tomba's pad-state globals.
 *   hle.cpp (8)            HLE entry points identified by Tomba guest address.
 *   dbg_server.cpp (6)     debug-server default watch set.
 *   gpu_vk.cpp (5)         presentation hooks reading Tomba scratchpad fields.
 *   overlay_glue.cpp (4)   overlay bookkeeping at Tomba scratchpad fields.
 *   scheduler.cpp (4)      framework scheduler reading Tomba task state.
 *   dualcore.cpp (3)       dual-core compare window (the packet-pool range literal).
 *   gpu_native.cpp (3)     native GPU path reading Tomba globals.
 *   mem.cpp (3)            memory-map special cases for Tomba addresses.
 *   render_queue.cpp (3)   render queue keyed on Tomba globals.
 *   menu_readouts.cpp (3)  UI readouts of Tomba game state.
 *   scheduler.h (2)        inline accessors of Tomba task state.
 *   overlay_router.cpp (1) one Tomba scratchpad field.
 *   render_node.h (1)      one Tomba scratchpad field.
 *   timing.cpp (1)         one Tomba global.
 *
 * Fields to prefer when fixing: GameConfig already has otRegionBase/otRegionStride, packetPoolBase/
 * packetPoolStride, otBasePtr, poolPtrCur/poolPtrLast, taskTableBase/taskSlotStride/taskCount,
 * curTaskPtr and more. Reuse before inventing. */
struct BaselineRow {
  const char* file;
  uint32_t value;
  int count;
};

static const BaselineRow kBaseline[] = {
    {"runtime/recomp/cd_override.cpp", 0x1f800137u, 1},
    {"runtime/recomp/cd_override.cpp", 0x1f800138u, 1},
    {"runtime/recomp/cd_override.cpp", 0x1f80019au, 1},
    {"runtime/recomp/cd_override.cpp", 0x1f8001f0u, 2},
    {"runtime/recomp/cd_override.cpp", 0x1f8001f4u, 3},
    {"runtime/recomp/cd_override.cpp", 0x1f8001f8u, 3},
    {"runtime/recomp/cd_override.cpp", 0x8001cf00u, 2},
    {"runtime/recomp/cd_override.cpp", 0x800be220u, 1},
    {"runtime/recomp/cd_override.cpp", 0x800be222u, 1},
    {"runtime/recomp/cd_override.cpp", 0x800be224u, 1},
    {"runtime/recomp/cd_override.cpp", 0x800bed80u, 1},
    {"runtime/recomp/cd_override.cpp", 0x801fe0e0u, 3},
    {"runtime/recomp/dbg_server.cpp", 0x800be258u, 1},
    {"runtime/recomp/dbg_server.cpp", 0x800ecf58u, 1},
    {"runtime/recomp/dbg_server.cpp", 0x800f2624u, 1},
    {"runtime/recomp/dbg_server.cpp", 0x800fb168u, 1},
    {"runtime/recomp/dbg_server.cpp", 0x801fe00cu, 1},
    {"runtime/recomp/dbg_server.cpp", 0x801fe048u, 1},
    {"runtime/recomp/dualcore.cpp", 0x1f800137u, 1},
    {"runtime/recomp/dualcore.cpp", 0x800b0000u, 1},
    {"runtime/recomp/dualcore.cpp", 0x80110000u, 1},
    {"runtime/recomp/gpu_native.cpp", 0x800bf544u, 1},
    {"runtime/recomp/gpu_native.cpp", 0x801fe00cu, 2},
    {"runtime/recomp/gpu_vk.cpp", 0x1f800138u, 1},
    {"runtime/recomp/gpu_vk.cpp", 0x80100400u, 4},
    {"runtime/recomp/hle.cpp", 0x1f800138u, 1},
    {"runtime/recomp/hle.cpp", 0x1f80019bu, 1},
    {"runtime/recomp/hle.cpp", 0x1f800234u, 1},
    {"runtime/recomp/hle.cpp", 0x800bf870u, 1},
    {"runtime/recomp/hle.cpp", 0x80108f9cu, 1},
    {"runtime/recomp/hle.cpp", 0x80109450u, 1},
    {"runtime/recomp/hle.cpp", 0x8018a000u, 1},
    {"runtime/recomp/hle.cpp", 0x801fe00cu, 1},
    {"runtime/recomp/interp.cpp", 0x1f80019au, 1},
    {"runtime/recomp/interp.cpp", 0x1f8001f0u, 1},
    {"runtime/recomp/interp.cpp", 0x1f8001f4u, 1},
    {"runtime/recomp/interp.cpp", 0x1f8001f8u, 1},
    {"runtime/recomp/interp.cpp", 0x80026874u, 2},
    {"runtime/recomp/interp.cpp", 0x80052208u, 2},
    {"runtime/recomp/interp.cpp", 0x800522b0u, 2},
    {"runtime/recomp/interp.cpp", 0x80074bf8u, 1},
    {"runtime/recomp/interp.cpp", 0x80074e48u, 2},
    {"runtime/recomp/interp.cpp", 0x80075834u, 2},
    {"runtime/recomp/interp.cpp", 0x800788ccu, 1},
    {"runtime/recomp/interp.cpp", 0x8007e998u, 1},
    {"runtime/recomp/interp.cpp", 0x8007e9c8u, 1},
    {"runtime/recomp/interp.cpp", 0x8008e390u, 1},
    {"runtime/recomp/interp.cpp", 0x80090210u, 1},
    {"runtime/recomp/interp.cpp", 0x80090560u, 1},
    {"runtime/recomp/interp.cpp", 0x800909c0u, 3},
    {"runtime/recomp/interp.cpp", 0x80090bd0u, 3},
    {"runtime/recomp/interp.cpp", 0x80091460u, 1},
    {"runtime/recomp/interp.cpp", 0x800914d0u, 1},
    {"runtime/recomp/interp.cpp", 0x800939a0u, 1},
    {"runtime/recomp/interp.cpp", 0x800be0e4u, 1},
    {"runtime/recomp/interp.cpp", 0x80104c30u, 1},
    {"runtime/recomp/interp.cpp", 0x801fe00cu, 1},
    {"runtime/recomp/interp.cpp", 0x801fe134u, 1},
    {"runtime/recomp/interp.cpp", 0x801fe138u, 1},
    {"runtime/recomp/interp.cpp", 0x801fe146u, 1},
    {"runtime/recomp/mem.cpp", 0x8009a420u, 1},
    {"runtime/recomp/mem.cpp", 0x801fe00cu, 2},
    {"runtime/recomp/native_boot.cpp", 0x1f8000d2u, 2},
    {"runtime/recomp/native_boot.cpp", 0x1f8000d6u, 2},
    {"runtime/recomp/native_boot.cpp", 0x1f8000dau, 2},
    {"runtime/recomp/native_boot.cpp", 0x1f800135u, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f800137u, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f800138u, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f80019cu, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f8001f0u, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f8001f4u, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f8001f8u, 1},
    {"runtime/recomp/native_boot.cpp", 0x1f800224u, 1},
    {"runtime/recomp/native_boot.cpp", 0x800ac424u, 1},
    {"runtime/recomp/native_boot.cpp", 0x800ac42cu, 1},
    {"runtime/recomp/native_boot.cpp", 0x800be0e4u, 1},
    {"runtime/recomp/native_boot.cpp", 0x800be3d8u, 1},
    {"runtime/recomp/native_boot.cpp", 0x800bf870u, 1},
    {"runtime/recomp/native_boot.cpp", 0x80104c28u, 2},
    {"runtime/recomp/native_boot.cpp", 0x801054b0u, 2},
    {"runtime/recomp/native_boot.cpp", 0x80108000u, 1},
    {"runtime/recomp/native_boot.cpp", 0x80108f60u, 1},
    {"runtime/recomp/native_boot.cpp", 0x80109450u, 1},
    {"runtime/recomp/native_boot.cpp", 0x801fe000u, 3},
    {"runtime/recomp/native_boot.cpp", 0x801fe00cu, 1},
    {"runtime/recomp/native_boot.cpp", 0x801fe0e0u, 1},
    {"runtime/recomp/native_boot.cpp", 0x801fe0ecu, 1},
    {"runtime/recomp/native_boot.cpp", 0x801fe134u, 1},
    {"runtime/recomp/native_boot.cpp", 0x801fe138u, 1},
    {"runtime/recomp/native_boot.cpp", 0x801fe146u, 1},
    {"runtime/recomp/overlay_glue.cpp", 0x1f8000d2u, 1},
    {"runtime/recomp/overlay_glue.cpp", 0x1f8000d6u, 1},
    {"runtime/recomp/overlay_glue.cpp", 0x1f8000dau, 1},
    {"runtime/recomp/overlay_glue.cpp", 0x801fe00cu, 1},
    {"runtime/recomp/overlay_router.cpp", 0x801fe00cu, 1},
    {"runtime/recomp/pad_input.cpp", 0x1f800138u, 1},
    {"runtime/recomp/pad_input.cpp", 0x1f800236u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800be258u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800bf809u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800bf80fu, 1},
    {"runtime/recomp/pad_input.cpp", 0x800bf839u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800bf870u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800bf89cu, 1},
    {"runtime/recomp/pad_input.cpp", 0x800e7e68u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800fe916u, 1},
    {"runtime/recomp/pad_input.cpp", 0x800fe91eu, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x1f800138u, 2},
    {"runtime/recomp/pc_scheduler.cpp", 0x1f800198u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x1f80019bu, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044bd4u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044c10u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044c2cu, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044c50u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044c64u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044ca0u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80044ca8u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051f14u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051f40u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051f54u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051f68u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051f70u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051f80u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051fa4u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051fb4u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051fdcu, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051ff0u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80051ff8u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80052000u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80052010u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80052054u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80052060u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80052068u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x8007fd54u, 2},
    {"runtime/recomp/pc_scheduler.cpp", 0x80080860u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x80080870u, 2},
    {"runtime/recomp/pc_scheduler.cpp", 0x80080890u, 3},
    {"runtime/recomp/pc_scheduler.cpp", 0x800808a0u, 3},
    {"runtime/recomp/pc_scheduler.cpp", 0x801062e4u, 4},
    {"runtime/recomp/pc_scheduler.cpp", 0x8010637cu, 3},
    {"runtime/recomp/pc_scheduler.cpp", 0x801063f4u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x8010649cu, 4},
    {"runtime/recomp/pc_scheduler.cpp", 0x80109164u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x801fe070u, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x801fe0ddu, 1},
    {"runtime/recomp/pc_scheduler.cpp", 0x801fe0deu, 1},
    {"runtime/recomp/render_node.h", 0x1f80028cu, 1},
    {"runtime/recomp/render_queue.cpp", 0x800f2624u, 1},
    {"runtime/recomp/render_queue.cpp", 0x800f2738u, 1},
    {"runtime/recomp/render_queue.cpp", 0x800fb168u, 1},
    {"runtime/recomp/repl.cpp", 0x800ac424u, 1},
    {"runtime/recomp/repl.cpp", 0x800ac42cu, 1},
    {"runtime/recomp/repl.cpp", 0x800bf870u, 1},
    {"runtime/recomp/repl.cpp", 0x800e7eacu, 1},
    {"runtime/recomp/repl.cpp", 0x800e7eb4u, 1},
    {"runtime/recomp/repl.cpp", 0x800f2624u, 1},
    {"runtime/recomp/repl.cpp", 0x800f2738u, 1},
    {"runtime/recomp/repl.cpp", 0x800fb168u, 1},
    {"runtime/recomp/repl.cpp", 0x80104c28u, 1},
    {"runtime/recomp/repl.cpp", 0x801054b0u, 1},
    {"runtime/recomp/repl.cpp", 0x8010637cu, 1},
    {"runtime/recomp/repl.cpp", 0x801fe00cu, 4},
    {"runtime/recomp/repl.cpp", 0x801fe048u, 1},
    {"runtime/recomp/sbs.cpp", 0x1f800008u, 1},
    {"runtime/recomp/sbs.cpp", 0x1f800010u, 1},
    {"runtime/recomp/sbs.cpp", 0x1f8000d2u, 2},
    {"runtime/recomp/sbs.cpp", 0x1f8000d6u, 2},
    {"runtime/recomp/sbs.cpp", 0x1f8000dau, 2},
    {"runtime/recomp/sbs.cpp", 0x1f8000e8u, 2},
    {"runtime/recomp/sbs.cpp", 0x1f8000eau, 2},
    {"runtime/recomp/sbs.cpp", 0x1f8000ecu, 1},
    {"runtime/recomp/sbs.cpp", 0x1f800100u, 1},
    {"runtime/recomp/sbs.cpp", 0x1f800134u, 1},
    {"runtime/recomp/sbs.cpp", 0x1f800137u, 4},
    {"runtime/recomp/sbs.cpp", 0x1f800138u, 4},
    {"runtime/recomp/sbs.cpp", 0x1f80017cu, 1},
    {"runtime/recomp/sbs.cpp", 0x1f80019bu, 3},
    {"runtime/recomp/sbs.cpp", 0x1f8001f0u, 1},
    {"runtime/recomp/sbs.cpp", 0x1f8001fcu, 1},
    {"runtime/recomp/sbs.cpp", 0x1f800200u, 1},
    {"runtime/recomp/sbs.cpp", 0x800a4d18u, 2},
    {"runtime/recomp/sbs.cpp", 0x800a4ef8u, 6},
    {"runtime/recomp/sbs.cpp", 0x800a4f7eu, 1},
    {"runtime/recomp/sbs.cpp", 0x800a4f80u, 2},
    {"runtime/recomp/sbs.cpp", 0x800a5000u, 1},
    {"runtime/recomp/sbs.cpp", 0x800abde0u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ac000u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ac5f8u, 2},
    {"runtime/recomp/sbs.cpp", 0x800ac700u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ac800u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be000u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be0e0u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be0e4u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be0f0u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be110u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be1f8u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be238u, 2},
    {"runtime/recomp/sbs.cpp", 0x800be258u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be358u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be3b8u, 1},
    {"runtime/recomp/sbs.cpp", 0x800be3f8u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bed78u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bed80u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bed84u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bed88u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf000u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf800u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf839u, 4},
    {"runtime/recomp/sbs.cpp", 0x800bf83au, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf870u, 7},
    {"runtime/recomp/sbs.cpp", 0x800bf873u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf89cu, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf900u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bf9b4u, 2},
    {"runtime/recomp/sbs.cpp", 0x800bf9b5u, 1},
    {"runtime/recomp/sbs.cpp", 0x800bfa13u, 1},
    {"runtime/recomp/sbs.cpp", 0x800e0000u, 1},
    {"runtime/recomp/sbs.cpp", 0x800e7de0u, 1},
    {"runtime/recomp/sbs.cpp", 0x800e7e74u, 1},
    {"runtime/recomp/sbs.cpp", 0x800e7e80u, 1},
    {"runtime/recomp/sbs.cpp", 0x800e7eacu, 1},
    {"runtime/recomp/sbs.cpp", 0x800ecf54u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ecf80u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ecfd4u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ed000u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ed020u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ed098u, 7},
    {"runtime/recomp/sbs.cpp", 0x800ed09cu, 2},
    {"runtime/recomp/sbs.cpp", 0x800ee480u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ef478u, 1},
    {"runtime/recomp/sbs.cpp", 0x800ef500u, 1},
    {"runtime/recomp/sbs.cpp", 0x800f2624u, 1},
    {"runtime/recomp/sbs.cpp", 0x800f2738u, 1},
    {"runtime/recomp/sbs.cpp", 0x800fb165u, 2},
    {"runtime/recomp/sbs.cpp", 0x800fb166u, 1},
    {"runtime/recomp/sbs.cpp", 0x800fb168u, 1},
    {"runtime/recomp/sbs.cpp", 0x801054ceu, 1},
    {"runtime/recomp/sbs.cpp", 0x80105c10u, 1},
    {"runtime/recomp/sbs.cpp", 0x80105ca0u, 1},
    {"runtime/recomp/sbs.cpp", 0x80105d00u, 1},
    {"runtime/recomp/sbs.cpp", 0x80105ee8u, 3},
    {"runtime/recomp/sbs.cpp", 0x80105eecu, 1},
    {"runtime/recomp/sbs.cpp", 0x80105f00u, 1},
    {"runtime/recomp/sbs.cpp", 0x80157000u, 1},
    {"runtime/recomp/sbs.cpp", 0x8017d000u, 1},
    {"runtime/recomp/sbs.cpp", 0x801fe150u, 1},
    {"runtime/recomp/sbs.cpp", 0x801ff200u, 1},
    {"runtime/recomp/scheduler.cpp", 0x8010637cu, 1},
    {"runtime/recomp/scheduler.cpp", 0x801063f4u, 2},
    {"runtime/recomp/scheduler.cpp", 0x801fe0e0u, 1},
    {"runtime/recomp/scheduler.h", 0x1f800138u, 1},
    {"runtime/recomp/scheduler.h", 0x801fe000u, 1},
    {"runtime/recomp/selftest.cpp", 0x1f800198u, 11},
    {"runtime/recomp/selftest.cpp", 0x800b0000u, 3},
    {"runtime/recomp/selftest.cpp", 0x800bf870u, 2},
    {"runtime/recomp/selftest.cpp", 0x800bf9b4u, 7},
    {"runtime/recomp/selftest.cpp", 0x801026e0u, 1},
    {"runtime/recomp/selftest.cpp", 0x80104c00u, 1},
    {"runtime/recomp/selftest.cpp", 0x80109450u, 5},
    {"runtime/recomp/selftest.cpp", 0x80110000u, 1},
    {"runtime/recomp/selftest.cpp", 0x801138a4u, 3},
    {"runtime/recomp/selftest.cpp", 0x801fe048u, 4},
    {"runtime/recomp/timing.cpp", 0x800abde0u, 1},
    {"runtime/ui/menu_readouts.cpp", 0x801062e4u, 1},
    {"runtime/ui/menu_readouts.cpp", 0x8010637cu, 1},
    {"runtime/ui/menu_readouts.cpp", 0x8010649cu, 1},
};

/* ---- checks ---------------------------------------------------------------------------------- */

/* The classifier, asserted in BOTH directions. The negative half is the point: a gate that trips on
 * console constants would be reverted within a week, so every family listed at the top is here. */
static void test_classifier(void) {
  /* POSITIVE — real game addresses this gate exists to catch. */
  CHECK(is_game_addr(0x800bfe68u)); /* Tomba packet-pool base (the ot_attr.cpp bug)      */
  CHECK(is_game_addr(0x800e7e68u)); /* Tomba packet-pool end                             */
  CHECK(is_game_addr(0x801fe00cu)); /* Tomba high-RAM global                             */
  CHECK(is_game_addr(0xa00bfe68u)); /* the same object through KSEG1                     */
  CHECK(is_game_addr(0x1f800138u)); /* scratchpad+0x138 = Tomba's current-task pointer   */
  CHECK(is_game_addr(0x1f8003ffu)); /* last scratchpad byte                              */
  CHECK(is_game_addr(0x80010004u)); /* just above the allowed PS-EXE load base           */

  /* NEGATIVE — the CONSOLE. None of these may ever trip this gate. */
  CHECK(!is_game_addr(0x1f801810u)); /* GP0                                              */
  CHECK(!is_game_addr(0x1f801814u)); /* GP1                                              */
  CHECK(!is_game_addr(0x1f801070u)); /* I_STAT                                           */
  CHECK(!is_game_addr(0x1f8010f0u)); /* DPCR                                             */
  CHECK(!is_game_addr(0x1f801c00u)); /* SPU voice regs                                   */
  CHECK(!is_game_addr(0x1f800000u)); /* scratchpad BASE names the region, not a field     */
  CHECK(!is_game_addr(0x00200000u)); /* main-RAM SIZE                                    */
  CHECK(!is_game_addr(0x1fffffffu)); /* physical-address mask                            */
  CHECK(!is_game_addr(0x80000000u)); /* KSEG0 base / segment mask                        */
  CHECK(!is_game_addr(0xa0000000u)); /* KSEG1 base                                       */
  CHECK(!is_game_addr(0xbfc00000u)); /* BIOS ROM                                         */
  CHECK(!is_game_addr(0x80000080u)); /* exception vector                                 */
  CHECK(!is_game_addr(0x80000200u)); /* A0 table                                         */
  CHECK(!is_game_addr(0x80000874u)); /* B0 table                                         */
  CHECK(!is_game_addr(0x80000674u)); /* C0 table                                         */
  CHECK(!is_game_addr(0x8000ffffu)); /* top of the kernel region                         */
  CHECK(!is_game_addr(0x80010000u)); /* PS-EXE load base (named exception)               */
  CHECK(!is_game_addr(0x801ffff0u)); /* conventional initial SP (named exception)        */
  CHECK(!is_game_addr(0x00000400u)); /* VRAM height 1024/512 as hex, GP0 opcodes, etc.   */
  CHECK(!is_game_addr(0xfc000000u)); /* R3000A opcode mask                               */
  CHECK(!is_game_addr(0x00000010u));
}

/* Proof that the SCANNER fires, in the shipping artifact, independent of any manual injection: a
 * synthetic source file whose every case is known. If this ever passes trivially the gate is dead. */
static void test_scanner_selftest(void) {
  const std::string src =
      "// a comment naming 0x800BFE68 is RE documentation, not a violation\n"
      "/* nor is 0x801FE00C in a block comment */\n"
      "static const char* name = \"packet pool at 0x800E7E68\";\n"
      "static uint32_t hw = 0x1F801810u;      // GP0: console\n"
      "static uint32_t spad = 0x1F800000u;    // scratchpad base: console\n"
      "static uint32_t ramsz = 0x200000u;     // console\n"
      "static uint32_t mask = 0x1FFFFFFFu;    // console\n"
      "static uint32_t load = 0x80010000u;    // PS-EXE load base: allowed\n"
      "static uint32_t pool = 0x800BFE68u;    // <-- VIOLATION (line 9)\n"
      "static uint32_t task = 0x1F800138u;    // <-- VIOLATION (line 10)\n";
  Scan sc;
  scan_text("synthetic.cpp", src, sc);
  /* The denominator, so a "found nothing" can never be confused with "never looked": 7 hex literals
   * survive comment/string stripping (the 3 in comments and strings do not). */
  CHECK_EQ(sc.numbers, 7);
  CHECK_EQ((int)sc.hits.size(), 2);
  CHECK_EQ(sc.hits[0].value, 0x800bfe68u);
  CHECK_EQ(sc.hits[0].line, 9);
  CHECK_EQ(sc.hits[1].value, 0x1f800138u);
  CHECK_EQ(sc.hits[1].line, 10);

  /* And the inverse: a file of nothing but console constants must yield ZERO hits out of a NON-ZERO
   * denominator. "0 of 0" would mean the scanner stopped working. */
  Scan clean;
  scan_text("clean.cpp", "uint32_t a[] = {0x1F801810u,0x1F800000u,0x200000u,0x80000080u,0xBFC00000u};\n",
            clean);
  CHECK_EQ(clean.numbers, 5);
  CHECK_EQ((int)clean.hits.size(), 0);
}

/* The corpus must exist and be non-trivial. Without this, a broken path would report a clean repo. */
static void test_corpus_present(void) {
  const Scan sc = scan_framework();
  fprintf(stderr, "    scanned %d framework source files under runtime/ + common/, %ld hex literals\n",
          sc.files, sc.numbers);
  CHECK(sc.corpus_ok);
  CHECK(sc.files > 100);   /* the framework is ~200 files; a handful means the walk broke */
  CHECK(sc.numbers > 500); /* and it really did look at numbers                          */
}

/* THE GATE. */
static void test_no_new_game_address_literals(void) {
  const Scan sc = scan_framework();
  CHECK(sc.corpus_ok); /* never certify a clean tree off a failed scan */

  typedef std::pair<std::string, uint32_t> Key;
  std::map<Key, int> found;
  std::map<Key, std::string> where; /* first line number seen, for the failure message */
  for (const Hit& h : sc.hits) {
    const Key k(h.file, h.value);
    if (found[k]++ == 0) where[k] = h.file + ":" + std::to_string(h.line);
  }

  std::map<Key, int> base;
  int base_total = 0;
  for (const BaselineRow& r : kBaseline) {
    base[Key(r.file, r.value)] += r.count;
    base_total += r.count;
  }

  int found_total = 0;
  for (const auto& kv : found) found_total += kv.second;

  /* The number, every run, so progress is legible. */
  fprintf(stderr,
          "    game-address literals in framework live code: %d (baseline %d) across %d (file,value)"
          " pairs\n",
          found_total, base_total, (int)found.size());

  int nnew = 0, nstale = 0, nfixed = 0;
  for (const auto& kv : found) {
    const int b = base.count(kv.first) ? base[kv.first] : 0;
    if (kv.second > b) {
      ++nnew;
      fprintf(stderr,
              "    NEW game address in framework code: %s 0x%08x x%d (baseline %d) at %s\n"
              "      -> move it into GameConfig (runtime/recomp/game_iface.h) and read it from"
              " there; see ot_attr.cpp pool_range() for the honest-zero shape.\n",
              kv.first.first.c_str(), kv.first.second, kv.second, b, where[kv.first].c_str());
    } else if (kv.second < b) {
      ++nstale;
      fprintf(stderr,
              "    STALE baseline row: %s 0x%08x now occurs %d time(s), baseline says %d.\n"
              "      -> lower the count in tests/test_no_game_address_literals.cpp (the baseline is"
              " shrink-only; a fix and its baseline edit land together).\n",
              kv.first.first.c_str(), kv.first.second, kv.second, b);
    }
  }
  for (const auto& kv : base) {
    if (found.count(kv.first)) continue;
    ++nfixed;
    fprintf(stderr,
            "    FIXED (baseline row no longer matches anything): %s 0x%08x\n"
            "      -> DELETE that row from tests/test_no_game_address_literals.cpp. Leaving it would"
            " let the literal come back unnoticed.\n",
            kv.first.first.c_str(), kv.first.second);
  }

  CHECK_EQ(nnew, 0);
  CHECK_EQ(nstale, 0);
  CHECK_EQ(nfixed, 0);
  CHECK_EQ(found_total, base_total);
}

int main(void) {
  RUN(classifier);
  RUN(scanner_selftest);
  RUN(corpus_present);
  RUN(no_new_game_address_literals);
  return pt_summary();
}
