// debug_commands.h — the control verbs every debug transport shares: one table over one Core, answering
// through the transport's line sink. The REPL and the debug server keep only their own run-control verbs.
#pragma once

#include "command_args.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

class Core;

namespace psx::debug {

// One reply line to the transport's client.
using ReplySink = std::function<void(std::string_view)>;

// A transport's own verb, listed by `help` before the shared table.
struct VerbUsage {
  std::string_view usage;
  std::string_view summary;
};

class DebugCommands {
public:
  // True when the command word is a shared verb or the title's command; the reply went to `reply`.
  bool run(Core &core, const CommandArgs &args, const ReplySink &reply);
  // `transportVerbs`, then every shared verb, one line each.
  static void help(std::span<const VerbUsage> transportVerbs, const ReplySink &reply);

  struct Call {
    Core &core;
    const CommandArgs &args;
    const ReplySink &reply;
    DebugCommands &commands;
  };

  // Active-low; press, release and hold edit it.
  std::uint16_t heldMask = 0xFFFF;
};

} // namespace psx::debug
