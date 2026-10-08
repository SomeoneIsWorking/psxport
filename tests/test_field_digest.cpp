// The per-field determinism digest must tell two RAM images apart wherever they differ, and must
// agree for equal images. A digest that ignored the tail of RAM would call two runs identical while
// they diverged there; both ends and the middle are therefore perturbed one byte at a time.
#include "field_digest.h"
#include "testutil.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using psx::diag::FieldDigest;

std::vector<std::uint8_t> image() {
  std::vector<std::uint8_t> ram(0x200000);
  for (std::size_t i = 0; i < ram.size(); ++i) {
    ram[i] = static_cast<std::uint8_t>(i * 31u + 7u);
  }
  return ram;
}

void test_equal_images_hash_equal() {
  const std::vector<std::uint8_t> a = image();
  const std::vector<std::uint8_t> b = image();
  CHECK(FieldDigest::hashWords(a) == FieldDigest::hashWords(b));
}

void test_a_single_byte_anywhere_changes_the_hash() {
  const std::vector<std::uint8_t> base = image();
  const std::uint64_t expected = FieldDigest::hashWords(base);
  const std::array<std::size_t, 5> where{0, 1, base.size() / 2, base.size() - 8, base.size() - 1};
  for (const std::size_t offset : where) {
    std::vector<std::uint8_t> changed = base;
    changed[offset] ^= 0x01u;
    CHECK(FieldDigest::hashWords(changed) != expected);
  }
}

void test_word_order_matters() {
  std::vector<std::uint8_t> a(16, 0);
  std::vector<std::uint8_t> b(16, 0);
  a[0] = 1;
  b[8] = 1;
  CHECK(FieldDigest::hashWords(a) != FieldDigest::hashWords(b));
}

} // namespace

int main() {
  RUN(equal_images_hash_equal);
  RUN(a_single_byte_anywhere_changes_the_hash);
  RUN(word_order_matters);
  return pt_summary();
}
