#include "number_text.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace psx::config {
namespace {

// The digits' base, with a 0x prefix removed from `text` where `base` allows one.
int resolveBase(std::string_view &text, int base) {
  const bool hexPrefix = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  if ((base == 16 || base == 0) && hexPrefix) {
    text.remove_prefix(2);
    return 16;
  }
  if (base == 0) {
    return text.size() > 1 && text[0] == '0' ? 8 : 10;
  }
  return base;
}

} // namespace

std::optional<std::uint64_t> parseUnsigned64(std::string_view text, int base) {
  base = resolveBase(text, base);
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

std::optional<long> parseSigned(std::string_view text, int base) {
  const bool negative = !text.empty() && text[0] == '-';
  if (negative) {
    text.remove_prefix(1);
  }
  const std::optional<std::uint64_t> magnitude = parseUnsigned64(text, base);
  if (!magnitude.has_value()) {
    return std::nullopt;
  }
  const auto largest = static_cast<std::uint64_t>(std::numeric_limits<long>::max());
  if (*magnitude > largest + (negative ? 1u : 0u)) {
    return std::nullopt;
  }
  if (negative) {
    return *magnitude > largest ? std::numeric_limits<long>::min() : -static_cast<long>(*magnitude);
  }
  return static_cast<long>(*magnitude);
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

std::vector<std::string_view> splitList(std::string_view text, std::string_view separators) {
  std::vector<std::string_view> entries;
  std::size_t at = text.find_first_not_of(separators);
  while (at != std::string_view::npos) {
    const std::size_t end = text.find_first_of(separators, at);
    entries.push_back(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
    at = end == std::string_view::npos ? end : text.find_first_not_of(separators, end);
  }
  return entries;
}

std::optional<std::vector<std::uint32_t>> parseUnsignedList(std::string_view text, int base) {
  std::vector<std::uint32_t> values;
  for (const std::string_view entry : splitList(text, kListSeparators)) {
    const std::optional<std::uint32_t> value = parseUnsigned(entry, base);
    if (!value.has_value()) {
      return std::nullopt;
    }
    values.push_back(*value);
  }
  return values;
}

} // namespace psx::config
