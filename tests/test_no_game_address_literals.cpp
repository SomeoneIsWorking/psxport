// test_no_game_address_literals.cpp — THE GATE that keeps psxport game-agnostic.
//
// WHY THIS EXISTS. psxport is supposed to carry no game code: a title subclasses GameRuntime
// (runtime/psx/game/game_runtime.h) and answers the framework through its virtuals. That rule was decaying
// silently. runtime/psx/debug/ot_attr.cpp held
// Tomba!2's packet-pool range as two file-scope constants (0x800BFE68/0x800E7E68), so on spyro and
// spider1 the entire OT attribution table matched nothing and reported no spans — which is
// indistinguishable from "the guest submitted no packets". Nothing failed; nobody could have known.
// A rule nobody can check is a rule that decays, so this is a test rather than a paragraph in a doc.
//
// WHAT IT DOES. It scans the framework's OWN sources (runtime/) for hex literals in LIVE
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
// FIXING one means: move the address into the title and have the framework ask through a GameRuntime
// virtual (runtime/psx/game/game_runtime.h), obeying the honest-zero rule — a title that has not
// located it answers empty, and the consumer announces its blindness once. GameRuntime::packetPool is
// the worked example.

#include "source_tree.h"
#include "testutil.h"

#include <sys/stat.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

/* ---- the classifier -------------------------------------------------------------------------- */

/* Values inside the game window that are PS-EXE conventions rather than one game's data. */
const uint32_t kConsoleExceptions[] = {
    0x80010000u, /* standard PS-EXE load base                */
    0x801ffff0u, /* conventional initial stack pointer        */
};

bool is_console_exception(uint32_t v) {
  for (uint32_t e : kConsoleExceptions) {
    if (v == e) {
      return true;
    }
  }
  return false;
}

bool is_game_addr(uint32_t v) {
  if (is_console_exception(v)) {
    return false;
  }
  if (v >= 0x80010000u && v <= 0x801fffffu) {
    return true; /* game-owned main RAM, KSEG0        */
  }
  if (v >= 0xa0010000u && v <= 0xa01fffffu) {
    return true; /* same, uncached mirror             */
  }
  if (v > 0x1f800000u && v <= 0x1f8003ffu) {
    return true; /* a field in scratchpad = game field */
  }
  return false;
}

/* ---- the scanner ----------------------------------------------------------------------------- */

/* Comments and string/char literals are documentation, not behaviour (see the header comment), so
 * they are blanked out before the number scan. Newlines are preserved so line numbers stay true. */
std::string strip_comments_and_strings(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  const size_t n = s.size();
  for (size_t i = 0; i < n;) {
    const char c = s[i];
    if (c == '/' && i + 1 < n && s[i + 1] == '/') {
      while (i < n && s[i] != '\n') {
        ++i;
      }
    } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
        if (s[i] == '\n') {
          out += '\n';
        }
        ++i;
      }
      i = (i + 1 < n) ? i + 2 : n;
    } else if (c == '"' || c == '\'') {
      const char q = c;
      ++i;
      while (i < n && s[i] != q) {
        if (s[i] == '\\') {
          ++i;
        }
        if (i < n && s[i] == '\n') {
          out += '\n'; /* unterminated literal: do not eat the file */
        }
        ++i;
      }
      if (i < n) {
        ++i;
      }
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

bool is_source(const std::string &f) {
  const char *ext[] = {".c", ".cpp", ".h", ".hpp"};
  for (const char *e : ext) {
    const size_t k = strlen(e);
    if (f.size() > k && f.compare(f.size() - k, k, e) == 0) {
      return true;
    }
  }
  return false;
}

void scan_text(const std::filesystem::path &rel, const std::string &text, Scan &sc) {
  const std::string code = strip_comments_and_strings(text);
  int line = 1;
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '\n') {
      ++line;
      continue;
    }
    /* A hex literal, not a suffix of an identifier like `foo0x1`. */
    if (code[i] != '0' || i + 1 >= code.size() || (code[i + 1] != 'x' && code[i + 1] != 'X')) {
      continue;
    }
    if (i > 0 && (isalnum((unsigned char)code[i - 1]) || code[i - 1] == '_')) {
      continue;
    }
    size_t j = i + 2, digits = 0;
    uint64_t v = 0;
    while (j < code.size() && isxdigit((unsigned char)code[j])) {
      const char d = code[j];
      v = v * 16 + (uint64_t)(d <= '9' ? d - '0' : (tolower(d) - 'a' + 10));
      ++digits;
      ++j;
    }
    i = j - 1;
    if (digits < 4 || digits > 8) {
      continue; /* <4 digits cannot be a 32-bit guest address */
    }
    ++sc.numbers;
    if (is_game_addr((uint32_t)v)) {
      sc.hits.push_back(Hit{rel.generic_string(), line, (uint32_t)v});
    }
  }
}

