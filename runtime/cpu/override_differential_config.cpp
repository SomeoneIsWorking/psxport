#include "override_differential_config.h"

#include <lucent/log.h>

#include <charconv>
#include <limits>
#include <utility>

namespace psx::cpu {
namespace {

bool isSeparator(char c) {
  return c == ',' || c == ' ' || c == '\t';
}

std::optional<std::uint32_t> parseAddress(std::string_view token) {
  if (token.size() < 3 || token[0] != '0' || (token[1] != 'x' && token[1] != 'X')) {
    return std::nullopt;
  }
  const std::string_view digits = token.substr(2);
  std::uint32_t value = 0;
  const auto [end, status] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
  if (status != std::errc{} || end != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

bool isAddressToken(std::string_view token) {
  return token.size() >= 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
}

std::optional<std::uint32_t> countFrom(std::int64_t value) {
  if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

} // namespace

OverrideDifferentialConfig overrideDifferentialConfigFrom(std::string_view selectors,
                                                          std::int64_t firstCalls,
                                                          std::int64_t everyKth,
                                                          std::int64_t deadStackBytes,
                                                          std::string_view reportPath) {
  OverrideDifferentialConfig config;
  std::size_t at = 0;
  while (at < selectors.size()) {
    while (at < selectors.size() && isSeparator(selectors[at])) {
      ++at;
    }
    std::size_t end = at;
    while (end < selectors.size() && !isSeparator(selectors[end])) {
      ++end;
    }
    if (end == at) {
      break;
    }
    const std::string_view token = selectors.substr(at, end - at);
    at = end;
    DifferentialSelector selector{std::string(token), std::nullopt};
    if (isAddressToken(token)) {
      selector.address = parseAddress(token);
      if (!selector.address) {
        config.error = lucent::format("selector '{}' starts with 0x but is not a 32-bit hex address", token);
      }
    }
    config.selectors.push_back(std::move(selector));
  }
  const auto first = countFrom(firstCalls);
  const auto every = countFrom(everyKth);
  const auto deadStack = countFrom(deadStackBytes);
  if (!first || !every || !deadStack) {
    config.error = lucent::format("sampling counts must be non-negative 32-bit values; got first={} every={} "
                                  "dead-stack={}",
                                  firstCalls,
                                  everyKth,
                                  deadStackBytes);
  } else {
    config.firstCalls = *first;
    config.everyKth = *every;
    config.deadStackBytes = *deadStack;
  }
  if (config.firstCalls == 0 && config.everyKth == 0 && !config.error) {
    config.error = "first=0 and every=0 samples nothing, so every requested override would report zero samples";
  }
  if (reportPath.empty()) {
    config.error = "the report path is empty";
  }
  config.reportPath = std::string(reportPath);
  return config;
}

} // namespace psx::cpu
