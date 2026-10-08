// test_state_file.cpp — the versioned container and the scalar codec every device section is written
// through.
//
// This is a NEGATIVE-FIRST test. A state file's whole job is to be refused when it should be: a
// truncated section, a duplicate name, an unknown section and a version this build does not write
// must each produce a NAMED refusal, because the failure mode they protect against is a load that
// applies part of a machine and leaves the rest at power-on — which is indistinguishable, at every
// call site downstream, from a state of a machine that really was at power-on.
//
// Hermetic: pure bytes in, bytes out. No Core, no Game, no disc.
#include "state_blob.h"
#include "state_file.h"
#include "testutil.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// A check that carries the refusal text: "opened" / "did not open" is only useful next to WHY.

std::vector<std::uint8_t> bytesOf(const char *text) {
  return std::vector<std::uint8_t>(text, text + std::strlen(text));
}

// Open must fail, and say why. The reason is checked for a SUBSTRING because a refusal nobody can
// act on is barely better than a silent success.
void refuses(const std::vector<std::uint8_t> &image, const char *expect) {
  psx::state::StateFile::OpenError error;
  const auto file = psx::state::StateFile::open(image, error);
  CHECK_MSG(!file.has_value(), "a state file that should have been refused opened");
  CHECK_MSG(error.reason.find(expect) != std::string::npos, error.reason.c_str());
  CHECK_MSG(error.reason.find(expect) == std::string::npos || !error.reason.empty(), "a refusal that named no reason");
}

void test_blob_round_trips_every_scalar_width(void) {
  psx::state::BlobWriter writer;
  writer.u8(0xA5);
  writer.u16(0xBEEF);
  writer.u32(0xDEADBEEFu);
  writer.u64(0x0123456789ABCDEFull);
  writer.i8(-3);
  writer.i32(-70000);
  writer.i64(-1234567890123LL);
  writer.boolean(true);
  writer.boolean(false);
  const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
  writer.blob(payload);

  psx::state::BlobReader reader(writer.bytesOut());
  CHECK_EQ(reader.u8(), 0xA5);
  CHECK_EQ(reader.u16(), 0xBEEF);
  CHECK_EQ(reader.u32(), 0xDEADBEEFu);
  CHECK(reader.u64() == 0x0123456789ABCDEFull);
  CHECK_EQ(static_cast<int>(reader.i8()), -3);
  CHECK_EQ(static_cast<int>(reader.i32()), -70000);
  CHECK(reader.i64() == -1234567890123LL);
  CHECK_EQ(reader.boolean(), true);
  CHECK_EQ(reader.boolean(), false);
  std::vector<std::uint8_t> readBack;
  CHECK(reader.blob(readBack));
  CHECK_EQ(readBack.size(), payload.size());
  CHECK(readBack == payload);
  CHECK_MSG(reader.ok(), "a reader that consumed exactly its section reported a failure");
  CHECK_MSG(reader.offset() == reader.size(), "a reader that consumed its section did not reach its end");
}

void test_blob_refuses_a_truncated_section_and_says_how_short(void) {
  // The vacuous-zero shape: a reader handed half a section must NOT hand back zeros and call it a
  // value. It must latch the failure AND publish both numbers, because "the tail was zero" is
  // exactly the answer a caller is most likely to want to believe.
  std::vector<std::uint8_t> truncated = {0x11, 0x22, 0x33};
  psx::state::BlobReader reader(truncated);
  (void)reader.u32();
  CHECK_MSG(!reader.ok(), "a reader past the end of its section still reported success");
  CHECK_EQ(static_cast<int>(reader.size()), 3);
  CHECK_EQ(static_cast<int>(reader.offset()), 3);

  // A blob whose declared length runs past the end is refused rather than read out of bounds.
  std::vector<std::uint8_t> lyingLength = {0x40, 0x00, 0x00, 0x00, 0xAA};
  psx::state::BlobReader liar(lyingLength);
  std::vector<std::uint8_t> out;
  CHECK_MSG(!liar.blob(out), "a blob claiming 64 bytes in a 5-byte section was accepted");
}

