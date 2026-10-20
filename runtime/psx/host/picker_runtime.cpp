#include "picker_runtime.h"

#include <format>
#include <string_view>

namespace psx::host {

bool PickerRuntime::titleCommand(Core &, const psx::game::TitleCommandLine &input, FILE *out) {
  if (!content_) {
    return false;
  }
  const psx::debug::CommandArgs &args = input.args;
  const std::string_view cmd = args.word(0);
  if (cmd == "picker") {
    std::fputs(content_->listing().c_str(), out);
    return true;
  }
  if (cmd == "pick") {
    const std::string slug(args.rest(1));
    std::string refusal;
    if (const TitleAvailability *title = content_->findAvailable(slug, refusal)) {
      pendingSlug_ = std::string(title->identity->slug);
      std::fputs(std::format("ok: starting {}\n", *pendingSlug_).c_str(), out);
    } else {
      std::fputs(std::format("refused: {}\n", refusal).c_str(), out);
    }
    return true;
  }
  if (cmd == "select") {
    const std::string_view argument = args.rest(1);
    if (content_->panelCount() <= 0) {
      std::fputs("refused: there are no panels to select\n", out);
      return true;
    }
    if (argument == "left" || argument == "right") {
      pendingStep_ += argument == "right" ? 1 : -1;
      std::fputs(std::format("ok: step {}\n", argument).c_str(), out);
      return true;
    }
    const int panel = content_->panelOf(argument);
    if (panel < 0) {
      std::fputs(std::format("refused: no panel shows '{}'\n", argument).c_str(), out);
      return true;
    }
    pendingPanel_ = panel;
    std::fputs(std::format("ok: panel {} ({})\n", panel, content_->panelTitle(panel).identity->slug).c_str(), out);
    return true;
  }
  return false;
}

} // namespace psx::host
