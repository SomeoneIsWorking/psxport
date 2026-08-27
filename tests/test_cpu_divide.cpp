// Exercise the shipping R3000 DIV/DIVU helpers used by both the interpreter and emitted code.
#include "core.h"
#include "cpu_divide.h"
#include "testutil.h"

#include <memory>

namespace {

void checkSigned(uint32_t numerator, uint32_t denominator, uint32_t quotient, uint32_t remainder) {
  auto core = std::make_unique<Core>();
  core->lo = 0xA5A5A5A5u;
  core->hi = 0x5A5A5A5Au;
  cpu_div(core.get(), numerator, denominator);
  CHECK_EQ(core->lo, quotient);
  CHECK_EQ(core->hi, remainder);
}

void checkUnsigned(uint32_t numerator, uint32_t denominator, uint32_t quotient, uint32_t remainder) {
  auto core = std::make_unique<Core>();
  core->lo = 0xA5A5A5A5u;
  core->hi = 0x5A5A5A5Au;
  cpu_divu(core.get(), numerator, denominator);
  CHECK_EQ(core->lo, quotient);
  CHECK_EQ(core->hi, remainder);
}

void test_signed_quotient_and_remainder_truncate_toward_zero() {
  checkSigned(7u, 3u, 2u, 1u);
  checkSigned(static_cast<uint32_t>(-7), 3u, static_cast<uint32_t>(-2), static_cast<uint32_t>(-1));
  checkSigned(7u, static_cast<uint32_t>(-3), static_cast<uint32_t>(-2), 1u);
  checkSigned(static_cast<uint32_t>(-7), static_cast<uint32_t>(-3), 2u, static_cast<uint32_t>(-1));
}

void test_signed_divide_by_zero_and_overflow_are_defined() {
  checkSigned(7u, 0u, 0xFFFFFFFFu, 7u);
  checkSigned(static_cast<uint32_t>(-7), 0u, 1u, static_cast<uint32_t>(-7));
  checkSigned(0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0u);
}

void test_unsigned_division_and_divide_by_zero() {
  checkUnsigned(7u, 3u, 2u, 1u);
  checkUnsigned(0xFFFFFFFFu, 2u, 0x7FFFFFFFu, 1u);
  checkUnsigned(0x12345678u, 0u, 0xFFFFFFFFu, 0x12345678u);
}

} // namespace

int main() {
  RUN(signed_quotient_and_remainder_truncate_toward_zero);
  RUN(signed_divide_by_zero_and_overflow_are_defined);
  RUN(unsigned_division_and_divide_by_zero);
  return pt_summary();
}