void test_image_round_trips_sections_in_order(void) {
  psx::state::StateImage image;
  CHECK(image.add("cpu", bytesOf("first")));
  CHECK(image.add("ram", bytesOf("second")));
  CHECK(image.add("title", bytesOf("third")));

  psx::state::StateFile::OpenError error;
  const auto file = psx::state::StateFile::open(image.bytes(), error);
  CHECK_MSG(file.has_value(), error.reason.c_str());
  CHECK_EQ(static_cast<int>(file->version()), static_cast<int>(psx::state::kFormatVersion));
  CHECK_EQ(static_cast<int>(file->names().size()), 3);
  CHECK_STREQ(file->names()[0].c_str(), "cpu");
  CHECK_STREQ(file->names()[2].c_str(), "title");
  const auto second = file->section("ram");
  CHECK(second.has_value());
  CHECK_STREQ(std::string(second->begin(), second->end()).c_str(), "second");
  CHECK_MSG(!file->section("nope").has_value(), "an absent section answered as present");
}

void test_image_refuses_a_duplicate_section_name(void) {
  // Two sections of one name would make the restored machine depend on which one a reader happened
  // to find — a silent, order-dependent difference between two runs that read the same file.
  psx::state::StateImage image;
  CHECK(image.add("cpu", bytesOf("a")));
  CHECK_MSG(!image.add("cpu", bytesOf("b")), "a duplicate section name was accepted");
  CHECK_MSG(!image.add("", bytesOf("c")), "an empty section name was accepted");
  const std::string tooLong(psx::state::kSectionNameBytes + 4, 'x');
  CHECK_MSG(!image.add(tooLong, bytesOf("d")), "an over-long section name was accepted");
}

void test_open_refuses_malformed_images_by_name(void) {
  psx::state::StateImage good;
  good.add("cpu", bytesOf("payload"));
  const std::vector<std::uint8_t> valid = good.bytes();
  CHECK(valid.size() > 16);

  refuses(std::vector<std::uint8_t>(8, 0), "shorter than");
  {
    std::vector<std::uint8_t> wrongMagic = valid;
    wrongMagic[0] = 'X';
    refuses(wrongMagic, "magic");
  }
  {
    std::vector<std::uint8_t> wrongVersion = valid;
    wrongVersion[8] = 99; // bytes [8,12) hold the format version
    refuses(wrongVersion, "format version 99");
  }
  {
    std::vector<std::uint8_t> truncated = valid;
    truncated.resize(truncated.size() - 3);
    refuses(truncated, "claims");
  }
  {
    // A section count larger than the image holds: the reader must notice, not scan off the end.
    std::vector<std::uint8_t> tooMany = valid;
    tooMany[12] = 40; // bytes [12,16) hold the section count; the image holds one section
    refuses(tooMany, "section 1 of 40 starts past the end");
  }
  {
    // A section name this build does not know. That is exactly the case where restoring the rest
    // would produce a machine that never existed, so it is a refusal and not a skip.
    std::vector<std::uint8_t> renamed = valid;
    std::memcpy(renamed.data() + 16, "mystery", 7);
    refuses(renamed, "unknown section 'mystery'");
  }
}

void test_every_framework_section_name_is_listed_once(void) {
  // The loader tells a framework section from a title section by consulting this list. A name added
  // to the writer but not to the list would make every file carrying it unloadable, and a name in the
  // list that nothing writes would let a file pass that omits a device.
  const auto &names = psx::state::frameworkSectionNames();
  CHECK_EQ(static_cast<int>(names.size()), 15);
  for (const std::string &name : names) {
    int occurrences = 0;
    for (const std::string &other : names) {
      if (other == name) {
        ++occurrences;
      }
    }
    CHECK_MSG(occurrences == 1, name.c_str());
  }
  CHECK(psx::state::isFrameworkSectionName("cpu"));
  CHECK(psx::state::isFrameworkSectionName("title"));
  CHECK(!psx::state::isFrameworkSectionName("mystery"));
}

} // namespace

int main(void) {
  RUN(blob_round_trips_every_scalar_width);
  RUN(blob_refuses_a_truncated_section_and_says_how_short);
  RUN(image_round_trips_sections_in_order);
  RUN(image_refuses_a_duplicate_section_name);
  RUN(open_refuses_malformed_images_by_name);
  RUN(every_framework_section_name_is_listed_once);
  return pt_summary();
}