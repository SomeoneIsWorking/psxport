// interp_diagnostics.cpp — trace-file and native-call diagnostics for the flat interpreter.
#include "interp_diagnostics.h"
#include "cfg.h"
#include "interp_diag.h"
#include <lucent/log.h>
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

// ── Dispatch-decision ring dump (crashbash-0018) ──────────────────────────────────────────────
// Emitted at a fatal recomp-MISS: pairs the MISS marker against the decision that preceded it, so
// the log names the router branch (or the bypass) that actually produced the miss instead of the
// canned — and for 0x80012840 provably wrong — explanation in the miss diagnostic itself.
void InterpDiag::dumpDispdec() const {
  auto kindName = [](uint32_t k) -> const char * {
    switch (k) {
    case DISPDEC_ENTER:
      return "ENTER";
    case DISPDEC_MAIN:
      return "MAIN";
    case DISPDEC_LIVE:
      return "LIVE";
    case DISPDEC_FIXED:
      return "FIXED";
    case DISPDEC_AMBIG:
      return "AMBIG";
    case DISPDEC_OVERRIDE:
      return "OVERRIDE";
    case DISPDEC_MISSDROP:
      return "MISSDROP";
    case DISPDEC_MISS:
      return "MISS";
    default:
      return "?";
    }
  };
  lucent::info("dispdec", "dispatch-decision ring, oldest first, {} of {} slots:", dispdec_n, DISPDEC_CAP);
  for (int i = 0; i < dispdec_n; i++) {
    const int slot = (dispdec_pos + DISPDEC_CAP - dispdec_n + i) % DISPDEC_CAP;
    const DispDecision &e = dispdec[slot];
    lucent::info("dispdec",
                 "  [{}] {} addr=0x{:08X} ra=0x{:08X}{}",
                 i,
                 kindName(e.kind),
                 e.addr,
                 e.ra,
                 e.aux ? lucent::format(" aux=0x{:08X}", e.aux) : std::string());
  }
}
