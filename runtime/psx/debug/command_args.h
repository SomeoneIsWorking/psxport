// command_args.h — one debug command line split into words, with strict numeric parsing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psx::debug {

// One command's usage line for `help`.
struct VerbUsage {
  std::string_view usage;
  std::string_view summary;
};

class CommandArgs {
public:
  explicit CommandArgs(std::string_view line);

  std::size_t size() const {
    return words_.size();
  }
  // Empty past the end.
  std::string_view word(std::size_t index) const;
  // The word as an owned string, for APIs that need a terminated path or name.
  std::string text(std::size_t index) const {
    return std::string(word(index));
  }
  // Everything from word `index` to the end of the line, inner spacing kept.
  std::string_view rest(std::size_t index) const;
  // The whole word in base 16, with or without a 0x prefix; nullopt when absent or not a number.
  std::optional<std::uint32_t> hex(std::size_t index) const;
  // The whole word in base 10.
  std::optional<std::uint32_t> number(std::size_t index) const;
  std::optional<int> integer(std::size_t index) const;

private:
  std::string_view line_;
  std::vector<std::string_view> words_;
};

// A pad button name (start, x, up, ...) as its active-high PSX pad bit; anything else is read as hex.
std::uint16_t padButtonBit(std::string_view name);

} // namespace psx::debug
