#include "image_writer.h"
#include "testutil.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

void test_invalid_inputs_are_refused() {
  const std::array<unsigned char, 3> pixel = {1, 2, 3};
  CHECK(!image_write_rgb24(nullptr, pixel.data(), 1, 1));
  CHECK(!image_write_rgb24("scratch/image-writer/invalid.ppm", nullptr, 1, 1));
  CHECK(!image_write_rgb24("scratch/image-writer/invalid.ppm", pixel.data(), 0, 1));
  CHECK(!image_write_rgb24("scratch/image-writer/invalid.ppm", pixel.data(), 1, 0));
}

void test_ppm_writes_exact_rgb24_payload_and_creates_parents() {
  const std::filesystem::path path = "scratch/image-writer/nested/two-pixels.ppm";
  const std::array<unsigned char, 6> pixels = {1, 2, 3, 252, 253, 254};

  CHECK(image_write_rgb24(path.c_str(), pixels.data(), 2, 1));
  CHECK(std::filesystem::is_regular_file(path));

  std::ifstream input(path, std::ios::binary);
  const std::string bytes(std::istreambuf_iterator<char>(input), {});
  const std::string header = "P6\n2 1\n255\n";
  CHECK_EQ(bytes.size(), header.size() + pixels.size());
  CHECK(bytes.compare(0, header.size(), header) == 0);
  CHECK_MEM_EQ(bytes.data() + header.size(), pixels.data(), pixels.size());
}

} // namespace

int main() {
  RUN(invalid_inputs_are_refused);
  RUN(ppm_writes_exact_rgb24_payload_and_creates_parents);
  return pt_summary();
}
