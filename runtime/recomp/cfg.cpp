// cfg — the framework's configuration + diagnostics front end, implemented on top of `lucent`
// (https://github.com/SomeoneIsWorking/lucent).
//
// WHY A SHIM AND NOT A REWRITE: the cfg_* C API is called from ~1000 sites, several of them in plain
// C translation units (xa_stream.c, disc.c, spu_beetle.c) that cannot use lucent's C++ templates.
// Keeping this surface means the whole port gains lucent's single output path — file redirection,
// test capture, per-channel silencing — without a thousand-site edit whose only product would be
// churn. New C++ code should call lucent directly; this file is the bridge for what already exists.
//
// The mapping is one-to-one:
//   cfg_on/cfg_int/cfg_str  -> lucent::config::flag/number/text
//   cfg_dbg/cfg_dbg_set     -> lucent::channel_on/enable_channels
//   cfg_logf                -> lucent debug level (channel-gated)
//   cfg_logi/logw/loge      -> lucent info/warn/error
//   CfgLine                 -> the same accumulate-then-flush shape as lucent::Line
// printf-style formatting is preserved here (vsnprintf into a buffer, then hand lucent the finished
// text) because every existing call site is printf-style; lucent itself is std::format-based.
#include "cfg.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// PUBLIC and at GLOBAL scope so cfg.h can read it INLINE — defining it inside the anonymous namespace
// below gives it internal linkage and makes every reference ambiguous against cfg.h's extern. It
// starts at 0 and bootstrap_once() makes it non-zero, which doubles as the "bootstrapped yet?" flag,
// so an inline reader takes the fast path on any non-zero value and falls back exactly once.
unsigned cfg_dbg_gen_v = 0;

namespace {

// Formats a printf-style call into a bounded buffer. Truncation marks itself rather than silently
// losing the tail, so a too-long diagnostic is visibly cut instead of quietly wrong.
std::string vformat(const char* fmt, va_list ap) {
  char buf[4096];
  const int n = vsnprintf(buf, sizeof buf, fmt, ap);
  if (n < 0) return std::string();
  if (static_cast<size_t>(n) >= sizeof buf) {
    std::string out(buf, sizeof buf - 1);
    out.append("...");
    return out;
  }
  return std::string(buf, static_cast<size_t>(n));
}

// Bumped whenever the enabled-channel SET changes; hot paths cache cfg_dbg() against it. Starts at 0
// and must become non-zero once, which is all bootstrap_once() is still for.
#define s_dbg_gen cfg_dbg_gen_v

// PSXPORT_DEBUG and PSXPORT_LOG_FILE are read by LUCENT ITSELF — cmake/psxport.cmake builds it with
// LUCENT_CHANNEL_ENV / LUCENT_LOG_FILE_ENV set to those names, and lucent resolves them lazily on its
// first log call. This function used to load them here instead, and that was a time bomb: it is
// reachable only from a cfg_* entry point, so a plain lucent::debug() never triggered it and the two
// variables worked purely because ~700 legacy cfg_log* sites happen to fire during boot. Retiring
// those would have turned every diagnostic channel in four repos off with no message. The load lives
// where it cannot be skipped now; tests/test_lucent_channel_env.cpp is the gate.
void bootstrap_once() {
  static bool done = false;
  if (done) return;
  done = true;
  s_dbg_gen++;
}

void emit(lucent::Level level, const char* chan, const char* fmt, va_list ap) {
  bootstrap_once();
  lucent::log(level, chan ? chan : "?", vformat(fmt, ap));
}

}  // namespace

