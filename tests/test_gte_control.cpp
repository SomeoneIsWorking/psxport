// test_gte_control.cpp — the control registers a render runs under: read, blended, written and handed back.
#include "beetle_gte.h"
#include "game.h"
#include "gte_control.h"
#include "proj_params.h"
#include "state_bytes.h"
#include "testutil.h"

#include "stub_game_runtime.h"
#include <memory>

namespace {

using psx::present::GteControl;

uint32_t pack(int16_t low, int16_t high) {
  return static_cast<uint16_t>(low) | (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16);
}

struct ControlSample {
  int16_t rotation = 0;
  int32_t translation = 0;
  uint32_t other = 0;
};

GteControl sample(ControlSample in) {
  const int16_t rotation = in.rotation;
  GteControl control{};
  for (uint32_t reg = 0; reg < psx::present::kGteRotationWords; ++reg) {
    control[reg] = pack(rotation, static_cast<int16_t>(-rotation));
  }
  for (uint32_t reg = psx::present::kGteRotationWords; reg < psx::present::kGteTranslationEnd; ++reg) {
    control[reg] = static_cast<uint32_t>(in.translation);
  }
  control[4] = static_cast<uint32_t>(static_cast<int32_t>(rotation)); // RT33 is one sign-extended s16
  control[24] = in.other;
  return control;
}

void test_rotation_halves_and_translation_move_and_the_rest_is_the_later_state(void) {
  const GteControl from = sample({0, -100, 1u});
  const GteControl to = sample({1000, 300, 2u});
  const GteControl half = psx::present::blendGteControl(from, to, 0.5f);
  CHECK_EQ(half[0], pack(500, -500));
  CHECK_EQ(half[4], 500u);
  CHECK_EQ(static_cast<int32_t>(half[5]), 100);
  CHECK_EQ(static_cast<int32_t>(half[7]), 100);
  CHECK_EQ(half[24], 2u);
  CHECK(psx::present::blendGteControl(from, to, 1.0f) == to);
  CHECK(psx::present::blendGteControl(from, to, 0.0f)[0] == from[0]);
}

void test_written_control_reads_back_and_the_guard_restores_the_gte(void) {
  psx::test::StubGameRuntime stubRuntime;
  auto game = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  psx::cpu::Gte &gte = game->core.gte();
  const GteControl before = sample({7, 9, 11u});
  psx::present::writeGteControl(gte, before);
  CHECK(psx::present::readGteControl(gte) == before);
  {
    const psx::present::GteGuard guard(gte);
    psx::present::writeGteControl(gte, sample({1, 2, 3u}));
    CHECK(psx::present::readGteControl(gte) != before);
  }
  CHECK(psx::present::readGteControl(gte) == before);
}

void test_state_bytes_read_back_what_was_written(void) {
  struct Header {
    uint32_t a;
    int16_t b;
  };
  psx::present::StateWriter writer;
  writer.put(Header{5u, -3});
  const uint8_t values[] = {1, 2, 3};
  writer.putAll(std::span<const uint8_t>(values));
  psx::present::StateReader reader(writer.bytes());
  const Header header = reader.get<Header>();
  CHECK_EQ(header.a, 5u);
  CHECK_EQ(header.b, -3);
  const std::vector<uint8_t> back = reader.getAll<uint8_t>(3);
  CHECK_EQ(back.size(), 3u);
  CHECK_EQ(back[2], 3u);
}

// Any GTE access re-points the register file at its own Core.
void test_a_gte_access_binds_its_cores_registers() {
  psx::test::StubGameRuntime stubRuntime;
  auto first = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  auto second = std::make_unique<Game>(stubRuntime, psx::config::Config{});
  first->core.gte().bind();
  (void)second->core.gte().readData(0);
  CHECK(GTE_CurState() == &second->core.gte().registers());
}

} // namespace

int main(void) {
  RUN(rotation_halves_and_translation_move_and_the_rest_is_the_later_state);
  RUN(written_control_reads_back_and_the_guard_restores_the_gte);
  RUN(a_gte_access_binds_its_cores_registers);
  RUN(state_bytes_read_back_what_was_written);
  return psx::test::summary();
}
