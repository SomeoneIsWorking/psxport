// A direct GameRuntime that uses the neutral presenter must not pull the concrete interpolation
// subsystem into its executable. Static-library object granularity matters here: a source-level null
// pointer is insufficient if the legacy adapter's vtable still drags Fps60 into the link.
#include "game.h"
#include "game_runtime.h"
#include "testutil.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

class DirectRuntime final : public GameRuntime {
public:
  bool guestVramIsPicture(const Game &) const override {
    return false;
  }
  void *createContext(Core &) override {
    return nullptr;
  }
  void destroyContext(void *) override {}
  void registerOverrides(Game &) override {}
  void bootInit(Core &) override {}
};

class NeutralBackend final : public FramePresentationBackend {
public:
  void emit(std::span<const RqItem>) override {}
  void presentReal() override {
    ++presents;
  }
  void presentIntermediate() override {
    ++intermediatePresents;
  }
  void captureDiagnostic(uint64_t, bool) override {}
  void pace(int, int) override {}
  void reconcile(uint64_t) override {}
  void beginLedgerFrame() override {}

  int presents = 0;
  int intermediatePresents = 0;
};

void test_direct_runtime_constructs_without_a_temporal_product() {
  DirectRuntime runtime;
  psxport_install_game(runtime);
  const auto game = std::make_unique<Game>();
  CHECK(game->temporalPresentation == nullptr);
  NeutralBackend backend;
  game->presentation.commit(backend, 1);
  CHECK_EQ(backend.presents, 1);
  CHECK_EQ(backend.intermediatePresents, 0);
}

void test_direct_runtime_binary_does_not_link_fps60(const char *executable) {
  const std::string command = std::string("nm -C -- '") + executable + "'";
  FILE *symbols = popen(command.c_str(), "r");
  CHECK(symbols != nullptr);
  if (!symbols) {
    return;
  }

  char line[1024];
  int lines = 0;
  bool concreteInterpolationLinked = false;
  while (std::fgets(line, sizeof(line), symbols)) {
    ++lines;
    const std::string symbol(line);
    concreteInterpolationLinked |= symbol.find("Fps60::") != std::string::npos;
    concreteInterpolationLinked |= symbol.find("vtable for Fps60") != std::string::npos;
  }
  CHECK_EQ(pclose(symbols), 0);
  CHECK(lines > 0);
  CHECK(!concreteInterpolationLinked);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 1) {
    return 2;
  }
  RUN(direct_runtime_constructs_without_a_temporal_product);
  test_direct_runtime_binary_does_not_link_fps60(argv[0]);
  return pt_summary();
}
