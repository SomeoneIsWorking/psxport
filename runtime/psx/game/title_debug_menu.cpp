#include "title_debug_menu.h"

#include "command_args.h"
#include "core.h"
#include "game.h"
#include "game_runtime.h"
#include "number_text.h"

#include <lucent/log.h>

#include <cctype>
#include <utility>

namespace psx::game {
namespace {

constexpr std::string_view kNoSoundTest = "this title has no sound test";

std::optional<std::int64_t> parseValue(Core &core, const DebugParam &param, std::string_view word) {
  switch (param.kind) {
  case DebugParam::Kind::Integer:
    if (const std::optional<long> value = psx::config::parseSigned(word, 10)) {
      return *value;
    }
    return std::nullopt;
  case DebugParam::Kind::Hex:
    if (const std::optional<std::uint64_t> value = psx::config::parseUnsigned64(word, 16)) {
      return static_cast<std::int64_t>(*value);
    }
    return std::nullopt;
  case DebugParam::Kind::Named:
    if (const std::optional<long> value = psx::config::parseSigned(word, 10)) {
      return *value;
    }
    [[fallthrough]];
  case DebugParam::Kind::Word: {
    const int count = param.names.count(core);
    for (int i = 0; i < count; ++i) {
      if (const std::string name = param.names.name(core, i); !name.empty() && name == word) {
        return i;
      }
    }
    return std::nullopt;
  }
  case DebugParam::Kind::Text:
    return 0;
  }
  return std::nullopt;
}

// The inclusive range a parsed value must fall in.
std::pair<std::int64_t, std::int64_t> valueRange(Core &core, const DebugParam &param) {
  if (param.kind == DebugParam::Kind::Named || param.kind == DebugParam::Kind::Word) {
    return {0, static_cast<std::int64_t>(param.names.count(core)) - 1};
  }
  return {param.min, param.max};
}

std::string rangeText(Core &core, const DebugParam &param, std::pair<std::int64_t, std::int64_t> range) {
  if (param.kind == DebugParam::Kind::Word) {
    std::string words;
    for (int i = 0; i <= range.second; ++i) {
      words += (i > 0 ? "|" : "") + param.names.name(core, i);
    }
    return words;
  }
  if (param.kind == DebugParam::Kind::Hex) {
    return lucent::format("0x{:X}..0x{:X}", range.first, range.second);
  }
  if (param.kind == DebugParam::Kind::Text) {
    return "text";
  }
  return lucent::format("{}..{}", range.first, range.second);
}

bool sameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view trimmed(std::string_view text) {
  constexpr std::string_view kBlank = " \t\"'";
  const std::size_t begin = text.find_first_not_of(kBlank);
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(kBlank) - begin + 1);
}

// A segment names a place by its number or, ignoring case, by its name.
const WarpPlace *placeNamed(std::span<const WarpPlace> places, std::string_view segment, std::size_t &index) {
  if (const std::optional<long> number = psx::config::parseSigned(segment, 10)) {
    if (*number >= 0 && static_cast<std::size_t>(*number) < places.size()) {
      index = static_cast<std::size_t>(*number);
      return &places[index];
    }
    return nullptr;
  }
  for (std::size_t i = 0; i < places.size(); ++i) {
    if (sameName(places[i].name, segment)) {
      index = i;
      return &places[i];
    }
  }
  return nullptr;
}

std::string childList(std::span<const WarpPlace> places) {
  std::string text;
  for (std::size_t i = 0; i < places.size(); ++i) {
    text += lucent::format("{}{} {}", i > 0 ? " | " : "", i, places[i].name);
  }
  return text;
}

void listTree(std::span<const WarpPlace> places, const std::string &indent, std::string &out) {
  for (std::size_t i = 0; i < places.size(); ++i) {
    out += lucent::format("\n{}{} {}", indent, i, places[i].name);
    listTree(places[i].places, indent + "  ", out);
  }
}

} // namespace

DebugNames DebugNames::of(std::span<const char *const> names) {
  return {
      [names](Core &) {
        return static_cast<int>(names.size());
      },
      [names](Core &, int index) {
        const char *name = index >= 0 && index < static_cast<int>(names.size()) ? names[index] : nullptr;
        return std::string(name != nullptr ? name : "");
      },
  };
}

DebugNames DebugNames::of(std::span<const std::string_view> names) {
  return {
      [names](Core &) {
        return static_cast<int>(names.size());
      },
      [names](Core &, int index) {
        return index >= 0 && index < static_cast<int>(names.size()) ? std::string(names[index]) : std::string();
      },
  };
}

