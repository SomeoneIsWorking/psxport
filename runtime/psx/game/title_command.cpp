#include "title_command.h"

#include "core.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>

namespace psx::game {

TitleCommandReply runTitleCommand(Core &core, const std::string &line) {
  constexpr const char *kSpace = " \t\r\n";
  const size_t first = line.find_first_not_of(kSpace);
  const size_t last = line.find_last_not_of(kSpace);
  const std::string trimmed = first == std::string::npos ? std::string() : line.substr(first, last - first + 1);
  const std::string command = trimmed.substr(0, trimmed.find_first_of(kSpace));
  char *buffer = nullptr;
  size_t length = 0;
  std::FILE *reply = open_memstream(&buffer, &length);
  if (reply == nullptr) {
    lucent::error("title-command", "no reply stream for '{}'", command);
    return {};
  }
  TitleCommandReply result;
  result.handled = core.runtime.titleCommand(core, {.command = command.c_str(), .line = trimmed.c_str()}, reply);
  std::fclose(reply);
  result.text.assign(buffer != nullptr ? buffer : "", length);
  std::free(buffer);
  while (!result.text.empty() && (result.text.back() == '\n' || result.text.back() == '\r')) {
    result.text.pop_back();
  }
  return result;
}

} // namespace psx::game
