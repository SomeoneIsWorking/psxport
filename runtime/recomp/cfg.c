// Centralized PSXPORT_* config. See cfg.h. One env read per name, cached; the debug channel set drives all
// diagnostic channels so the project no longer needs a separate env var per debug print.
#include "cfg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

extern char** environ;

#define CFG_MAX 192
static struct { const char* name; char kind; int ival; const char* sval; } s_cache[CFG_MAX];
static int s_n = 0;

static int find(const char* name, char kind) {
  for (int i = 0; i < s_n; i++)
    if (s_cache[i].kind == kind && strcmp(s_cache[i].name, name) == 0) return i;
  return -1;
}
static int put(const char* name, char kind, int ival, const char* sval) {
  if (s_n < CFG_MAX) { s_cache[s_n] = (typeof(s_cache[0])){ name, kind, ival, sval }; s_n++; }
  return ival;
}

int cfg_on(const char* name) {
  int i = find(name, 'b'); if (i >= 0) return s_cache[i].ival;
  const char* e = getenv(name);
  return put(name, 'b', (e && strcmp(e, "0") != 0) ? 1 : 0, 0);
}
int cfg_int(const char* name, int def) {
  int i = find(name, 'i'); if (i >= 0) return s_cache[i].ival;
  const char* e = getenv(name);
  return put(name, 'i', e ? atoi(e) : def, 0);
}
const char* cfg_str(const char* name) {
  int i = find(name, 's'); if (i >= 0) return s_cache[i].sval;
  const char* e = getenv(name);
  put(name, 's', 0, e);
  return e;
}

// PSXPORT_ORACLE — pure PSX reference mode. Read once; dedicated accessor (vs cfg_on at the call site)
// so hot render gates can consult it by a named predicate without a per-call name scan.
int oracle_mode(void) {
  static int v = -1;
  if (v < 0) v = cfg_on("PSXPORT_ORACLE");
  return v;
}

// --- Debug channel set — set at runtime via the REPL `debug <chanA,chanB,...|all>` command (no env) ---
// Diagnostics are driven from the REPL, not an env var: `debug scene,stage` enables those channels,
// `debug all` enables everything, `debug` (empty) clears. cfg_dbg(chan) is then true while enabled.
static int         s_dbg_all = 0, s_dbg_nch = 0;
static char        s_dbg_buf[1024];
static const char* s_dbg_ch[96];
void cfg_dbg_set(const char* chans) {
  s_dbg_all = 0; s_dbg_nch = 0; s_dbg_buf[0] = 0;
  if (!chans || !*chans) return;
  if (!strcmp(chans, "all") || !strcmp(chans, "1")) { s_dbg_all = 1; return; }
  strncpy(s_dbg_buf, chans, sizeof s_dbg_buf - 1);
  for (char* p = strtok(s_dbg_buf, ",: "); p && s_dbg_nch < 96; p = strtok(0, ",: "))
    s_dbg_ch[s_dbg_nch++] = p;
}
int cfg_dbg(const char* chan) {
  // Lazy one-time seed from the PSXPORT_DEBUG env var. The header + docs/config.md document cfg_dbg as
  // "driven by PSXPORT_DEBUG=chanA,chanB,… (or =all)", but nothing wired that env in (channels were only
  // settable via the REPL/debug-server `debug` command) — so headless/SBS runs couldn't enable a channel.
  // Seed once here so the documented env actually works; a later REPL `debug …` (cfg_dbg_set) overrides it.
  static int s_env_seeded = 0;
  if (!s_env_seeded) { s_env_seeded = 1; const char* e = getenv("PSXPORT_DEBUG"); if (e && *e) cfg_dbg_set(e); }
  if (s_dbg_all) return 1;
  for (int i = 0; i < s_dbg_nch; i++) if (!strcmp(s_dbg_ch[i], chan)) return 1;
  return 0;
}

// --- PC-enhancement gate (USER 2026-07-16) -------------------------------------------------------
// The third behavior class next to pc_render (no guest writes) and pc_skip (multi-step collapse):
// enhancements DELIBERATELY change meaningful guest state (expanded object load/unload, faster
// fades/transitions, ...). One env var carries them all: PSXPORT_ENH=<name,name|all>. The gate is
// force-suppressed under any oracle-compare run (PSXPORT_ORACLE, or SBS active) so a stray .env
// can never clobber a byte-compare — suppression is central here, not per-call-site discipline.
static int         s_enh_all = 0, s_enh_n = 0, s_enh_seeded = 0, s_enh_suppressed = 0;
static char        s_enh_buf[512];
static const char* s_enh[64];
int cfg_enh(const char* name) {
  if (!s_enh_seeded) {
    s_enh_seeded = 1;
    const char* e = cfg_str("PSXPORT_ENH");
    if (e && *e) {
      if (oracle_mode() || cfg_on("PSXPORT_SBS") || cfg_str("PSXPORT_SBS_MODE")) {
        s_enh_suppressed = 1;
        fprintf(stderr, "[cfg] PSXPORT_ENH=%s SUPPRESSED: oracle/SBS run must stay enhancement-free\n", e);
      } else if (!strcmp(e, "all")) {
        s_enh_all = 1;
      } else {
        strncpy(s_enh_buf, e, sizeof s_enh_buf - 1);
        for (char* p = strtok(s_enh_buf, ",: "); p && s_enh_n < 64; p = strtok(0, ",: "))
          s_enh[s_enh_n++] = p;
      }
    }
  }
  if (s_enh_suppressed) return 0;
  if (s_enh_all) return 1;
  for (int i = 0; i < s_enh_n; i++) if (!strcmp(s_enh[i], name)) return 1;
  return 0;
}

// --- Channel-gated diagnostic logger ------------------------------------------------------------
// THE one diagnostic print primitive (USER directive 2026-07-16): replaces the scattered two-step
// `if (cfg_dbg(chan)) fprintf(stderr, "[chan] ...\n", ...)` idiom with one gated call. Prefixes
// "[chan] ", appends the newline itself. Sink is stderr by default; PSXPORT_LOG_FILE=<path>
// redirects every channel to that file (append, opened once, line-buffered so tail -f works).
static FILE* log_sink(void) {
  static FILE* s_sink = 0;
  if (!s_sink) {
    const char* path = cfg_str("PSXPORT_LOG_FILE");
    s_sink = path && *path ? fopen(path, "a") : 0;
    if (s_sink) setvbuf(s_sink, 0, _IOLBF, 0);
    else s_sink = stderr;
  }
  return s_sink;
}
void cfg_logf(const char* chan, const char* fmt, ...) {
  if (!cfg_dbg(chan)) return;
  FILE* f = log_sink();
  fprintf(f, "[%s] ", chan);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  size_t n = strlen(fmt);
  if (!n || fmt[n - 1] != '\n') fputc('\n', f);
}

void cfg_dump(void) {
  static int done = 0; if (done) return; done = 1;
  if (!environ) return;
  int any = 0;
  for (char** e = environ; *e; e++)
    if (!strncmp(*e, "PSXPORT_", 8)) { if (!any) fprintf(stderr, "[cfg] active:"); any = 1; fprintf(stderr, " %s", *e); }
  if (any) fprintf(stderr, "\n");
}