DebugParam DebugParam::integer(std::string_view name, std::int64_t min, std::int64_t max) {
  return {.name = name, .kind = Kind::Integer, .min = min, .max = max};
}

DebugParam DebugParam::hex(std::string_view name, std::int64_t min, std::int64_t max) {
  return {.name = name, .kind = Kind::Hex, .min = min, .max = max};
}

DebugParam DebugParam::named(std::string_view name, DebugNames names) {
  return {.name = name, .kind = Kind::Named, .names = std::move(names)};
}

DebugParam DebugParam::word(std::string_view name, DebugNames names) {
  return {.name = name, .kind = Kind::Word, .names = std::move(names)};
}

DebugParam DebugParam::text(std::string_view name) {
  return {.name = name, .kind = Kind::Text};
}

DebugParam DebugParam::orAbsent() const {
  DebugParam copy = *this;
  copy.optional = true;
  return copy;
}

std::string TitleDebugMenu::usage(const DebugVerb &verb) {
  std::string text(verb.name);
  for (const DebugParam &param : verb.params) {
    text += param.optional ? " [" : " <";
    text += param.name;
    text += param.optional ? "]" : ">";
  }
  return text;
}

bool TitleDebugMenu::run(Core &core, const psx::debug::CommandArgs &args, std::string &reply) const {
  const std::string_view name = args.word(0);
  for (const DebugVerb &verb : verbs_) {
    if (verb.name != name) {
      continue;
    }
    const bool takesText = !verb.params.empty() && verb.params.back().kind == DebugParam::Kind::Text;
    if (!takesText && args.size() - 1 > verb.params.size()) {
      reply = "usage: " + usage(verb);
      return true;
    }
    DebugValues values;
    for (std::size_t i = 0; i < verb.params.size(); ++i) {
      const DebugParam &param = verb.params[i];
      const std::string_view word = param.kind == DebugParam::Kind::Text ? args.rest(i + 1) : args.word(i + 1);
      if (word.empty()) {
        if (!param.optional) {
          reply = "usage: " + usage(verb);
          return true;
        }
        break;
      }
      const std::optional<std::int64_t> value = parseValue(core, param, word);
      const std::pair<std::int64_t, std::int64_t> range = valueRange(core, param);
      if (!value) {
        reply = lucent::format("refused: {} '{}' is not one of {}", param.name, word, rangeText(core, param, range));
        return true;
      }
      if (*value < range.first || *value > range.second) {
        reply = lucent::format("refused: {} {} is out of range ({})", param.name, word, rangeText(core, param, range));
        return true;
      }
      values.values_[i] = value;
      values.words_[i] = word;
    }
    if (verb.refusal) {
      if (const std::string refusal = verb.refusal(core); !refusal.empty()) {
        reply = "refused: " + refusal;
        return true;
      }
    }
    verb.run({.core = core, .values = values, .reply = reply});
    while (!reply.empty() && reply.back() == '\n') {
      reply.pop_back();
    }
    return true;
  }
  return false;
}

TitleDebugMenu::Section &TitleDebugMenu::current() {
  if (sections_.empty()) {
    sections_.push_back({.heading = "Debug"});
  }
  return sections_.back();
}

void TitleDebugMenu::section(std::string_view heading) {
  sections_.push_back({.heading = heading});
}

void TitleDebugMenu::verb(DebugVerb verb) {
  if (verb.params.size() > kMaxDebugParams) {
    lucent::error(
        "debug-menu", "verb {} declares {} parameters; at most {}", verb.name, verb.params.size(), kMaxDebugParams);
    std::abort();
  }
  current().verbs.push_back(verbs_.size());
  verbs_.push_back(std::move(verb));
}

void TitleDebugMenu::readout(std::string_view label, std::function<std::string(Core &)> text) {
  current().readouts.push_back(readouts_.size());
  readouts_.push_back({.label = label, .text = std::move(text)});
}

