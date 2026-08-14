#include "gte_state.h"
#include "native_projection.h"
#include "native_projection_internal.h"
#include "testutil.h"

#include <array>
#include <cmath>
#include <cstdint>

extern "C" void GTE_Init(void);

namespace {
using namespace psxport::native_projection;

struct CompareResult {
  unsigned compared = 0;
  unsigned mismatched = 0;
  unsigned first = 9;
};

struct FractionalExpected {
  std::array<int64_t, 3> raw_fixed{};
  std::array<uint16_t, 3> fraction{};
  std::array<float, 3> raw{};
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
};

static unsigned compare_fractional(const NativeProjectedVertex &actual,
                                   const FractionalExpected &expected) {
  unsigned mismatched = 0;
  for (unsigned i = 0; i < 3; ++i) {
    mismatched += actual.raw_view_fixed[i] != expected.raw_fixed[i];
    mismatched +=
        ((uint64_t)actual.raw_view_fixed[i] & 0xfffu) != expected.fraction[i];
    mismatched += actual.raw_view[i] != expected.raw[i];
  }
  mismatched += std::fabs(actual.px - expected.px) > 1.0e-6f;
  mismatched += std::fabs(actual.py - expected.py) > 1.0e-6f;
  mismatched += std::fabs(actual.pz - expected.pz) > 1.0e-6f;
  return mismatched;
}

static CompareResult compare(const NativeProjectedVertex &native,
                             const GteRegs &guest, unsigned shift = 12) {
  const std::array<int32_t, 9> expected = {
      (int16_t)guest.REG[9],          (int16_t)guest.REG[10],
      (int16_t)guest.REG[11],         (int32_t)guest.REG[25],
      (int32_t)guest.REG[26],         (int32_t)guest.REG[27],
      (uint16_t)guest.REG[19],        (int16_t)guest.REG[14],
      (int16_t)(guest.REG[14] >> 16),
  };
  const std::array<int32_t, 9> actual = {
      native.ir[0],
      native.ir[1],
      native.ir[2],
      (int32_t)(native.raw_view_fixed[0] >> shift),
      (int32_t)(native.raw_view_fixed[1] >> shift),
      (int32_t)(native.raw_view_fixed[2] >> shift),
      native.sz,
      native.sx,
      native.sy,
  };
  CompareResult result{};
  for (unsigned i = 0; i < actual.size(); ++i) {
    ++result.compared;
    if (actual[i] != expected[i]) {
      if (!result.mismatched)
        result.first = i;
      ++result.mismatched;
    }
  }
  return result;
}

static uint32_t random_word(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

static GteRegs make_guest(const FixedAffine &affine,
                          const ProjectionParams &projection,
                          ModelVertex vertex) {
  GteRegs guest{};
  guest.REG[32] =
      (uint16_t)affine.m[0][0] | ((uint32_t)(uint16_t)affine.m[0][1] << 16);
  guest.REG[33] =
      (uint16_t)affine.m[0][2] | ((uint32_t)(uint16_t)affine.m[1][0] << 16);
  guest.REG[34] =
      (uint16_t)affine.m[1][1] | ((uint32_t)(uint16_t)affine.m[1][2] << 16);
  guest.REG[35] =
      (uint16_t)affine.m[2][0] | ((uint32_t)(uint16_t)affine.m[2][1] << 16);
  guest.REG[36] = (uint16_t)affine.m[2][2];
  for (unsigned i = 0; i < 3; ++i)
    guest.REG[37 + i] = (uint32_t)affine.t[i];
  guest.REG[56] = (uint32_t)projection.ofx;
  guest.REG[57] = (uint32_t)projection.ofy;
  guest.REG[58] = projection.h;
  guest.REG[0] = (uint16_t)vertex.x | ((uint32_t)(uint16_t)vertex.y << 16);
  guest.REG[1] = (uint16_t)vertex.z;
  return guest;
}

static void check_diagnostic_mode(const FixedAffine &affine,
                                  const ProjectionParams &projection,
                                  ModelVertex vertex, unsigned shift,
                                  bool limit_mode) {
  GteRegs guest = make_guest(affine, projection, vertex);
  const uint32_t insn = 0x4a000001u | (shift == 12 ? (1u << 19) : 0u) |
                        (limit_mode ? (1u << 10) : 0u);
  CHECK(GTE_ExecuteIsolated(&guest, insn) >= 0);
  const NativeProjectedVertex native =
      detail::project_gte_mode(affine, projection, vertex, shift, limit_mode);
  const CompareResult result = compare(native, guest, shift);
  CHECK_EQ(result.compared, 9u);
  CHECK_EQ(result.mismatched, 0u);
}

static void check_case(const FixedAffine &affine,
                       const ProjectionParams &projection, ModelVertex vertex) {
  GteRegs guest = make_guest(affine, projection, vertex);
  CHECK(GTE_ExecuteIsolated(&guest, 0x4a180001u) >= 0);
  const NativeProjectedVertex native = project(affine, projection, vertex);
  const CompareResult result = compare(native, guest);
  CHECK_EQ(result.compared, 9u);
  CHECK_EQ(result.mismatched, 0u);
}

static void test_random_and_edges(void) {
  uint32_t seed = 0x6d2b79f5u;
  uint32_t cases = 0;
  for (unsigned n = 0; n < 1024; ++n) {
    FixedAffine affine{};
    for (auto &row : affine.m)
      for (int16_t &value : row)
        value = (int16_t)random_word(seed);
    for (int32_t &value : affine.t)
      value = (int32_t)random_word(seed);
    const ProjectionParams projection{(int32_t)random_word(seed),
                                      (int32_t)random_word(seed),
                                      (uint16_t)random_word(seed)};
    const ModelVertex vertex{(int16_t)random_word(seed),
                             (int16_t)random_word(seed),
                             (int16_t)random_word(seed)};
    check_case(affine, projection, vertex);
    ++cases;
  }
  FixedAffine identity{};
  identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = 4096;
  check_case(identity, {160 << 16, 120 << 16, 256}, {0, 0, 0});
  identity.t[0] = INT32_MAX;
  identity.t[1] = INT32_MIN;
  identity.t[2] = -1;
  check_case(identity, {INT32_MAX, INT32_MIN, 0xffff}, {-32768, 32767, -32768});
  CHECK_EQ(cases, 1024u);
}

static void test_forced_mismatch_other_answer(void) {
  FixedAffine affine{};
  affine.m[0][0] = affine.m[1][1] = affine.m[2][2] = 4096;
  const ProjectionParams projection{160 << 16, 120 << 16, 256};
  const ModelVertex vertex{4, -7, 512};
  GteRegs guest = make_guest(affine, projection, vertex);
  CHECK(GTE_ExecuteIsolated(&guest, 0x4a180001u) >= 0);
  NativeProjectedVertex native = project(affine, projection, vertex);
  CHECK_EQ(compare(native, guest).mismatched, 0u);
  ++native.sx;
  const CompareResult corrupt = compare(native, guest);
  CHECK_EQ(corrupt.compared, 9u);
  CHECK_EQ(corrupt.mismatched, 1u);
  CHECK_EQ(corrupt.first, 7u);
}

static void test_diagnostic_modes(void) {
  uint32_t seed = 0xa511e9b3u;
  unsigned cases = 0;
  for (unsigned n = 0; n < 256; ++n) {
    FixedAffine affine{};
    for (auto &row : affine.m)
      for (int16_t &value : row)
        value = (int16_t)random_word(seed);
    for (int32_t &value : affine.t)
      value = (int32_t)random_word(seed);
    const ProjectionParams projection{(int32_t)random_word(seed),
                                      (int32_t)random_word(seed),
                                      (uint16_t)random_word(seed)};
    const ModelVertex vertex{(int16_t)random_word(seed),
                             (int16_t)random_word(seed),
                             (int16_t)random_word(seed)};
    check_diagnostic_mode(affine, projection, vertex, 0, false);
    check_diagnostic_mode(affine, projection, vertex, 0, true);
    check_diagnostic_mode(affine, projection, vertex, 12, true);
    cases += 3;
  }
  CHECK_EQ(cases, 768u);
}

static void test_fractional_endpoint_channels(void) {
  const ProjectionParams projection{160 << 16, 120 << 16, 256};

  FixedAffine near_affine{};
  near_affine.m = {{{1, 0, 0}, {0, -1, 0}, {0, 0, 1}}};
  near_affine.t = {{1, -1, 100}};
  NativeProjectedVertex near = project(near_affine, projection, {1, 1, 1});
  // Hand-derived fixed accumulators: T*4096 + M*V. Z is below H/2,
  // so only the float projection depth clamps to 128.
  const FractionalExpected near_expected{
      {4097, -4097, 409601},
      {1, 4095, 1},
      {1.0f + 1.0f / 4096.0f, -1.0f - 1.0f / 4096.0f, 100.0f + 1.0f / 4096.0f},
      162.0f,
      116.0f,
      128.0f,
  };
  CHECK_EQ(compare_fractional(near, near_expected), 0u);

  FixedAffine far_affine{};
  far_affine.m = {{{2048, 0, 0}, {0, 1024, 0}, {0, 0, 3}}};
  far_affine.t = {{4, -3, 512}};
  NativeProjectedVertex far = project(far_affine, projection, {1, 1, 1});
  constexpr float far_z = 512.0f + 3.0f / 4096.0f;
  const FractionalExpected far_expected{
      {18432, -11264, 2097155},       {2048, 1024, 3},
      {4.5f, -2.75f, far_z},          160.0f + 4.0f * 256.0f / far_z,
      120.0f - 3.0f * 256.0f / far_z, far_z,
  };
  CHECK_EQ(compare_fractional(far, far_expected), 0u);

  ++far.raw_view_fixed[2];
  far.px += 0.25f;
  CHECK_EQ(compare_fractional(far, far_expected), 3u);
}

} // namespace

int main() {
  GTE_Init();
  RUN(random_and_edges);
  RUN(forced_mismatch_other_answer);
  RUN(diagnostic_modes);
  RUN(fractional_endpoint_channels);
  return pt_summary();
}
