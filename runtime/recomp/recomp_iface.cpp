// recomp_iface.cpp — framework-side storage for the framework↔generated-substrate seam
// (recomp_iface.h). Holds the process-global RecompRegistry the game installs at startup. Names no
// generated symbols (game-agnostic) — the game supplies the pointers.
#include "recomp_iface.h"

static const RecompRegistry* g_recomp = nullptr;

void                  psxport_install_recomp(const RecompRegistry* r) { g_recomp = r; }
const RecompRegistry* psxport_recomp()                                { return g_recomp; }
