// guest_packet_pool_windows.cpp — the one place that asks a Core's Game for its declared facts.
//
// Separated from ot_attr.cpp DELIBERATELY, and it is a compile-enforced fence rather than a
// convention: `ot_attr.cpp` includes `core.h` only, which forward-declares `Game`, so any future
// `c->game->…` written there fails to build. That fence is what kept the packet-store path — the
// hottest path in the substrate, reached on every guest memory write — off the whole object graph
// (see the note at the top of ot_attr.cpp about the crash that reaching through `c->game` for a
// frame counter caused). Reading a title's declared facts is once-per-store-miss work, not the hot
// path, so it belongs here.
#include "guest_packet_pool_windows.h"

#include "core.h"
#include "game.h"

const GuestPacketPoolWindows *declaredGuestPacketPoolWindows(const Core &core) {
  // A legacy `GameConfig` game keeps its existing owner: the config's packetPool* fields are read
  // where they always were, and this returns null so the two are never both consulted. The title
  // that still runs on the adapter therefore behaves EXACTLY as before, which is the property that
  // makes adding this seam safe for the ports still on it.
  if (core.cfg) {
    return nullptr;
  }
  const GameRuntime *runtime = core.game ? core.game->runtime : nullptr;
  if (!runtime) {
    return nullptr;
  }
  const GuestPacketPoolWindows *windows = runtime->guestPacketPoolWindows();
  // A declared-but-empty value is not a declaration. Returning it would make the caller report a
  // pool of extent zero and, worse, take the cache key of a real declaration.
  return windows && windows->valid() ? windows : nullptr;
}
