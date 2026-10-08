// The one hardware fact seven producers had each spelled out: which entry of a GTE SHORTMATRIX's
// five packed control words lands where in the 3x3 rotation.
//
// The words carry nine int16 entries in a fixed order, so a test whose entries are all DISTINCT is
// the only one that can catch a transposed column. A matrix of ones, or an identity, passes every
// wrong ordering there is.
#include "native_projection.h"

#include "testutil.h"

namespace {

using psxport::native_projection::rotationFromControlWords;

// CR0..CR4, low half first. The nine entries read 0x1111 through 0x9999 in ROW-MAJOR order, so a
// wrong answer names itself.
constexpr std::array<uint32_t, 5> kWords{0x22221111u, 0x44443333u, 0x66665555u, 0x88887777u, 0xAAAA9999u};

void test_the_nine_entries_land_in_row_major_order(void) {
  const auto m = rotationFromControlWords(kWords);
  CHECK_EQ(m[0][0], (int16_t)0x1111);
  CHECK_EQ(m[0][1], (int16_t)0x2222);
  CHECK_EQ(m[0][2], (int16_t)0x3333);
  CHECK_EQ(m[1][0], (int16_t)0x4444);
  CHECK_EQ(m[1][1], (int16_t)0x5555);
  CHECK_EQ(m[1][2], (int16_t)0x6666);
  CHECK_EQ(m[2][0], (int16_t)0x7777);
  CHECK_EQ(m[2][1], (int16_t)0x8888);
  CHECK_EQ(m[2][2], (int16_t)0x9999);
}

// The high half of CR4 is CR30 in a Moby draw record, not a tenth rotation entry. Changing it must
// not move the matrix.
void test_the_high_half_of_the_fifth_word_is_not_part_of_the_rotation(void) {
  auto other = kWords;
  other[4] = 0x00009999u;
  const auto a = rotationFromControlWords(kWords);
  const auto b = rotationFromControlWords(other);
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      CHECK_EQ(a[row][column], b[row][column]);
    }
  }
}

// Rotation entries are signed 1.3.12 fixed point, so the top bit is a sign and not magnitude.
void test_entries_are_signed(void) {
  const auto m = rotationFromControlWords({0x0000FFFFu, 0u, 0u, 0u, 0u});
  CHECK_EQ(m[0][0], (int16_t)-1);
  CHECK_EQ(m[0][1], (int16_t)0);
}

} // namespace

int main(void) {
  RUN(the_nine_entries_land_in_row_major_order);
  RUN(the_high_half_of_the_fifth_word_is_not_part_of_the_rotation);
  RUN(entries_are_signed);
  return pt_summary();
}
