// The guest's SPU state (voice end, ENDX) advances every display field whether or not anything consumes the
// PCM: guest code waits on it, so a headless run without a capture must not freeze it.
#include "game.h"
#include "testutil.h"

#include <memory>

namespace {

void test_field_advances_spu_without_a_consumer() {
  auto game = std::make_unique<Game>();
  game->spu_audio.init();
  game->spu_audio.frame();
  CHECK_EQ(game->spu_audio.fieldReports().size(), 1u);
  game->spu_audio.frame();
  CHECK_EQ(game->spu_audio.fieldReports().size(), 2u);
}

} // namespace

int main() {
  RUN(field_advances_spu_without_a_consumer);
  return pt_summary();
}
