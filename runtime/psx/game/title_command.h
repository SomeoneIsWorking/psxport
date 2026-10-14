// title_command.h — one control-channel line answered by the title, its reply captured as text.
#pragma once

#include <string>

class Core;

namespace psx::game {

struct TitleCommandReply {
  bool handled = false;
  // The title's reply, without the trailing newline; empty when the stream could not be opened.
  std::string text;
};

// GameRuntime::titleCommand for `line` with its surrounding whitespace dropped; its first word is the command.
TitleCommandReply runTitleCommand(Core &core, const std::string &line);

} // namespace psx::game
