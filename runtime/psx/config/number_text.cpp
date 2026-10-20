#include "number_text.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace psx::config {

std::optional<std::uint64_t> parseUnsigned64(std::string_view text, int base) {
  if (base == 16 && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::uint32_t> parseUnsigned(std::string_view text, int base) {
  const std::optional<std::uint64_t> value = parseUnsigned64(text, base);
  if (!value.has_value() || *value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*value);
}

std::optional<long> parseSigned(std::string_view text) {
  long value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> parseInt(std::string_view text) {
  const std::optional<long> value = parseSigned(text);
  if (!value.has_value() || *value < std::numeric_limits<int>::min() || *value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(*value);
}

std::optional<float> parseFloat(std::string_view text) {
  float value = 0.0f;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<NumberPair> parsePair(std::string_view text, PairFormat format) {
  const std::size_t split = text.find(format.separator);
  if (split == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> first = parseUnsigned(text.substr(0, split), format.base);
  const std::optional<std::uint32_t> second = parseUnsigned(text.substr(split + 1), format.base);
  if (!first.has_value() || !second.has_value()) {
    return std::nullopt;
  }
  return NumberPair{*first, *second};
}

} // namespace psx::config
