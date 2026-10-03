#include "field_turn.h"

#include "c_subsys.h" // watchdog_resume
#include "config_var.h"
#include "core.h"
#include "dbg_server.h"
#include "game.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <lucent/log.h>

namespace psx {
namespace {

// `PSXPORT_RAMDUMP_FRAME` asks for a RAM dump at native frame N. The two knob values are held in
// const references, not `const char *` into a temporary: `TextVar::get()` returns by VALUE, so a
// `const char *` bound to `.c_str()` of the temporary would dangle before the first use. A const
// reference to a prvalue lifetime-EXTENDS the temporary, which is both the safe form and the copy-free
// one the style check requires.
const std::string &ramDumpPathFor(std::string &fallback) {
  const std::string &requested = psx::config::cv_ramdump.get();
  if (requested.empty()) {
    fallback = "scratch/bin/midrun_ram.bin";
    return fallback;
  }
  return requested;
}

} // namespace

void FieldTurn::beginField(Core &core) const {
  if (!core.game) {
    lucent::error("field-turn", "the field turn has no bound Game; refusing to pump host services");
    std::abort();
  }
  // The pause is answered BEFORE the watchdog is re-armed: honourPause can idle the machine for as long
  // as the client stays frozen, and an idle field must not spend the frame timeout. DbgServer
  // suspends the watchdog itself across that idle, so this call is the re-arm the frame owes.
  core.game->dbg_server.honourPause(&core);
  watchdog_resume();
}

void FieldTurn::endField(Core &core, std::uint32_t frame) const {
  dumpRamIfRequested(core, frame);
  core.game->dbg_server.service(&core);
}

void FieldTurn::dumpRamIfRequested(Core &core, std::uint32_t frame) const {
  const std::string &requestedFrame = psx::config::cv_ramdump_frame.get();
  if (requestedFrame.empty() || frame != static_cast<std::uint32_t>(strtoul(requestedFrame.c_str(), nullptr, 0))) {
    return;
  }
  std::string fallback;
  const std::string &path = ramDumpPathFor(fallback);
  FILE *dump = fopen(path.c_str(), "wb");
  if (dump == nullptr) {
    lucent::error("field-turn",
                  "mid-run RAM dump @frame {} could not open {} — the path is relative to the working "
                  "directory, which for an agent run is the repository root",
                  frame,
                  path);
    return;
  }
  fwrite(core.ram, 1, 0x200000, dump);
  fclose(dump);
  lucent::info("field-turn", "mid-run RAM dump @frame {} -> {}", frame, path);
}

} // namespace psx