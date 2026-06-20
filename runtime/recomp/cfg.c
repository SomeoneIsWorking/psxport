// Centralized PSXPORT_* config. See cfg.h. One env read per name, cached; the debug channel set drives all
// diagnostic channels so the project no longer needs a separate env var per debug print.
#include "cfg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
  if (s_dbg_all) return 1;
  for (int i = 0; i < s_dbg_nch; i++) if (!strcmp(s_dbg_ch[i], chan)) return 1;
  return 0;
}

void cfg_dump(void) {
  static int done = 0; if (done) return; done = 1;
  if (!environ) return;
  int any = 0;
  for (char** e = environ; *e; e++)
    if (!strncmp(*e, "PSXPORT_", 8)) { if (!any) fprintf(stderr, "[cfg] active:"); any = 1; fprintf(stderr, " %s", *e); }
  if (any) fprintf(stderr, "\n");
}
