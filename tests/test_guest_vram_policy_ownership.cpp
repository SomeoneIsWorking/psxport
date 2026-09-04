// The renderer asks the installed derived runtime whether guest VRAM is picture content for this
// frame. It must not regress to the static legacy GameConfig bit that caused mixed guest/native
// titles to preserve texture-atlas rows beneath native geometry.
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

void test_gpu_renderer_uses_runtime_policy_not_legacy_config() {
  const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::string renderer = read_source(root / "runtime/psx/gpu_vk.cpp");
  CHECK(!renderer.empty());
  CHECK(renderer.find("preserveVramBackdrop") == std::string::npos);
  // Structural guard only: transition behavior is exercised through the production policy by
  // test_guest_vram_composite_policy.cpp.
  CHECK(renderer.find("game_guest_vram_is_picture(*game)") != std::string::npos);
  CHECK(renderer.find("game_guest_vram_is_picture(*core->game)") != std::string::npos);
}

} // namespace

int main() {
  RUN(gpu_renderer_uses_runtime_policy_not_legacy_config);
  return pt_summary();
}
