// title_debug_menu.h — a title's debug verbs, readouts, sound test and About lines: the one table behind
// the REPL, `help`, the control channel and the in-window Debug tab.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Core;

namespace psx::debug {
class CommandArgs;
}

namespace psx::game {

// The values a named or word parameter offers, read live because a list may come from guest data.
struct DebugNames {
  std::function<int(Core &)> count;
  // Empty for a value with no sourced name.
  std::function<std::string(Core &, int)> name;

  // A fixed table; an empty entry is an unnamed value.
  static DebugNames of(std::span<const char *const> names);
  static DebugNames of(std::span<const std::string_view> names);
};

// One typed parameter of a debug verb, parsed for the control channel and stepped by the menu.
struct DebugParam {
  enum class Kind : std::uint8_t {
    Integer, // decimal in [min, max]
    Hex,     // base 16 in [min, max]
    Named,   // an index into names, by number or by name
    Word,    // one of names, written as the word
  };

  std::string_view name;
  Kind kind = Kind::Integer;
  std::int64_t min = 0;
  std::int64_t max = 0;
  DebugNames names;
  bool optional = false;

  static DebugParam integer(std::string_view name, std::int64_t min, std::int64_t max);
  static DebugParam hex(std::string_view name, std::int64_t min, std::int64_t max);
  static DebugParam named(std::string_view name, DebugNames names);
  static DebugParam word(std::string_view name, DebugNames names);
  // The same parameter, which a line may leave out; every parameter after it is optional too.
  DebugParam orAbsent() const;
};

inline constexpr std::size_t kMaxDebugParams = 4;

// One verb's parsed parameters in declaration order: the number, or the index of a named or word value.
class DebugValues {
public:
  bool has(std::size_t index) const {
    return at(index).has_value();
  }
  std::int64_t value(std::size_t index, std::int64_t absent = 0) const {
    return at(index).value_or(absent);
  }
  std::uint32_t u32(std::size_t index, std::uint32_t absent = 0) const {
    return static_cast<std::uint32_t>(at(index).value_or(absent));
  }
  // The word as the line spelled it.
  std::string_view word(std::size_t index) const {
    return index < words_.size() ? words_[index] : std::string_view{};
  }

private:
  friend class TitleDebugMenu;
  std::optional<std::int64_t> at(std::size_t index) const {
    return index < values_.size() ? values_[index] : std::nullopt;
  }

  std::array<std::optional<std::int64_t>, kMaxDebugParams> values_{};
  std::array<std::string_view, kMaxDebugParams> words_{};
};

// One verb running: the machine, its parameters and the reply to the client, one line per '\n'.
struct DebugCall {
  Core &core;
  const DebugValues &values;
  std::string &reply;
};

struct DebugVerb {
  std::string_view name;
  // One line for `help`; the menu's row label.
  std::string_view summary;
  std::vector<DebugParam> params;
  std::function<void(const DebugCall &)> run;
  // Empty when the verb may run now; otherwise why it may not, in the title's words.
  std::function<std::string(Core &)> refusal;
  // False keeps a diagnostic verb off the menu; it stays on the control channel and in `help`.
  bool inMenu = true;
};

struct DebugReadout {
  std::string_view label;
  std::function<std::string(Core &)> text;
};

// The decomp_pipeline.py input that holds a code image's bytes: its tools/decomp/manifest.json entry and the file.
struct DecompImage {
  std::string manifestName;
  std::filesystem::path file;
};

class TitleDebugMenu {
public:
  // A heading on the Debug tab and the verbs and readouts registered under it.
  struct Section {
    std::string_view heading;
    std::vector<std::size_t> verbs;
    std::vector<std::size_t> readouts;
  };

  // A title with no debug routes holds one as is; the Debug tab says so.
  TitleDebugMenu() = default;
  virtual ~TitleDebugMenu() = default;
  TitleDebugMenu(const TitleDebugMenu &) = delete;
  TitleDebugMenu &operator=(const TitleDebugMenu &) = delete;
  TitleDebugMenu(TitleDebugMenu &&) = delete;
  TitleDebugMenu &operator=(TitleDebugMenu &&) = delete;

  // True when the line's first word is one of these verbs; the answer is in `reply`.
  bool run(Core &core, const psx::debug::CommandArgs &args, std::string &reply) const;

  std::span<const DebugVerb> verbs() const {
    return verbs_;
  }
  std::span<const DebugReadout> readouts() const {
    return readouts_;
  }
  std::span<const Section> sections() const {
    return sections_;
  }
  std::span<const std::string_view> about() const {
    return about_;
  }
  // `warp <area> [entry]`, from the verb's parameters.
  static std::string usage(const DebugVerb &verb);

  // The screen the machine shows: a key that changes when the screen does, read every frame.
  std::uint64_t screenKey(Core &core) const {
    return screenKey_ ? screenKey_(core) : 0u;
  }
  // The control-channel line that reaches the screen `key` names ("warp 11"); empty when no route does.
  std::string screenRoute(Core &core, std::uint64_t key) const {
    return screenRoute_ ? screenRoute_(core, key) : std::string{};
  }
  // The decomp input for the code image the catalog names `imageName`: the booted executable when `resident`; a title
  // with overlays names each overlay's manifest entry and file. Empty when the manifest has no entry for it.
  virtual std::optional<DecompImage> decompImage(Core &core, std::string_view imageName, bool resident) const;

protected:
  // Later verbs and readouts go under this heading.
  void section(std::string_view heading);
  void verb(DebugVerb verb);
  void readout(std::string_view label, std::function<std::string(Core &)> text);
  // `music <track>` and `music-stop` over the title's TitleAudio, with a now-playing readout.
  void soundTest();
  // A credit or prior-art line under the framework's own About lines.
  void aboutLine(std::string_view line);
  // How the owner journal names a screen: a cheap key, and the verb line a key's screen is reached by.
  void screen(std::function<std::uint64_t(Core &)> key, std::function<std::string(Core &, std::uint64_t)> route);

private:
  Section &current();

  std::vector<DebugVerb> verbs_;
  std::vector<DebugReadout> readouts_;
  std::vector<Section> sections_;
  std::vector<std::string_view> about_;
  std::function<std::uint64_t(Core &)> screenKey_;
  std::function<std::string(Core &, std::uint64_t)> screenRoute_;
};

} // namespace psx::game
