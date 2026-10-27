#include "title_debug_menu.h"

#include "command_args.h"
#include "core.h"
#include "game_runtime.h"
#include "number_text.h"

#include <lucent/log.h>

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
  return lucent::format("{}..{}", range.first, range.second);
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
    if (args.size() - 1 > verb.params.size()) {
      reply = "usage: " + usage(verb);
      return true;
    }
    DebugValues values;
    for (std::size_t i = 0; i < verb.params.size(); ++i) {
      const DebugParam &param = verb.params[i];
      const std::string_view word = args.word(i + 1);
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

void TitleDebugMenu::aboutLine(std::string_view line) {
  about_.push_back(line);
}

} // namespace psx::game