void TitleDebugMenu::soundTest() {
  section("Sound Test");
  readout("Playing", [](Core &core) {
    const TitleAudio *audio = core.runtime.audio(core);
    const std::string_view name = audio != nullptr ? audio->nowPlayingName() : std::string_view{};
    return name.empty() ? std::string("stopped") : std::string(name);
  });
  const auto refusal = [](Core &core) {
    return core.runtime.audio(core) == nullptr ? std::string(kNoSoundTest) : std::string();
  };
  verb({
      .name = "music",
      .summary = "Play",
      .params = {DebugParam::named("track",
                                   {
                                       [](Core &core) {
                                         const TitleAudio *audio = core.runtime.audio(core);
                                         return audio != nullptr ? audio->trackCount() : 0;
                                       },
                                       [](Core &core, int track) {
                                         const TitleAudio *audio = core.runtime.audio(core);
                                         return audio != nullptr ? std::string(audio->trackName(track)) : std::string();
                                       },
                                   })},
      .run =
          [](const DebugCall &call) {
            TitleAudio &audio = *call.core.runtime.audio(call.core);
            const int track = static_cast<int>(call.values.value(0));
            audio.soundTestPlay(track);
            call.reply = lucent::format("ok: playing track {} ({})", track, audio.trackName(track));
          },
      .refusal = refusal,
  });
  verb({
      .name = "music-stop",
      .summary = "Stop",
      .run =
          [](const DebugCall &call) {
            call.core.runtime.audio(call.core)->soundTestPlay(-1);
            call.reply = "ok: stopped";
          },
      .refusal = refusal,
  });
}

void TitleDebugMenu::screen(std::function<std::uint64_t(Core &)> key,
                            std::function<std::string(Core &, std::uint64_t)> route) {
  screenKey_ = std::move(key);
  screenRoute_ = std::move(route);
}

void TitleDebugMenu::warp(WarpCatalogue catalogue) {
  warp_ = std::move(catalogue);
  section("Warp");
  verb({
      .name = "warp",
      .summary = "Go to a place: names or numbers joined by '/', '?' lists",
      .params = {DebugParam::text("path").orAbsent()},
      .run =
          [this](const DebugCall &call) {
            call.reply = warpTo(call.core, call.values.word(0));
          },
      .inMenu = false,
  });
}

std::span<const WarpPlace> TitleDebugMenu::warpPlaces(Core &core) const {
  if (warpPlaces_.empty() && warp_.places) {
    warpPlaces_ = warp_.places(core);
  }
  return warpPlaces_;
}

std::string TitleDebugMenu::warpTo(Core &core, std::string_view path) const {
  std::span<const WarpPlace> places = warpPlaces(core);
  if (places.empty()) {
    return "refused: the warp places are not loaded yet";
  }
  path = trimmed(path);
  std::string reached;
  std::size_t depth = 0;
  const WarpPlace *place = nullptr;
  while (!path.empty() && path != "?") {
    const std::size_t slash = path.find('/');
    const std::string_view segment = trimmed(path.substr(0, slash));
    path = slash == std::string_view::npos ? std::string_view{} : trimmed(path.substr(slash + 1));
    const std::string_view level = depth < warp_.levels.size() ? warp_.levels[depth] : std::string_view("place");
    if (segment == "?") {
      path = segment;
      break;
    }
    std::size_t index = 0;
    place = placeNamed(places, segment, index);
    if (place == nullptr) {
      return lucent::format("refused: no {} '{}'{}; one of: {}",
                            level,
                            segment,
                            reached.empty() ? std::string() : " in " + reached,
                            childList(places));
    }
    reached += (reached.empty() ? "" : "/") + place->name;
    places = place->places;
    ++depth;
  }
  if (path == "?" || place == nullptr) {
    std::string tree = reached.empty() ? std::string("places:") : reached + ":";
    listTree(places, "  ", tree);
    return tree;
  }
  if (!place->arrive) {
    const std::string_view level = depth < warp_.levels.size() ? warp_.levels[depth] : std::string_view("place");
    return lucent::format("refused: {} is not an arrival; pick its {}, one of: {}", reached, level, childList(places));
  }
  if (const std::string refusal = warpRefusal(core); !refusal.empty()) {
    return "refused: " + refusal;
  }
  lucent::info("warp", "arrival {} armed", reached);
  return place->arrive(core);
}

void TitleDebugMenu::aboutLine(std::string_view line) {
  about_.push_back(line);
}

std::optional<DecompImage>
TitleDebugMenu::decompImage(Core &core, std::string_view /*imageName*/, bool resident) const {
  const psx::host::TitleIdentity *title = core.game->title;
  if (!resident || title == nullptr || core.game->executable.empty()) {
    return std::nullopt;
  }
  return DecompImage{std::string(title->slug), core.game->executable};
}

} // namespace psx::game