extern "C" {

int cfg_on(const char* name) { return lucent::config::flag(name) ? 1 : 0; }

int cfg_int(const char* name, int def) {
  return static_cast<int>(lucent::config::number(name, def));
}

const char* cfg_str(const char* name) {
  const std::string& v = lucent::config::text(name);
  return v.empty() ? nullptr : v.c_str();   // callers test for NULL, not for ""
}

int cfg_dbg(const char* chan) {
  bootstrap_once();
  return lucent::channel_on(chan) ? 1 : 0;
}

// Defined OUTSIDE the anonymous namespace above: cfg.h declares it extern "C", so a definition inside
// that namespace would be a different, internally-linked function and the link would fail.
unsigned cfg_dbg_generation(void) { bootstrap_once(); return s_dbg_gen; }

void cfg_dbg_set(const char* chans) {
  bootstrap_once();
  lucent::enable_channels(chans ? chans : "");
  s_dbg_gen++;                       // invalidate every hot-path cache of cfg_dbg()
}

void cfg_logf(const char* chan, const char* fmt, ...) {
  bootstrap_once();
  if (!lucent::channel_on(chan)) return;
  va_list ap; va_start(ap, fmt);
  lucent::log(lucent::Level::Debug, chan ? chan : "?", vformat(fmt, ap));
  va_end(ap);
}

void cfg_logi(const char* chan, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); emit(lucent::Level::Info, chan, fmt, ap); va_end(ap);
}
void cfg_logw(const char* chan, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); emit(lucent::Level::Warn, chan, fmt, ap); va_end(ap);
}
void cfg_loge(const char* chan, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt); emit(lucent::Level::Error, chan, fmt, ap); va_end(ap);
}

// PSXPORT_ORACLE — the pure PSX reference mode. Cached once; every enhancement gate consults it.
int oracle_mode(void) {
  static int v = -1;
  if (v < 0) v = cfg_on("PSXPORT_ORACLE");
  return v;
}

// PSXPORT_ENH=<name,name|all> — the sanctioned enhancement class. FORCE-SUPPRESSED under
// PSXPORT_ORACLE or any SBS mode, so a stray .env can never contaminate a byte-compare. That
// suppression is the whole point of this function living in cfg rather than at each call site.
int cfg_enh(const char* name) {
  static bool seeded = false;
  static bool suppressed = false, all = false;
  static std::vector<std::string> names;
  if (!seeded) {
    seeded = true;
    const std::string& e = lucent::config::text("PSXPORT_ENH");
    if (!e.empty()) {
      if (oracle_mode() || cfg_on("PSXPORT_SBS") || !lucent::config::text("PSXPORT_SBS_MODE").empty()) {
        suppressed = true;
        lucent::log(lucent::Level::Warn, "cfg",
                    "PSXPORT_ENH=" + e + " SUPPRESSED: oracle/SBS run must stay enhancement-free");
      } else if (e == "all") {
        all = true;
      } else {
        size_t start = 0;
        while (start <= e.size()) {
          const size_t sep = e.find_first_of(",: ", start);
          const size_t end = (sep == std::string::npos) ? e.size() : sep;
          if (end > start) names.emplace_back(e.substr(start, end - start));
          if (sep == std::string::npos) break;
          start = sep + 1;
        }
      }
    }
  }
  if (suppressed) return 0;
  if (all) return 1;
  for (const std::string& n : names) if (n == name) return 1;
  return 0;
}

void cfg_dump(void) {
  static int done = 0;
  if (done) return;
  done = 1;
  bootstrap_once();
  std::string line;
  for (const std::string& entry : lucent::config::active()) {
    if (entry.rfind("PSXPORT_", 0) != 0) continue;
    line.push_back(' ');
    line.append(entry);
  }
  if (!line.empty()) lucent::log(lucent::Level::Info, "cfg", "active:" + line);
}

// --- CfgLine: the piecewise line accumulator ----------------------------------------------------
// Same accumulate-then-flush shape as lucent::Line, kept as a C struct because C translation units
// construct it directly. The flush is what routes through lucent.
void cfg_line_reset(CfgLine* l) { l->used = 0; l->buf[0] = 0; }

void cfg_line_addf(CfgLine* l, const char* fmt, ...) {
  if (l->used >= sizeof l->buf - 1) return;
  const size_t space = sizeof l->buf - l->used;
  va_list ap; va_start(ap, fmt);
  const int w = vsnprintf(l->buf + l->used, space, fmt, ap);
  va_end(ap);
  if (w < 0) return;
  if (static_cast<size_t>(w) >= space) {
    l->used = static_cast<unsigned>(sizeof l->buf - 1);
    memcpy(l->buf + l->used - 3, "...", 3);
    l->buf[l->used] = 0;
    return;
  }
  l->used += static_cast<unsigned>(w);
}

void cfg_line_flush(CfgLine* l, const char* chan) {
  if (l->used) {
    bootstrap_once();
    lucent::log(lucent::Level::Info, chan ? chan : "?", l->buf);
  }
  cfg_line_reset(l);
}

}  // extern "C"
