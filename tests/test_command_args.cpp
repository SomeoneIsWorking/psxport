// Strict number parsing and debug command splitting: the one parser behind config values and both
// control transports.
#include "command_args.h"
#include "number_text.h"
#include "testutil.h"

#include <cstdint>
#include <optional>

namespace {

void test_unsigned_numbers_must_fill_the_whole_text() {
  CHECK_EQ(psx::test::require(psx::config::parseUnsigned("80010000", 16)), 0x80010000u);
  CHECK_EQ(psx::test::require(psx::config::parseUnsigned("0x1F800000", 16)), 0x1F800000u);
  CHECK_EQ(psx::test::require(psx::config::parseUnsigned("42", 10)), 42u);
  CHECK(!psx::config::parseUnsigned("42x", 10).has_value());
  CHECK(!psx::config::parseUnsigned("", 16).has_value());
  CHECK(!psx::config::parseUnsigned("0x", 16).has_value());
  CHECK(!psx::config::parseUnsigned("100000000", 16).has_value()); // past 32 bits
  CHECK_EQ(psx::test::require(psx::config::parseUnsigned64("7f0000000000", 16)), 0x7f0000000000ull);
}

void test_signed_numbers_and_pairs() {
  CHECK_EQ(psx::test::require(psx::config::parseInt("-3")), -3);
  CHECK(!psx::config::parseInt("1.5").has_value());
  CHECK(!psx::config::parseInt("99999999999").has_value());
  const psx::config::NumberPair range =
      psx::test::require(psx::config::parsePair("8001F000,80020000", psx::config::kHexRange));
  CHECK_EQ(range.first, 0x8001F000u);
  CHECK_EQ(range.second, 0x80020000u);
  const psx::config::NumberPair size = psx::test::require(psx::config::parsePair("1280x720", psx::config::kDimensions));
  CHECK_EQ(size.first, 1280u);
  CHECK_EQ(size.second, 720u);
  CHECK(!psx::config::parsePair("1280", psx::config::kDimensions).has_value());
  CHECK(!psx::config::parsePair("1280x", psx::config::kDimensions).has_value());
}

void test_a_command_line_splits_into_words_and_a_rest() {
  const psx::debug::CommandArgs args("  cvar  PSXPORT_DEBUG   cd,gpu  extra \r\n");
  CHECK_EQ(args.size(), 4u);
  CHECK(args.word(0) == "cvar");
  CHECK(args.word(1) == "PSXPORT_DEBUG");
  CHECK(args.rest(2) == "cd,gpu  extra");
  CHECK(args.word(9).empty());
  CHECK(args.rest(9).empty());

  const psx::debug::CommandArgs numbers("r 0x80010000 16 -2 zz");
  CHECK_EQ(psx::test::require(numbers.hex(1)), 0x80010000u);
  CHECK_EQ(psx::test::require(numbers.number(2)), 16u);
  CHECK_EQ(psx::test::require(numbers.integer(3)), -2);
  CHECK(!numbers.hex(4).has_value());
  CHECK(!numbers.number(5).has_value());
}

void test_pad_button_names_map_to_active_high_bits() {
  CHECK_EQ(psx::debug::padButtonBit("start"), 0x0008);
  CHECK_EQ(psx::debug::padButtonBit("cross"), 0x4000);
  CHECK_EQ(psx::debug::padButtonBit("x"), 0x4000);
  CHECK_EQ(psx::debug::padButtonBit("sq"), 0x8000);
  CHECK_EQ(psx::debug::padButtonBit("left"), 0x0080);
  CHECK_EQ(psx::debug::padButtonBit("0x0100"), 0x0100);
  CHECK_EQ(psx::debug::padButtonBit("nonsense"), 0);
}

} // namespace

int main() {
  RUN(unsigned_numbers_must_fill_the_whole_text);
  RUN(signed_numbers_and_pairs);
  RUN(a_command_line_splits_into_words_and_a_rest);
  RUN(pad_button_names_map_to_active_high_bits);
  return psx::test::summary();
}
