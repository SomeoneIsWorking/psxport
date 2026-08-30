// recomp_iface.cpp — framework-side storage for the framework↔generated-substrate seam
// (recomp_iface.h). Holds the process-global RecompRegistry the game installs at startup. Names no
// generated symbols (game-agnostic) — the game supplies the pointers.
#include "recomp_iface.h"

#include <lucent/log.h>

static const RecompRegistry *g_recomp = nullptr;

void psxport_install_recomp(const RecompRegistry *r) {
  g_recomp = r;
  if (r && r->substrate_id && r->substrate_id[0]) {
    lucent::info("recomp", "generated substrate identity: {}", r->substrate_id);
  } else {
    lucent::warn("recomp",
                 "generated substrate identity: UNKNOWN — regenerate the substrate and expose "
                 "g_rec_substrate_id through RecompRegistry before using this run as evidence");
  }
}
const RecompRegistry *psxport_recomp() {
  return g_recomp;
}
