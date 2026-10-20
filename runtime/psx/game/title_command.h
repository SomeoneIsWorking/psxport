// title_command.h — one control-channel line answered by the title, its reply captured as text.
#pragma once

#include <string>

class Core;

namespace psx::debug {
class CommandArgs;
}

namespace psx::game {

struct TitleCommandReply {
  bool handled = false;
  // The title's reply, without the trailing newline; empty when the stream could not be opened.
  std::string text;
};

// GameRuntime::titleCommand for `args`; its first word is the command.
TitleCommandReply runTitleCommand(Core &core, const psx::debug::CommandArgs &args);

} // namespace psx::game
