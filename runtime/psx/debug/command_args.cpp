#include "command_args.h"

#include "number_text.h"

#include <array>

namespace psx::debug {

namespace {

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

struct PadButtonName {
  std::string_view name;
  std::uint16_t bit;
};

inline constexpr std::array<PadButtonName, 14> kPadButtonNames{{
    {"select", 0x0001},
    {"start", 0x0008},
    {"up", 0x0010},
    {"right", 0x0020},
    {"down", 0x0040},
    {"left", 0x0080},
    {"t", 0x1000},
    {"triangle", 0x1000},
    {"o", 0x2000},
    {"circle", 0x2000},
    {"x", 0x4000},
    {"cross", 0x4000},
    {"sq", 0x8000},
    {"square", 0x8000},
}};

} // namespace

std::uint16_t padButtonBit(std::string_view name) {
  for (const PadButtonName &entry : kPadButtonNames) {
    if (entry.name == name) {
      return entry.bit;
    }
  }
  return static_cast<std::uint16_t>(config::parseUnsigned(name, 16).value_or(0));
}

CommandArgs::CommandArgs(std::string_view line) : line_(line) {
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && isSpace(line[i])) {
      ++i;
    }
    const std::size_t start = i;
    while (i < line.size() && !isSpace(line[i])) {
      ++i;
    }
    if (i > start) {
      words_.push_back(line.substr(start, i - start));
    }
  }
}

std::string_view CommandArgs::word(std::size_t index) const {
  return index < words_.size() ? words_[index] : std::string_view{};
}

std::string_view CommandArgs::rest(std::size_t index) const {
  if (index >= words_.size()) {
    return {};
  }
  std::string_view tail = line_.substr(static_cast<std::size_t>(words_[index].data() - line_.data()));
  while (!tail.empty() && isSpace(tail.back())) {
    tail.remove_suffix(1);
  }
  return tail;
}

std::optional<std::uint32_t> CommandArgs::hex(std::size_t index) const {
  return config::parseUnsigned(word(index), 16);
}

std::optional<std::uint32_t> CommandArgs::number(std::size_t index) const {
  return config::parseUnsigned(word(index), 10);
}

std::optional<int> CommandArgs::integer(std::size_t index) const {
  return config::parseInt(word(index));
}

} // namespace psx::debug
