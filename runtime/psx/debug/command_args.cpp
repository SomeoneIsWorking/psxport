#include "command_args.h"

#include "host_input.h"
#include "number_text.h"

namespace psx::debug {

namespace {

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

std::uint16_t padButtonBit(std::string_view name) {
  if (const std::uint16_t bit = input::buttonNamed(name)) {
    return bit;
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

std::optional<float> CommandArgs::decimal(std::size_t index) const {
  return config::parseFloat(word(index));
}

} // namespace psx::debug
