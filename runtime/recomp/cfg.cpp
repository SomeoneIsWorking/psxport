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
//
// ── AND THE CONFIG HALF IS NOW A FRONT END ONTO THE LAYERED CVar SYSTEM ─────────────────────────
// cfg_on / cfg_int / cfg_str used to be `lucent::config::flag/number/text` and nothing else: the
// environment was the only layer, and precedence between the environment, psxport_settings.ini and
// a REPL `debug` command was undocumented because there was nothing to document it against.
// runtime/recomp/config.h now owns that, with an explicit ladder (default < value < env < runtime).
//
// TWO PATHS THROUGH HERE, and the split is the whole compatibility story:
//   * the name IS a declared CVar (runtime/recomp/config_vars.h) — resolve through the full ladder.
//     For a run that only sets the environment, that is the same answer as before, by construction:
//     the CVar's Override layer is bound with the very expression this function used to return.
//   * the name is NOT — fall through to lucent::config exactly as before, and RECORD the read, so
//     the environment audit can tell an un-migrated knob apart from a typo. 190-odd knobs come
//     through here and none of them changes behaviour.
#include "cfg.h"
#include "config.h"
#include "config_vars.h"

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

namespace {

// A declared CVar of the wrong Kind for the accessor being used is a mistake in the inventory, not a
// runtime condition — but silently falling through to the environment would hide it, and a knob that
// half-works is exactly what this system exists to stop. Say it once, then behave as before.
psx::config::CVarBase* declared_as(const char* name, psx::config::Kind want) {
  psx::config::CVarBase* v = psx::config::find(name);
  if (!v) return nullptr;
  if (v->external()) return nullptr;   // lucent owns it; see config_vars.h
  if (v->kind() != want) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      lucent::warn("cfg", "{} is declared as {} but is being read as {} — falling back to the environment",
                   name, psx::config::kind_name(v->kind()), psx::config::kind_name(want));
    }
    return nullptr;
  }
  return v;
}

}  // namespace

int cfg_on(const char* name) {
  if (psx::config::CVarBase* v = declared_as(name, psx::config::Kind::Bool))
    return static_cast<psx::config::BoolVar*>(v)->get() ? 1 : 0;
  const bool on = lucent::config::flag(name);
  psx::config::note_legacy_read(name, psx::config::Kind::Bool, on ? "1" : "0");
  return on ? 1 : 0;
}

int cfg_int(const char* name, int def) {
  if (psx::config::CVarBase* v = declared_as(name, psx::config::Kind::Int)) {
    psx::config::IntVar* iv = static_cast<psx::config::IntVar*>(v);
    // The caller's `def` and the CVar's declared default must agree, or the same knob means two
    // things depending on which call site got there first. That is a build-time mistake; report it
    // rather than picking a winner in silence. The inventory is the authority.
    if ((long)def != iv->default_value()) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        lucent::warn("cfg", "cfg_int({}, {}) disagrees with the declared default {} — using the declared one",
                     name, def, iv->default_value());
      }
    }
    return static_cast<int>(iv->get());
  }
  const long v = lucent::config::number(name, def);
  psx::config::note_legacy_read(name, psx::config::Kind::Int, std::to_string(v));
  return static_cast<int>(v);
}

const char* cfg_str(const char* name) {
  if (psx::config::CVarBase* v = declared_as(name, psx::config::Kind::Text)) {
    const std::string& s = static_cast<psx::config::TextVar*>(v)->get();
    return s.empty() ? nullptr : s.c_str();   // callers test for NULL, not for ""
  }
  const std::string& v = lucent::config::text(name);
  psx::config::note_legacy_read(name, psx::config::Kind::Text, v);
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

// PSXPORT_ORACLE — the pure PSX reference mode. MIGRATED: the hand-rolled `static int v = -1` cache
// is gone; the CVar binds the environment once and every enhancement gate reads the same object, so
// `report()` can say what oracle mode resolved to and from which layer.
int oracle_mode(void) { return psx::config::cv_oracle.get() ? 1 : 0; }

// PSXPORT_ENH=<name,name|all> — the sanctioned enhancement class, MIGRATED onto the CVar ladder
// (Tomba2Engine kanban #92). The body used to live here: it read lucent::config::text("PSXPORT_ENH")
// into a function-local SEEDED STATIC and applied the ORACLE/SBS suppression itself. Three things were
// wrong with that and all three are gone — no Value/Runtime layer and no row in the CVar dump or the
// environment audit; a one-shot seeding that froze the answer on the first call; and one global warning
// naming the raw PSXPORT_ENH string, so a run with two enhancements set could not name both.
//
// This is now a C-callable forwarder onto psx::config::enh_named(), which is the ONE definition of the
// suppression rule (psx::config::compare_run) — a game declaring its enhancements as its own CVars
// reaches the same rule through psx::config::enh(). Two copies of "what a byte-compare run IS" is the
// worst possible duplication: diverge, and one of them fails to recognise an SBS variant while the
// contaminated compare still looks clean.
int cfg_enh(const char* name) {
  bootstrap_once();
  return psx::config::enh_named(name) ? 1 : 0;
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
  // Print the raw list even when it is EMPTY. "active:" with nothing after it says "this run was
  // configured with no PSXPORT_* variables at all", which is a fact; printing nothing says only that
  // this function may not have run.
  lucent::log(lucent::Level::Info, "cfg", line.empty() ? "active: (no PSXPORT_* variables set)"
                                                       : "active:" + line);
  // ...and then the part the raw list cannot give you: what each knob RESOLVED to, which layer it
  // came from, and which variables in that list matched nothing at all.
  psx::config::report_once();
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