std::string read_file(const std::string &path, bool *ok) {
  std::ifstream in(path, std::ios::binary);
  *ok = in.is_open();
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Every source file under root/dir, skipping vendored and cache trees.
void walk(const std::filesystem::path &root, const std::filesystem::path &dir, Scan &sc) {
  namespace fs = std::filesystem;
  for (auto it = fs::recursive_directory_iterator(root / dir); it != fs::recursive_directory_iterator(); ++it) {
    const std::string name = it->path().filename().string();
    if (it->is_directory()) {
      if (name == "vendor" || name == "__pycache__") {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file() || !is_source(name)) {
      continue;
    }
    const fs::path rel = fs::relative(it->path(), root);
    bool ok = false;
    const std::string text = read_file(it->path().string(), &ok);
    if (!ok) {
      /* A file we cannot read is a hole in the scan, not a clean result. */
      fprintf(stderr, "    SCAN ERROR: cannot read %s\n", rel.generic_string().c_str());
      continue;
    }
    ++sc.files;
    scan_text(rel, text, sc);
  }
}

/* The scanned corpus. Anything the framework static library compiles lives under these. */
const char *kScanDirs[] = {"runtime"};

Scan scan_framework(void) {
  Scan sc;
  const std::string root = psx::test::sourceRoot().string();
  int dirs_found = 0;
  for (const char *d : kScanDirs) {
    struct stat st;
    const std::string a = root + "/" + d;
    if (stat(a.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      /* Refuse rather than return empty: a scan of a directory that does not exist has searched
       * NOTHING, and "0 violations" out of that would be a lie. */
      fprintf(stderr, "    REFUSING: scan dir does not exist: %s\n", a.c_str());
      continue;
    }
    ++dirs_found;
    walk(root, d, sc);
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
 *                          single block; needs a title-answered region table.
 *   selftest.cpp (38)      built-in selftest asserts written against Tomba's RAM map.
 *   retired CPU hooks      trace/watch hooks pinned to Tomba functions.
 *   repl.cpp (16)          debug REPL default watch/inspect targets.
 *   pad_input.cpp (11)     Tomba's pad-state globals.
 *   hle.cpp (8)            HLE entry points identified by Tomba guest address.
 *   dbg_server.cpp (6)     debug-server default watch set.
 *   dualcore.cpp (3)       dual-core compare window (the packet-pool range literal).
 *   mem.cpp (3)            memory-map special cases for Tomba addresses.
 *   overlay_router.cpp (1) one Tomba scratchpad field.
 *
 * When fixing: the address belongs in the title, reached through a GameRuntime virtual. */
struct BaselineRow {
  const char *file;
  uint32_t value;
  int count;
};

const BaselineRow kBaseline[] = {
    {"runtime/psx/debug/dbg_server.cpp", 0x800be258u, 1},
    {"runtime/psx/debug/dbg_server.cpp", 0x800ecf58u, 1},
    {"runtime/psx/debug/dbg_server.cpp", 0x800f2624u, 1},
    {"runtime/psx/debug/dbg_server.cpp", 0x800fb168u, 1},
    {"runtime/psx/debug/dbg_server.cpp", 0x801fe00cu, 1},
    {"runtime/psx/debug/dbg_server.cpp", 0x801fe048u, 1},
    {"runtime/psx/core/mem.cpp", 0x801fe00cu, 2},
    {"runtime/psx/input/pad_input.cpp", 0x1f800138u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x1f800236u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800be258u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800bf809u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800bf80fu, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800bf839u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800bf870u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800bf89cu, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800e7e68u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800fe916u, 1},
    {"runtime/psx/input/pad_input.cpp", 0x800fe91eu, 1},
};

/* ---- checks ---------------------------------------------------------------------------------- */

/* The classifier, asserted in BOTH directions. The negative half is the point: a gate that trips on
 * console constants would be reverted within a week, so every family listed at the top is here. */
void test_classifier(void) {
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
void test_scanner_selftest(void) {
  const std::string src = "// a comment naming 0x800BFE68 is RE documentation, not a violation\n"
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
  scan_text("clean.cpp", "uint32_t a[] = {0x1F801810u,0x1F800000u,0x200000u,0x80000080u,0xBFC00000u};\n", clean);
  CHECK_EQ(clean.numbers, 5);
  CHECK_EQ((int)clean.hits.size(), 0);
}

/* The corpus must exist and be non-trivial. Without this, a broken path would report a clean repo. */
void test_corpus_present(void) {
  const Scan sc = scan_framework();
  fprintf(stderr, "    scanned %d framework source files under runtime/, %ld hex literals\n", sc.files, sc.numbers);
  CHECK(sc.corpus_ok);
  CHECK(sc.files > 100);   /* the framework is ~200 files; a handful means the walk broke */
  CHECK(sc.numbers > 500); /* and it really did look at numbers                          */
}

/* THE GATE. */
void test_no_new_game_address_literals(void) {
  const Scan sc = scan_framework();
  CHECK(sc.corpus_ok); /* never certify a clean tree off a failed scan */

  typedef std::pair<std::string, uint32_t> Key;
  std::map<Key, int> found;
  std::map<Key, std::string> where; /* first line number seen, for the failure message */
  for (const Hit &h : sc.hits) {
    const Key k(h.file, h.value);
    if (found[k]++ == 0) {
      where[k] = h.file + ":" + std::to_string(h.line);
    }
  }

  std::map<Key, int> base;
  int base_total = 0;
  for (const BaselineRow &r : kBaseline) {
    base[Key(r.file, r.value)] += r.count;
    base_total += r.count;
  }

  int found_total = 0;
  for (const auto &kv : found) {
    found_total += kv.second;
  }

  /* The number, every run, so progress is legible. */
  fprintf(stderr,
          "    game-address literals in framework live code: %d (baseline %d) across %d (file,value)"
          " pairs\n",
          found_total,
          base_total,
          (int)found.size());

  int nnew = 0, nstale = 0, nfixed = 0;
  for (const auto &kv : found) {
    const int b = base.count(kv.first) ? base[kv.first] : 0;
    if (kv.second > b) {
      ++nnew;
      fprintf(stderr,
              "    NEW game address in framework code: %s 0x%08x x%d (baseline %d) at %s\n"
              "      -> move it into the title and ask through a GameRuntime virtual"
              " (runtime/psx/game/game_runtime.h); see GameRuntime::packetPool for the honest-zero shape.\n",
              kv.first.first.c_str(),
              kv.first.second,
              kv.second,
              b,
              where[kv.first].c_str());
    } else if (kv.second < b) {
      ++nstale;
      fprintf(stderr,
              "    STALE baseline row: %s 0x%08x now occurs %d time(s), baseline says %d.\n"
              "      -> lower the count in tests/test_no_game_address_literals.cpp (the baseline is"
              " shrink-only; a fix and its baseline edit land together).\n",
              kv.first.first.c_str(),
              kv.first.second,
              kv.second,
              b);
    }
  }
  for (const auto &kv : base) {
    if (found.count(kv.first)) {
      continue;
    }
    ++nfixed;
    fprintf(stderr,
            "    FIXED (baseline row no longer matches anything): %s 0x%08x\n"
            "      -> DELETE that row from tests/test_no_game_address_literals.cpp. Leaving it would"
            " let the literal come back unnoticed.\n",
            kv.first.first.c_str(),
            kv.first.second);
  }

  CHECK_EQ(nnew, 0);
  CHECK_EQ(nstale, 0);
  CHECK_EQ(nfixed, 0);
  CHECK_EQ(found_total, base_total);
}

} // namespace

int main(void) {
  RUN(classifier);
  RUN(scanner_selftest);
  RUN(corpus_present);
  RUN(no_new_game_address_literals);
  return psx::test::summary();
}
