#include "picker_runtime.h"

#include "picker_composite.h"

#include <format>
#include <string_view>

namespace psx::host {
bool PickerRuntime::titleCommand(Core &, const psx::game::TitleCommandLine &input, FILE *out) {
  return handle(input.args, out);
}

void PickerRuntime::setSelection(int selected) {
  selected_ = selected;
}

bool PickerRuntime::handle(const psx::debug::CommandArgs &args, FILE *out) {
  if (!content_) {
    return false;
  }
  const std::string_view cmd = args.word(0);
  if (cmd == "picker") {
    // The composited frame — every panel, dividers and screen text — since no single
    // session's present shot can see a frame the host assembled from three.
    if (args.word(1) == "shot") {
      const std::string path(args.rest(2));
      if (path.empty() || composite_ == nullptr) {
        std::fprintf(out, "refused: picker shot needs a path and a composited frame\n");
        return true;
      }
      composite_->captureShot(path.c_str());
      std::fprintf(out, "ok: picker shot %s\n", path.c_str());
      return true;
    }
    std::fputs(content_->listing().c_str(), out);
    return true;
  }
  if (cmd == "pick") {
    const std::string slug(args.rest(1));
    std::string refusal;
    if (const TitleAvailability *title = content_->findAvailable(slug, refusal)) {
      pendingSlug_ = std::string(title->identity->slug);
      const std::string answer = std::format("ok: starting {}\n", *pendingSlug_);
      std::fputs(answer.c_str(), out);
    } else {
      const std::string answer = std::format("refused: {}\n", refusal);
      std::fputs(answer.c_str(), out);
    }
    return true;
  }
  if (cmd == "select") {
    const std::string_view argument = args.rest(1);
    const int panelCount = content_->panelCount();
    if (panelCount <= 0) {
      std::fprintf(out, "refused: there are no panels to select\n");
      return true;
    }
    int panel = -1;
    if (argument == "left" || argument == "right") {
      // The pad's own wrap (psx::ui::ChoiceNavigator), so the channel and the D-pad cannot
      // disagree about where "right" lands at the end of the row.
      const int step = argument == "right" ? 1 : -1;
      panel = (selected_ + step + panelCount) % panelCount;
    } else {
      panel = content_->panelOf(argument);
      if (panel < 0) {
        std::fprintf(out, "refused: no panel shows '%.*s'\n", static_cast<int>(argument.size()), argument.data());
        return true;
      }
    }
    pendingSelection_ = panel;
    // The slug is a string_view, not a C string: formatted rather than handed to a %s.
    const std::string line_out = std::format("ok: panel {} ({})\n", panel, content_->panelTitle(panel).identity->slug);
    std::fputs(line_out.c_str(), out);
    return true;
  }
  return false;
}

} // namespace psx::host
