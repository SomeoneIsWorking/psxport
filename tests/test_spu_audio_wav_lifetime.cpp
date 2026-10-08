// PSXPORT_WAV is an explicit capture resource: it must be finalized while its SpuAudio owner is
// still alive.  An atexit callback retaining a Game-owned object is too late, because Game is
// normally destroyed before process teardown.
#include "game.h"
#include "testutil.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

std::filesystem::path capturePath(const char *name) {
  return std::filesystem::current_path() / name;
}

uint32_t readLe32(const std::filesystem::path &path, std::streamoff offset) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(offset);
  unsigned char bytes[4] = {};
  input.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8u) |
         (static_cast<uint32_t>(bytes[2]) << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
}

void test_active_wav_is_finalized_before_spu_owner_teardown() {
  const auto initial = capturePath("spu-audio-wav-initial.wav");
  const auto active = capturePath("spu-audio-wav-active.wav");
  std::filesystem::remove(initial);
  std::filesystem::remove(active);

  CHECK_EQ(setenv("PSXPORT_WAV", initial.c_str(), 1), 0);
  {
    auto game = std::make_unique<Game>();
    game->spu_audio.init();
    game->spu_audio.wavReopen(active.c_str());
  }

  CHECK(std::filesystem::is_regular_file(initial));
  CHECK(std::filesystem::is_regular_file(active));
  CHECK_EQ(std::filesystem::file_size(initial), 44u);
  CHECK_EQ(std::filesystem::file_size(active), 44u);
  CHECK_EQ(readLe32(initial, 4), 36u);
  CHECK_EQ(readLe32(initial, 40), 0u);
  CHECK_EQ(readLe32(active, 4), 36u);
  CHECK_EQ(readLe32(active, 40), 0u);

  unsetenv("PSXPORT_WAV");
  std::filesystem::remove(initial);
  std::filesystem::remove(active);
}

} // namespace

int main() {
  RUN(active_wav_is_finalized_before_spu_owner_teardown);
  return pt_summary();
}
