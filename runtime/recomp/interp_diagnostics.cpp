// interp_diagnostics.cpp — trace-file and native-call diagnostics for the flat interpreter.
#include "interp_diagnostics.h"
#include "cfg.h"
#include <stdio.h>

void interp_trace_open(Core *core, const char *path) {
  FILE *&fp = core->idiag.trace_fp;
  if (path && *path) {
    fp = fopen(path, "w");
    if (!fp) {
      perror(path);
    } else {
      setvbuf(fp, 0, _IOLBF, 0);
    }
  } else if (fp) {
    fclose(fp);
    fp = nullptr;
  }
}

void interp_trace_call(InterpDiag &diag, uint32_t from, uint32_t to) {
  if (diag.trace_fp) {
    fprintf(diag.trace_fp, "%08X -> %08X\n", from, to);
  }
}

void interp_ncall_open_once(InterpDiag &diag) {
  if (diag.ncall_init) {
    return;
  }
  diag.ncall_init = 1;
  const char *path = cfg_str("PSXPORT_NCALL_TRACE");
  if (path && *path) {
    diag.ncall_fp = fopen(path, "w");
    if (!diag.ncall_fp) {
      perror(path);
    } else {
      setvbuf(diag.ncall_fp, 0, _IOLBF, 0);
    }
  }
}

void interp_ncall_log(InterpDiag &diag,
                      char kind,
                      uint32_t target,
                      uint32_t a0,
                      uint32_t a1,
                      uint32_t a2,
                      uint32_t a3,
                      uint32_t v0,
                      uint32_t v1) {
  if (!diag.ncall_fp) {
    return;
  }
  fprintf(diag.ncall_fp,
          "%ld %c %08X  a:%08X %08X %08X %08X -> v:%08X %08X\n",
          diag.ncall_seq++,
          kind,
          target,
          a0,
          a1,
          a2,
          a3,
          v0,
          v1);
}
