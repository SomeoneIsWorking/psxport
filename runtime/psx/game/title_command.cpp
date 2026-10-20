#include "title_command.h"

#include "command_args.h"
#include "core.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>

namespace psx::game {

TitleCommandReply runTitleCommand(Core &core, const psx::debug::CommandArgs &args) {
  const std::string command = args.text(0);
  char *buffer = nullptr;
  size_t length = 0;
  std::FILE *reply = open_memstream(&buffer, &length);
  if (reply == nullptr) {
    lucent::error("title-command", "no reply stream for '{}'", command);
    return {};
  }
  TitleCommandReply result;
  result.handled = core.runtime.titleCommand(core, {.command = command.c_str(), .args = args}, reply);
  std::fclose(reply);
  result.text.assign(buffer != nullptr ? buffer : "", length);
  std::free(buffer);
  while (!result.text.empty() && (result.text.back() == '\n' || result.text.back() == '\r')) {
    result.text.pop_back();
  }
  return result;
}

} // namespace psx::game
