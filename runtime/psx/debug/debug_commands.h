// debug_commands.h — the control verbs every debug transport shares: one table over one Core, answering
// through the transport's line sink. The REPL and the debug server keep only their own run-control verbs.
#pragma once

#include "command_args.h"
#include "host_input.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

class Core;

namespace psx::debug {

// One reply line to the transport's client.
using ReplySink = std::function<void(std::string_view)>;

class DebugCommands {
public:
  // True when the command word is a shared verb or the title's command; the reply went to `reply`.
  bool run(Core &core, const CommandArgs &args, const ReplySink &reply);
  // False for a shared verb that acts on the window, host input or configuration; a title's own verb acts on its
  // machine.
  static bool reachesMachine(std::string_view name);
  // `transportVerbs`, every shared verb, then the title's commands, one line each.
  static void help(const Core &core, std::span<const VerbUsage> transportVerbs, const ReplySink &reply);

  struct Call {
    Core &core;
    const CommandArgs &args;
    const ReplySink &reply;
    DebugCommands &commands;
  };
};

} // namespace psx::debug
