#include "title_debug_rows.h"

#include "command_args.h"
#include "game.h"
#include "menu_row.h"
#include "psxport_build_id.h"
#include "rml_text.h"
#include "wrap.h"

#include <lucent/log.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace psx::ui {
namespace {

using psx::game::DebugParam;
using psx::game::DebugVerb;

constexpr std::string_view kRowPrefix = "debug:";
constexpr std::string_view kWarpRow = "debug:warp";
constexpr std::string_view kAbsentText = "-";

// `debug:<verb>` or `debug:<verb>:<param>`.
struct RowId {
  std::size_t verb = 0;
  std::optional<std::size_t> param;
};

std::optional<std::size_t> parseIndex(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<RowId> parseRowId(std::string_view id) {
  if (id.substr(0, kRowPrefix.size()) != kRowPrefix) {
    return std::nullopt;
  }
  id.remove_prefix(kRowPrefix.size());
  const std::size_t colon = id.find(':');
  const std::optional<std::size_t> verb = parseIndex(id.substr(0, colon));
  if (!verb) {
    return std::nullopt;
  }
  if (colon == std::string_view::npos) {
    return RowId{*verb, std::nullopt};
  }
  const std::optional<std::size_t> param = parseIndex(id.substr(colon + 1));
  if (!param) {
    return std::nullopt;
  }
  return RowId{*verb, param};
}

// The span a parameter's selection steps over, and its first value.
std::pair<std::int64_t, std::int64_t> stepRange(Core &core, const DebugParam &param) {
  if (param.kind == DebugParam::Kind::Named || param.kind == DebugParam::Kind::Word) {
    return {0, static_cast<std::int64_t>(param.names.count(core)) - 1};
  }
  return {param.min, param.max};
}

std::string selectButton(std::string_view kind, const std::string &id, std::string_view label, std::string_view value) {
  return "<select-button " + std::string(kind) + "=\"" + id + "\"><key>" + rmlTextMarkup(label) + "</key><value>" +
         rmlTextMarkup(value) + "</value></select-button>";
}

std::string paragraph(std::string_view text, bool hint) {
  return std::string(hint ? "<p class=\"hint\">" : "<p>") + rmlTextMarkup(text) + "</p>";
}

} // namespace

class DebugParamBinding final : public RowBinding {
public:
  DebugParamBinding(TitleDebugRows &rows, std::size_t verb, std::size_t param)
      : mRows(rows), mVerb(verb), mParam(param) {}
  bool text(std::string &out) const override {
    out = mRows.valueText(mVerb, mParam);
    return true;
  }
  void step(int dir) override {
    mRows.step(mVerb, mParam, dir);
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  TitleDebugRows &mRows;
  std::size_t mVerb;
  std::size_t mParam;
};

class DebugRunBinding final : public RowBinding {
public:
  DebugRunBinding(TitleDebugRows &rows, std::size_t verb) : mRows(rows), mVerb(verb) {}
  bool text(std::string &out) const override {
    out = mRows.available(mVerb) ? "\xe2\x86\x92" : "unavailable";
    return true;
  }
  void step(int) override {
    mRows.run(mVerb);
  }

private:
  TitleDebugRows &mRows;
  std::size_t mVerb;
};

class WarpLevelBinding final : public RowBinding {
public:
  WarpLevelBinding(TitleDebugRows &rows, std::size_t level) : mRows(rows), mLevel(level) {}
  bool text(std::string &out) const override {
    out = mRows.warpText(mLevel);
    return true;
  }
  void step(int dir) override {
    mRows.warpStep(mLevel, dir);
  }
  bool steps_with_arrows() const override {
    return true;
  }

private:
  TitleDebugRows &mRows;
  std::size_t mLevel;
};

class WarpGoBinding final : public RowBinding {
public:
  explicit WarpGoBinding(TitleDebugRows &rows) : mRows(rows) {}
  bool text(std::string &out) const override {
    out = mRows.warpAvailable() ? "\xe2\x86\x92" : "unavailable";
    return true;
  }
  void step(int) override {
    mRows.warpGo();
  }

private:
  TitleDebugRows &mRows;
};

TitleDebugRows::TitleDebugRows(Game *game, std::function<void(const std::string &)> status)
    : mGame(game), mStatus(std::move(status)) {
  if (const psx::game::TitleDebugMenu *debug = menu()) {
    mWarpAt.assign(debug->warpLevels().size(), 0);
    mSelections.resize(debug->verbs().size());
    for (std::size_t v = 0; v < mSelections.size(); ++v) {
      const DebugVerb &verb = debug->verbs()[v];
      for (std::size_t p = 0; p < verb.params.size(); ++p) {
        mSelections[v][p] = {.value = stepRange(mGame->core, verb.params[p]).first, .present = true};
      }
    }
  }
}

const psx::game::TitleDebugMenu *TitleDebugRows::menu() const {
  return mGame != nullptr ? &mGame->runtime.debugMenu() : nullptr;
}

std::string TitleDebugRows::readoutId(std::size_t index) {
  return "debug_readout_" + std::to_string(index);
}

std::string TitleDebugRows::debugMarkup() const {
  const psx::game::TitleDebugMenu *debug = menu();
  if (debug == nullptr) {
    return {};
  }
  std::string rml;
  bool first = true;
  if (!debug->warpLevels().empty()) {
    rml += "<div class=\"section-heading\">Warp</div>";
    for (std::size_t level = 0; level < debug->warpLevels().size(); ++level) {
      rml += selectButton(
          "adjust", std::string(kWarpRow) + ":" + std::to_string(level), debug->warpLevels()[level], warpText(level));
    }
    rml += selectButton("action", std::string(kWarpRow), "Go", "");
    first = false;
  }
  for (const psx::game::TitleDebugMenu::Section &section : debug->sections()) {
    std::string rows;
    for (const std::size_t r : section.readouts) {
      rows += "<div class=\"readout\" id=\"" + readoutId(r) + "\"></div>";
    }
    for (const std::size_t v : section.verbs) {
      const DebugVerb &verb = debug->verbs()[v];
      if (!verb.inMenu) {
        continue;
      }
      for (std::size_t p = 0; p < verb.params.size(); ++p) {
        rows += selectButton("adjust",
                             std::string(kRowPrefix) + std::to_string(v) + ":" + std::to_string(p),
                             verb.params[p].name,
                             valueText(v, p));
      }
      rows += selectButton("action", std::string(kRowPrefix) + std::to_string(v), verb.summary, "");
    }
    if (rows.empty()) {
      continue;
    }
    rml += std::string("<div class=\"section-heading") + (first ? "" : " spaced") + "\">" +
           rmlTextMarkup(section.heading) + "</div>" + rows;
    first = false;
  }
  if (rml.empty()) {
    rml = paragraph("This title declares no debug routes.", true);
  }
  return rml;
}

std::string TitleDebugRows::aboutMarkup() const {
  if (mGame == nullptr) {
    return {};
  }
  std::string rml;
  if (mGame->title != nullptr) {
    rml += paragraph(lucent::format("{} ({})", mGame->title->displayName, mGame->title->serial), false);
  }
  if (!mGame->productName.empty()) {
    rml += paragraph(lucent::format("{}, a native port on psxport", mGame->productName), mGame->title != nullptr);
  }
  for (const std::string_view line : mGame->runtime.debugMenu().about()) {
    rml += paragraph(line, true);
  }
  rml += paragraph(lucent::format("Build {}", PSXPORT_BUILD_ID_COMPOSITE), true);
  return rml;
}

std::unique_ptr<RowBinding> TitleDebugRows::bind(std::string_view id) {
  const psx::game::TitleDebugMenu *debug = menu();
  if (debug != nullptr && id.substr(0, kWarpRow.size()) == kWarpRow) {
    if (id.size() == kWarpRow.size()) {
      return std::make_unique<WarpGoBinding>(*this);
    }
    const std::optional<std::size_t> level =
        id[kWarpRow.size()] == ':' ? parseIndex(id.substr(kWarpRow.size() + 1)) : std::nullopt;
    if (!level || *level >= mWarpAt.size()) {
      return nullptr;
    }
    return std::make_unique<WarpLevelBinding>(*this, *level);
  }
  const std::optional<RowId> row = parseRowId(id);
  if (!row || debug == nullptr || row->verb >= debug->verbs().size()) {
    return nullptr;
  }
  if (!row->param) {
    return std::make_unique<DebugRunBinding>(*this, row->verb);
  }
  if (*row->param >= debug->verbs()[row->verb].params.size()) {
    return nullptr;
  }
  return std::make_unique<DebugParamBinding>(*this, row->verb, *row->param);
}

std::string TitleDebugRows::valueText(std::size_t verb, std::size_t param) const {
  const DebugParam &spec = menu()->verbs()[verb].params[param];
  const Selection &selection = mSelections[verb][param];
  const std::pair<std::int64_t, std::int64_t> range = stepRange(mGame->core, spec);
  if (!selection.present || range.second < range.first) {
    return std::string(kAbsentText);
  }
  const std::int64_t value = std::clamp(selection.value, range.first, range.second);
  switch (spec.kind) {
  case DebugParam::Kind::Integer:
    return std::to_string(value);
  case DebugParam::Kind::Hex:
    return lucent::format("0x{:X}", value);
  case DebugParam::Kind::Named: {
    const std::string name = spec.names.name(mGame->core, static_cast<int>(value));
    return name.empty() ? std::to_string(value) : lucent::format("{} - {}", value, name);
  }
  case DebugParam::Kind::Word:
    return spec.names.name(mGame->core, static_cast<int>(value));
  case DebugParam::Kind::Text:
    return std::string(kAbsentText);
  }
  return std::string(kAbsentText);
}

void TitleDebugRows::step(std::size_t verb, std::size_t param, int dir) {
  const DebugParam &spec = menu()->verbs()[verb].params[param];
  Selection &selection = mSelections[verb][param];
  const std::pair<std::int64_t, std::int64_t> range = stepRange(mGame->core, spec);
  if (range.second < range.first) {
    return;
  }
  // An optional parameter's absence is one more stop before its first value.
  const std::int64_t stops = range.second - range.first + 1 + (spec.optional ? 1 : 0);
  const std::int64_t at =
      selection.present ? std::clamp(selection.value, range.first, range.second) - range.first + (spec.optional ? 1 : 0)
                        : 0;
  const std::int64_t next = wrap<std::int64_t>(at, dir, stops);
  selection.present = !spec.optional || next > 0;
  selection.value = range.first + next - (spec.optional ? 1 : 0);
}

std::string TitleDebugRows::line(std::size_t verb) const {
  const DebugVerb &spec = menu()->verbs()[verb];
  std::string text(spec.name);
  for (std::size_t p = 0; p < spec.params.size(); ++p) {
    const DebugParam &param = spec.params[p];
    const Selection &selection = mSelections[verb][p];
    const std::pair<std::int64_t, std::int64_t> range = stepRange(mGame->core, param);
    if (!selection.present || range.second < range.first) {
      break;
    }
    const std::int64_t value = std::clamp(selection.value, range.first, range.second);
    switch (param.kind) {
    case DebugParam::Kind::Integer:
    case DebugParam::Kind::Named:
      text += " " + std::to_string(value);
      break;
    case DebugParam::Kind::Hex:
      text += lucent::format(" 0x{:X}", value);
      break;
    case DebugParam::Kind::Word:
      text += " " + param.names.name(mGame->core, static_cast<int>(value));
      break;
    case DebugParam::Kind::Text:
      return text;
    }
  }
  return text;
}

bool TitleDebugRows::available(std::size_t verb) const {
  const DebugVerb &spec = menu()->verbs()[verb];
  return !spec.refusal || spec.refusal(mGame->core).empty();
}

std::span<const psx::game::WarpPlace> TitleDebugRows::warpChoices(std::size_t level) const {
  std::span<const psx::game::WarpPlace> places = menu()->warpPlaces(mGame->core);
  for (std::size_t above = 0; above < level; ++above) {
    if (mWarpAt[above] >= places.size()) {
      return {};
    }
    places = places[mWarpAt[above]].places;
  }
  return places;
}

std::string TitleDebugRows::warpText(std::size_t level) const {
  const std::span<const psx::game::WarpPlace> places = warpChoices(level);
  return mWarpAt[level] < places.size() ? places[mWarpAt[level]].name : std::string(kAbsentText);
}

void TitleDebugRows::warpStep(std::size_t level, int dir) {
  const std::span<const psx::game::WarpPlace> places = warpChoices(level);
  if (places.empty()) {
    return;
  }
  mWarpAt[level] = static_cast<std::size_t>(
      wrap<std::int64_t>(static_cast<std::int64_t>(mWarpAt[level]), dir, static_cast<std::int64_t>(places.size())));
  for (std::size_t below = level + 1; below < mWarpAt.size(); ++below) {
    mWarpAt[below] = 0;
  }
}

bool TitleDebugRows::warpAvailable() const {
  return !warpChoices(mWarpAt.size() - 1).empty() && menu()->warpRefusal(mGame->core).empty();
}

void TitleDebugRows::warpGo() {
  std::string command = "warp ";
  for (std::size_t level = 0; level < mWarpAt.size(); ++level) {
    command += (level > 0 ? "/" : "") + std::to_string(mWarpAt[level]);
  }
  send(command);
}

void TitleDebugRows::run(std::size_t verb) {
  send(line(verb));
}

void TitleDebugRows::send(const std::string &command) {
  std::string reply;
  if (!menu()->run(mGame->core, psx::debug::CommandArgs(command), reply)) {
    reply = "not a debug verb: " + command;
  }
  lucent::info("rmlui", "debug menu '{}': {}", command, reply);
  if (mStatus) {
    mStatus(reply);
  }
}

} // namespace psx::ui
