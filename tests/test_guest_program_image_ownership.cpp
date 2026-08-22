// The executable-image fact slice has one production owner: GuestProgramImage on GameRuntime.
// These generic consumers must never drift back to reading the deprecated GameConfig bag.
#include "testutil.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_source(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::ostringstream contents;
  if (input) {
    contents << input.rdbuf();
  }
  return contents.str();
}

void test_program_image_consumers_do_not_read_legacy_bag() {
  const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const char *consumers[] = {
      "runtime/recomp/crt0_boot.h",
      "runtime/recomp/crt0_verify.h",
      "runtime/recomp/native_boot.cpp",
      "runtime/recomp/overlay_router.cpp",
      "runtime/recomp/sync_overrides.cpp",
  };

  for (const char *relative : consumers) {
    const std::string source = read_source(root / relative);
    fprintf(stderr, "  [ownership] %s: %zu byte(s) checked\n", relative, source.size());
    CHECK(!source.empty());
    CHECK(source.find("c->cfg->recMain") == std::string::npos);
    CHECK(source.find("cfg->recMain") == std::string::npos);
    CHECK(source.find("cfg->stackTopBase") == std::string::npos);
    CHECK(source.find("cfg->bssZero") == std::string::npos);
    CHECK(source.find("cfg->heapSizePtr") == std::string::npos);
    CHECK(source.find("cfg->heapBasePtr") == std::string::npos);
    CHECK(source.find("cfg->hle.codeScan") == std::string::npos);
  }
}

void test_legacy_hooks_cannot_own_program_image() {
  const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::string hooks = read_source(root / "runtime/recomp/legacy_game_hooks.h");
  CHECK(!hooks.empty());
  CHECK(hooks.find("GuestProgramImage") == std::string::npos);
  CHECK(hooks.find("guestProgramImage") == std::string::npos);
}

} // namespace

int main() {
  RUN(program_image_consumers_do_not_read_legacy_bag);
  RUN(legacy_hooks_cannot_own_program_image);
  return pt_summary();
}
