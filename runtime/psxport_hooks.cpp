// psxport hook & override registry. See psxport_hooks.h.

#include "psxport_hooks.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace {

struct Hook
{
  uint32_t pc;
  uint32_t expected_instr; // 0 = no signature check
  psxport_hook_fn fn;
};

std::vector<Hook> s_hooks;

} // namespace

uint32_t psxport_hook_count = 0;
uint8_t* psxport_cov_bitmap = nullptr;
int psxport_cd_instant = []() {
  const char* v = std::getenv("PSXPORT_CD_INSTANT");
  return v ? static_cast<int>(std::strtol(v, nullptr, 0)) : -1; // -1 = unset
}();
int psxport_cdc_log = []() { const char* v = std::getenv("PSXPORT_CDC_LOG"); return (v && *v && *v != '0') ? 1 : 0; }();

uint32_t psxport_last_pc = 0;

// PC-sampling profiler. Counts every dispatched instruction PC into a histogram
// so we can see where the CPU actually spends time during a load/dwell.
int psxport_prof = 0;
namespace {
std::unordered_map<uint32_t, uint64_t> s_prof_hist;
uint64_t s_prof_total = 0;
}
void psxport_prof_reset(void)
{
  s_prof_hist.clear();
  s_prof_total = 0;
}
void psxport_prof_report(int top)
{
  std::vector<std::pair<uint32_t, uint64_t>> v(s_prof_hist.begin(), s_prof_hist.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  fprintf(stderr, "=== PC profile: %llu samples, %zu distinct PCs ===\n",
          (unsigned long long)s_prof_total, v.size());
  // Region breakdown so the hotspot's home (BIOS ROM vs kernel RAM vs game) is obvious.
  uint64_t r_biosrom = 0, r_kernel = 0, r_game = 0, r_other = 0;
  for (const auto& p : v)
  {
    const uint32_t pc = p.first & 0x1FFFFFFF;
    if (pc >= 0x1FC00000) r_biosrom += p.second;          // BIOS ROM (0xBFC.....)
    else if (pc < 0x10000) r_kernel += p.second;          // kernel/handlers in low RAM
    else if (pc < 0x200000) r_game += p.second;           // game / overlay code
    else r_other += p.second;
  }
  auto pct = [&](uint64_t n) { return s_prof_total ? (100.0 * n / s_prof_total) : 0.0; };
  fprintf(stderr, "  region: biosrom=%.1f%%  kernelRAM=%.1f%%  game=%.1f%%  other=%.1f%%\n",
          pct(r_biosrom), pct(r_kernel), pct(r_game), pct(r_other));
  const int n = (int)v.size() < top ? (int)v.size() : top;
  for (int i = 0; i < n; i++)
    fprintf(stderr, "  %2d. pc=%08X  %8llu  %5.1f%%\n", i + 1, v[i].first,
            (unsigned long long)v[i].second, pct(v[i].second));
  // Top game-range PCs (0x10000..0x200000) even when negligible overall: when a
  // wait lives entirely inside the BIOS, the few game samples are the poll-loop
  // call site that initiated it -- the thing to native-skip.
  fprintf(stderr, "  -- top game-range PCs --\n");
  int shown = 0;
  for (const auto& p : v)
  {
    const uint32_t a = p.first & 0x1FFFFFFF;
    if (a >= 0x10000 && a < 0x200000)
    {
      fprintf(stderr, "     pc=%08X  %8llu  %5.3f%%\n", p.first,
              (unsigned long long)p.second, pct(p.second));
      if (++shown >= 12) break;
    }
  }
  if (!shown) fprintf(stderr, "     (none)\n");
}

// Write-watchpoint. Armed from PSXPORT_WATCHW in the frontend. Logs distinct
// (pc,val) pairs to stderr so a store in a per-frame loop doesn't spam — each
// new writing PC or new value prints once, which is exactly the transition log
// you want when finding who drives a variable.
uint32_t psxport_watch_addr = PSXPORT_WATCH_OFF;
const uint8_t* psxport_ram_ptr = nullptr; // set at init; for watchpoint backtraces

// Heuristic MIPS stack backtrace: scan $sp upward for word-aligned text addresses
// preceded by a jal/jalr (return addresses). Prints the call chain so a flag's
// writer can be chased up to its decision site (vs poking the flag).
void psxport_backtrace(void)
{
  const uint8_t* ram = psxport_ram_ptr;
  if (!ram) return;
  const uint32_t* gpr = psxport_cpu_gpr();
  fprintf(stderr, "    bt: pc=%08X ra=%08X\n", psxport_last_pc, gpr[31]);
  const uint32_t sp = gpr[29] & 0x1FFFFF;
  int found = 0;
  for (uint32_t off = 0; off < 0x1000 && found < 20; off += 4) {
    if (sp + off + 4 > 0x200000) break;
    uint32_t w; memcpy(&w, ram + sp + off, 4);
    if ((w & 0xF0000003) == 0x80000000 && (w & 0x1FFFFF) >= 0x8000 && (w & 0x1FFFFF) < 0x1FFFF8) {
      uint32_t prev; memcpy(&prev, ram + ((w - 8) & 0x1FFFFF), 4);
      const bool is_jal = (prev >> 26) == 3;
      const bool is_jalr = ((prev >> 26) == 0) && ((prev & 0x3F) == 9);
      if (is_jal || is_jalr) {
        char tgt[16] = "reg";
        if (is_jal) snprintf(tgt, sizeof(tgt), "%08X", ((w - 8) & 0xF0000000) | ((prev & 0x3FFFFFF) << 2));
        fprintf(stderr, "      sp+%03X ret=%08X (-> %s)\n", off, w, tgt);
        found++;
      }
    }
  }
}

extern "C" void psxport_on_write(uint32_t addr, uint32_t val, uint32_t pc, int width)
{
  static int s_bt = -1;
  if (s_bt < 0) s_bt = std::getenv("PSXPORT_WATCHW_BT") ? 1 : 0;
  static uint32_t last_pc = 0xFFFFFFFFu, last_val = 0xDEADBEEFu;
  if (pc == last_pc && val == last_val)
    return;
  last_pc = pc;
  last_val = val;
  const uint32_t ra = psxport_cpu_gpr()[31]; // caller (return address)
  fprintf(stderr, "[WATCHW] f%u pc=%08X ra=%08X writes %08X (w%d) -> [%08X]\n",
          psxport_frame, pc, ra, val, width, addr);
  if (s_bt) psxport_backtrace();
}
unsigned psxport_frame = 0;

// --- Scoped PC trace ring (RE aid) -------------------------------------------
// Captures every executed PC within [psxport_pctrace_lo, psxport_pctrace_hi)
// into a ring buffer, so a function's exact execution path on one frame can be
// dumped and diffed against another frame (e.g. a "waiting" vs "advancing"
// iteration of a state machine). Enabled via PSXPORT_PCTRACE="lohex-hihex";
// dumped with the REPL `pctrace <path>` command. Cheap: one range check per
// instruction, push only in-range.
uint32_t  psxport_pctrace_lo = 0, psxport_pctrace_hi = 0;
// Optional exclusion sub-range [excl_lo,excl_hi): PCs in it are NOT recorded, so a
// hot spin loop (e.g. StrPlayer pace dwell 0x80050CE4) can't flood the ring and bury
// the once-per-frame divergence we're hunting. Set PSXPORT_PCTRACE_EXCL="lo-hi".
uint32_t  psxport_pctrace_excl_lo = 0, psxport_pctrace_excl_hi = 0;
uint32_t* g_pctrace = nullptr;
uint32_t  g_pctrace_cap = 0, g_pctrace_idx = 0;
bool      g_pctrace_wrapped = false;
extern "C" void psxport_pctrace_init() {
  const char* v = std::getenv("PSXPORT_PCTRACE");
  if (!v || !*v) return;
  unsigned lo = 0, hi = 0;
  if (std::sscanf(v, "%x-%x", &lo, &hi) == 2 && hi > lo) {
    psxport_pctrace_lo = lo; psxport_pctrace_hi = hi;
    g_pctrace_cap = 1u << 22; // 4M entries (16 MB) — many frames of one function
    g_pctrace = (uint32_t*)std::malloc(g_pctrace_cap * sizeof(uint32_t));
    fprintf(stderr, "[pctrace] capturing PCs in [%08X,%08X) cap=%u\n", lo, hi, g_pctrace_cap);
  }
  const char* e = std::getenv("PSXPORT_PCTRACE_EXCL");
  if (e && *e) {
    unsigned elo = 0, ehi = 0;
    if (std::sscanf(e, "%x-%x", &elo, &ehi) == 2 && ehi > elo) {
      psxport_pctrace_excl_lo = elo; psxport_pctrace_excl_hi = ehi;
      fprintf(stderr, "[pctrace] excluding PCs in [%08X,%08X)\n", elo, ehi);
    }
  }
}
extern "C" void psxport_pctrace_push(uint32_t pc) {
  if (pc < psxport_pctrace_lo || pc >= psxport_pctrace_hi) return;
  if (pc >= psxport_pctrace_excl_lo && pc < psxport_pctrace_excl_hi) return;
  g_pctrace[g_pctrace_idx] = pc;
  if (++g_pctrace_idx >= g_pctrace_cap) { g_pctrace_idx = 0; g_pctrace_wrapped = true; }
}
extern "C" void psxport_pctrace_dump(const char* path) {
  if (!g_pctrace) { fprintf(stderr, "[pctrace] not enabled\n"); return; }
  FILE* f = std::fopen(path, "w");
  if (!f) { fprintf(stderr, "[pctrace] cannot open %s\n", path); return; }
  uint32_t start = g_pctrace_wrapped ? g_pctrace_idx : 0;
  uint32_t n = g_pctrace_wrapped ? g_pctrace_cap : g_pctrace_idx;
  for (uint32_t i = 0; i < n; i++)
    std::fprintf(f, "%08X\n", g_pctrace[(start + i) % g_pctrace_cap]);
  std::fclose(f);
  fprintf(stderr, "[pctrace] dumped %u PCs -> %s\n", n, path);
}
// --- Streaming call trace (RE aid) -------------------------------------------
// Logs every jal/jalr target (function entry) landing in [lo,hi) to a file, with
// "# frame N" markers, so the game's actual boot CALL PATH (function sequence) can
// be read end-to-end. Sparse (only calls), so it holds the whole boot, unlike the
// per-instruction pctrace ring. Enable: PSXPORT_CALLTRACE="lohex-hihex[:path]".
uint32_t psxport_calltrace_on = 0;
static FILE* g_ct = nullptr;
static uint32_t g_ct_lo = 0, g_ct_hi = 0, g_ct_n = 0, g_ct_max = 4000000;
extern "C" void psxport_calltrace_init() {
  const char* v = std::getenv("PSXPORT_CALLTRACE");
  if (!v || !*v) return;
  unsigned lo = 0, hi = 0; char path[512] = "scratch/logs/calltrace.txt";
  if (std::sscanf(v, "%x-%x:%511s", &lo, &hi, path) >= 2 && hi > lo) {
    g_ct_lo = lo; g_ct_hi = hi;
    g_ct = std::fopen(path, "w");
    psxport_calltrace_on = (g_ct != nullptr);
    fprintf(stderr, "[calltrace] %s calls into [%08X,%08X) -> %s\n",
            g_ct ? "logging" : "FAILED", lo, hi, path);
  }
}
extern "C" void psxport_calltrace(uint32_t pc, uint32_t instr, const uint32_t* gpr) {
  if (!g_ct || g_ct_n >= g_ct_max) return;
  uint32_t op = instr >> 26, tgt;
  if (op == 3) tgt = (pc & 0xF0000000u) | ((instr & 0x03FFFFFFu) << 2);   // jal
  else if (op == 0 && (instr & 0x3F) == 9) tgt = gpr[(instr >> 21) & 31]; // jalr
  else return;
  if (tgt < g_ct_lo || tgt >= g_ct_hi) return;
  std::fprintf(g_ct, "%08X %08X\n", pc, tgt);
  g_ct_n++;
}
extern "C" void psxport_calltrace_frame(int frame) {
  if (g_ct) std::fprintf(g_ct, "# frame %d\n", frame);
}

int psxport_gte_capture = 0;
int psxport_rtp_capture = 0;
int psxport_gpu_capture = 0;
unsigned psxport_gte_op[64] = {0};
int psxport_mvmva_capture = 0;

namespace {
psxport_gte_cr_fn s_gte_cr_fn = nullptr;
psxport_rtp_fn s_rtp_fn = nullptr;
psxport_gpu_poly_fn s_gpu_poly_fn = nullptr;
}

extern "C" void psxport_on_gte_cr(unsigned which, uint32_t value)
{
  if (s_gte_cr_fn)
    s_gte_cr_fn(which, value);
}

void psxport_set_gte_cr_hook(psxport_gte_cr_fn fn)
{
  s_gte_cr_fn = fn;
  psxport_gte_capture = (fn != nullptr) ? 1 : 0;
}

extern "C" void psxport_on_rtp_vertex(int32_t vx, int32_t vy, int32_t vz, int32_t sx, int32_t sy, uint32_t sf)
{
  if (s_rtp_fn)
    s_rtp_fn(vx, vy, vz, sx, sy, sf);
}

void psxport_set_rtp_hook(psxport_rtp_fn fn)
{
  s_rtp_fn = fn;
  psxport_rtp_capture = (fn != nullptr) ? 1 : 0;
}

namespace {
psxport_mvmva_fn s_mvmva_fn = nullptr;
}

extern "C" void psxport_on_mvmva(int32_t vx, int32_t vy, int32_t vz, int32_t mac1, int32_t mac2, int32_t mac3)
{
  if (s_mvmva_fn)
    s_mvmva_fn(vx, vy, vz, mac1, mac2, mac3);
}

void psxport_set_mvmva_hook(psxport_mvmva_fn fn)
{
  s_mvmva_fn = fn;
  psxport_mvmva_capture = (fn != nullptr) ? 1 : 0;
}

extern "C" void psxport_on_gpu_poly(uint32_t cc, const uint32_t* cb, int32_t off_x, int32_t off_y)
{
  if (s_gpu_poly_fn)
    s_gpu_poly_fn(cc, cb, off_x, off_y);
}

void psxport_set_gpu_poly_hook(psxport_gpu_poly_fn fn)
{
  s_gpu_poly_fn = fn;
  psxport_gpu_capture = (fn != nullptr) ? 1 : 0;
}

namespace {
psxport_gpu_flip_fn s_gpu_flip_fn = nullptr;
}

extern "C" void psxport_on_gpu_flip(uint32_t value)
{
  if (s_gpu_flip_fn)
    s_gpu_flip_fn(value);
}

void psxport_set_gpu_flip_hook(psxport_gpu_flip_fn fn)
{
  s_gpu_flip_fn = fn;
}

int psxport_bios_log = 0;

extern "C" int psxport_on_pc(uint32_t pc, uint32_t instr, uint32_t* gpr, uint32_t* redirect_pc)
{
  const uint32_t prev_pc = psxport_last_pc; // PC of the instruction that jumped here
  psxport_last_pc = pc;
  if (psxport_prof)
  {
    s_prof_hist[pc]++;
    s_prof_total++;
  }
  pc &= 0x1FFFFFFF; // KSEG-agnostic

  // RE aid: PSXPORT_TRAP_LOWPC=1 — fire once when execution derails below RAM
  // text (a jr/jalr to a null/garbage target), dumping the culprit jump (prev_pc)
  // and the full register file so the bad target register + its source is visible.
  // Excludes the legitimate low vectors (syscall/break/exception/A0/B0/C0).
  {
    static int s_trap = -1;
    if (s_trap < 0)
      s_trap = std::getenv("PSXPORT_TRAP_LOWPC") ? 1 : 0;
    if (s_trap && pc < 0x10000 && pc != 0x40 && pc != 0x80 && pc != 0xA0 &&
        pc != 0xB0 && pc != 0xC0 && pc != 0x180)
    {
      static int s_done = 0;
      if (!s_done)
      {
        s_done = 1;
        static const char* nm[34] = {"zr","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
                                     "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra","lo","hi"};
        std::fprintf(stderr, "[trap] DERAIL -> pc=%08X  jumped from prev=%08X\n", psxport_last_pc, prev_pc);
        for (int i = 0; i < 32; i += 4)
          std::fprintf(stderr, "  %s=%08X %s=%08X %s=%08X %s=%08X\n", nm[i], gpr[i], nm[i+1], gpr[i+1],
                       nm[i+2], gpr[i+2], nm[i+3], gpr[i+3]);
      }
    }
  }
  if (psxport_bios_log && (pc == 0xA0 || pc == 0xB0 || pc == 0xC0))
  {
    // $t1 = GPR[9] holds the function number; a0..a3 = GPR[4..7].
    const char tbl = (pc == 0xA0) ? 'A' : (pc == 0xB0) ? 'B' : 'C';
    const uint32_t fn = gpr[9];
    static char s_last = 0;
    static uint32_t s_last_fn = 0xFFFFFFFF, s_last_a0 = 0;
    if (!(tbl == s_last && fn == s_last_fn && gpr[4] == s_last_a0))
    {
      fprintf(stderr, "[bios f%u] %c0(%02X) a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X\n",
              psxport_frame, tbl, fn, gpr[4], gpr[5], gpr[6], gpr[7], gpr[31]);
      s_last = tbl; s_last_fn = fn; s_last_a0 = gpr[4];
    }
  }
  for (const Hook& h : s_hooks)
  {
    if (h.pc != pc)
      continue;
    if (h.expected_instr != 0 && h.expected_instr != instr)
      continue; // different overlay resident at this address
    return h.fn(pc, gpr, redirect_pc);
  }
  return PSXPORT_HOOK_CONTINUE;
}

void psxport_add_hook(uint32_t pc, uint32_t expected_instr, psxport_hook_fn fn)
{
  s_hooks.push_back({pc & 0x1FFFFFFF, expected_instr, fn});
  psxport_hook_count = static_cast<uint32_t>(s_hooks.size());
}

void psxport_dump_cpu_state(const uint8_t* ram)
{
  const uint32_t* gpr = psxport_cpu_gpr();
  static const char* names[34] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
                                  "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                  "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra", "lo", "hi"};
  fprintf(stderr, "=== emulated CPU state: pc=%08X ===\n", psxport_last_pc);
  for (int i = 0; i < 32; i += 4)
  {
    fprintf(stderr, "  %s=%08X %s=%08X %s=%08X %s=%08X\n", names[i], gpr[i], names[i + 1], gpr[i + 1], names[i + 2],
            gpr[i + 2], names[i + 3], gpr[i + 3]);
  }
  fprintf(stderr, "  ra=%08X is the immediate caller; stack scan:\n", gpr[31]);
  const uint32_t sp = gpr[29] & 0x1FFFFF;
  int found = 0;
  for (uint32_t off = 0; off < 0x800 && found < 16; off += 4)
  {
    uint32_t w;
    if (sp + off + 4 > 0x200000)
      break;
    memcpy(&w, ram + sp + off, 4);
    // plausible return address: word-aligned RAM text, preceded by jal/jalr
    if ((w & 0xF0000003) == 0x80000000 && (w & 0x1FFFFF) >= 0x8000 && (w & 0x1FFFFF) < 0x1FFFF8)
    {
      uint32_t prev;
      memcpy(&prev, ram + ((w - 8) & 0x1FFFFF), 4);
      const bool is_jal = (prev >> 26) == 3;
      const bool is_jalr = ((prev >> 26) == 0) && ((prev & 0x3F) == 9);
      if (is_jal || is_jalr)
      {
        char target[16] = "reg";
        if (is_jal)
          snprintf(target, sizeof(target), "%08X", ((w - 8) & 0xF0000000) | ((prev & 0x3FFFFFF) << 2));
        fprintf(stderr, "    sp+%03X: ret=%08X  (caller %s -> %s)\n", off, w, is_jal ? "jal" : "jalr", target);
        found++;
      }
    }
  }
  if (!found)
    fprintf(stderr, "    (no plausible return addresses in first 2KB of stack)\n");
}

void psxport_clear_hooks()
{
  s_hooks.clear();
  psxport_hook_count = 0;
}
