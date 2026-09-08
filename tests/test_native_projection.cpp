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
  unsigned first = 10;
};

struct FractionalExpected {
  std::array<int64_t, 3> raw_fixed{};
  std::array<uint16_t, 3> fraction{};
  std::array<float, 3> raw{};
  float px = 0.0f;
  float py = 0.0f;
  float pz = 0.0f;
};

static unsigned compare_fractional(const NativeProjectedVertex &actual, const FractionalExpected &expected) {
  unsigned mismatched = 0;
  for (unsigned i = 0; i < 3; ++i) {
    mismatched += actual.raw_view_fixed[i] != expected.raw_fixed[i];
    mismatched += ((uint64_t)actual.raw_view_fixed[i] & 0xfffu) != expected.fraction[i];
    mismatched += actual.raw_view[i] != expected.raw[i];
  }
  mismatched += std::fabs(actual.px - expected.px) > 1.0e-6f;
  mismatched += std::fabs(actual.py - expected.py) > 1.0e-6f;
  mismatched += std::fabs(actual.pz - expected.pz) > 1.0e-6f;
  return mismatched;
}

static CompareResult compare(const NativeProjectedVertex &native, const GteRegs &guest, unsigned shift = 12) {
  const std::array<int64_t, 10> expected = {
      (int16_t)guest.REG[9],
      (int16_t)guest.REG[10],
      (int16_t)guest.REG[11],
      (int32_t)guest.REG[25],
      (int32_t)guest.REG[26],
      (int32_t)guest.REG[27],
      (uint16_t)guest.REG[19],
      (int16_t)guest.REG[14],
      (int16_t)(guest.REG[14] >> 16),
      guest.FLAGS,
  };
  const std::array<int64_t, 10> actual = {
      native.ir[0],
      native.ir[1],
      native.ir[2],
      (int32_t)(native.raw_view_fixed[0] >> shift),
      (int32_t)(native.raw_view_fixed[1] >> shift),
      (int32_t)(native.raw_view_fixed[2] >> shift),
      native.sz,
      native.sx,
      native.sy,
      native.flags,
  };
  CompareResult result{};
  for (unsigned i = 0; i < actual.size(); ++i) {
    ++result.compared;
    if (actual[i] != expected[i]) {
      if (!result.mismatched) {
        result.first = i;
      }
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

static GteRegs make_guest(const FixedAffine &affine, const ProjectionParams &projection, ModelVertex vertex) {
  GteRegs guest{};
  guest.REG[32] = (uint16_t)affine.m[0][0] | ((uint32_t)(uint16_t)affine.m[0][1] << 16);
  guest.REG[33] = (uint16_t)affine.m[0][2] | ((uint32_t)(uint16_t)affine.m[1][0] << 16);
  guest.REG[34] = (uint16_t)affine.m[1][1] | ((uint32_t)(uint16_t)affine.m[1][2] << 16);
  guest.REG[35] = (uint16_t)affine.m[2][0] | ((uint32_t)(uint16_t)affine.m[2][1] << 16);
  guest.REG[36] = (uint16_t)affine.m[2][2];
  for (unsigned i = 0; i < 3; ++i) {
    guest.REG[37 + i] = (uint32_t)affine.t[i];
  }
  guest.REG[56] = (uint32_t)projection.ofx;
  guest.REG[57] = (uint32_t)projection.ofy;
  guest.REG[58] = projection.h;
  guest.REG[59] = (uint16_t)projection.dqa;
  guest.REG[60] = (uint32_t)projection.dqb;
  guest.REG[0] = (uint16_t)vertex.x | ((uint32_t)(uint16_t)vertex.y << 16);
  guest.REG[1] = (uint16_t)vertex.z;
  return guest;
}

static void check_diagnostic_mode(const FixedAffine &affine,
                                  const ProjectionParams &projection,
                                  ModelVertex vertex,
                                  unsigned shift,
                                  bool limit_mode) {
  GteRegs guest = make_guest(affine, projection, vertex);
  const uint32_t insn = 0x4a000001u | (shift == 12 ? (1u << 19) : 0u) | (limit_mode ? (1u << 10) : 0u);
  CHECK(GTE_ExecuteIsolated(&guest, insn) >= 0);
  const NativeProjectedVertex native = detail::project_gte_mode(affine, projection, vertex, shift, limit_mode);
  const CompareResult result = compare(native, guest, shift);
  CHECK_EQ(result.compared, 10u);
  CHECK_EQ(result.mismatched, 0u);
}

static void check_case(const FixedAffine &affine, const ProjectionParams &projection, ModelVertex vertex) {
  GteRegs guest = make_guest(affine, projection, vertex);
  CHECK(GTE_ExecuteIsolated(&guest, 0x4a180001u) >= 0);
  const NativeProjectedVertex native = project(affine, projection, vertex);
  const CompareResult result = compare(native, guest);
  CHECK_EQ(result.compared, 10u);
  CHECK_EQ(result.mismatched, 0u);
}

static void test_random_and_edges(void) {
  uint32_t seed = 0x6d2b79f5u;
  uint32_t cases = 0;
  for (unsigned n = 0; n < 1024; ++n) {
    FixedAffine affine{};
    for (auto &row : affine.m) {
      for (int16_t &value : row) {
        value = (int16_t)random_word(seed);
      }
    }
    for (int32_t &value : affine.t) {
      value = (int32_t)random_word(seed);
    }
    const ProjectionParams projection{(int32_t)random_word(seed),
                                      (int32_t)random_word(seed),
                                      (uint16_t)random_word(seed),
                                      (int16_t)random_word(seed),
                                      (int32_t)random_word(seed)};
    const ModelVertex vertex{(int16_t)random_word(seed), (int16_t)random_word(seed), (int16_t)random_word(seed)};
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
  CHECK_EQ(corrupt.compared, 10u);
  CHECK_EQ(corrupt.mismatched, 1u);
  CHECK_EQ(corrupt.first, 7u);
}

static void test_flag_output_and_negative_control(void) {
  FixedAffine identity{};
  identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = 4096;
  const ProjectionParams projection{160 << 16, 120 << 16, 256};

  GteRegs ordinary_guest = make_guest(identity, projection, {4, -7, 512});
  CHECK(GTE_ExecuteIsolated(&ordinary_guest, 0x4a180001u) >= 0);
  NativeProjectedVertex ordinary = project(identity, projection, {4, -7, 512});
  CHECK_EQ(ordinary.flags, 0u);
  CHECK_EQ(compare(ordinary, ordinary_guest).mismatched, 0u);

  GteRegs clipped_guest = make_guest(identity, projection, {0, 0, 0});
  CHECK(GTE_ExecuteIsolated(&clipped_guest, 0x4a180001u) >= 0);
  NativeProjectedVertex clipped = project(identity, projection, {0, 0, 0});
  CHECK((clipped.flags & (1u << 17)) != 0);
  CHECK((clipped.flags & (1u << 31)) != 0);
  CHECK_EQ(compare(clipped, clipped_guest).mismatched, 0u);

  clipped.flags ^= 1u << 17;
  const CompareResult corrupt = compare(clipped, clipped_guest);
  CHECK_EQ(corrupt.compared, 10u);
  CHECK_EQ(corrupt.mismatched, 1u);
  CHECK_EQ(corrupt.first, 9u);
}

static void test_diagnostic_modes(void) {
  uint32_t seed = 0xa511e9b3u;
  unsigned cases = 0;
  for (unsigned n = 0; n < 256; ++n) {
    FixedAffine affine{};
    for (auto &row : affine.m) {
      for (int16_t &value : row) {
        value = (int16_t)random_word(seed);
      }
    }
    for (int32_t &value : affine.t) {
      value = (int32_t)random_word(seed);
    }
    const ProjectionParams projection{(int32_t)random_word(seed),
                                      (int32_t)random_word(seed),
                                      (uint16_t)random_word(seed),
                                      (int16_t)random_word(seed),
                                      (int32_t)random_word(seed)};
    const ModelVertex vertex{(int16_t)random_word(seed), (int16_t)random_word(seed), (int16_t)random_word(seed)};
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
      162.0f + 1.0f / 2048.0f,
      118.0f - 1.0f / 2048.0f,
      128.0f,
  };
  CHECK_EQ(compare_fractional(near, near_expected), 0u);

  FixedAffine far_affine{};
  far_affine.m = {{{2048, 0, 0}, {0, 1024, 0}, {0, 0, 3}}};
  far_affine.t = {{4, -3, 512}};
  NativeProjectedVertex far = project(far_affine, projection, {1, 1, 1});
  constexpr float far_z = 512.0f + 3.0f / 4096.0f;
  const FractionalExpected far_expected{
      {18432, -11264, 2097155},
      {2048, 1024, 3},
      {4.5f, -2.75f, far_z},
      160.0f + 4.5f * 256.0f / far_z,
      120.0f - 2.75f * 256.0f / far_z,
      far_z,
  };
  CHECK_EQ(compare_fractional(far, far_expected), 0u);
  // Native float channels retain fractions, while hardware channels remain independently exact.
  check_case(near_affine, projection, {1, 1, 1});
  check_case(far_affine, projection, {1, 1, 1});

  ++far.raw_view_fixed[2];
  far.px += 0.25f;
  CHECK_EQ(compare_fractional(far, far_expected), 3u);
}

static void test_continuous_view_projection_limits(void) {
  const ProjectionParams projection{160 << 16, 120 << 16, 256};
  const auto near = project_view({1.25f, -1.25f, -100.0f}, projection);
  CHECK_EQ(near.px, 162.5f);
  CHECK_EQ(near.py, 117.5f);
  CHECK_EQ(near.pz, 128.0f);

  // Depth exceeds both signed IR3 and unsigned SZ. It remains usable for native projection.
  const auto far = project_view({40000.5f, -40000.5f, 131072.0f}, projection);
  CHECK_EQ(far.px, 160.0f + 32767.0f / 512.0f);
  CHECK_EQ(far.py, 56.0f);
  CHECK_EQ(far.pz, 131072.0f);
  const auto fractional_depth = project_view({0.0f, 0.0f, 40000.25f}, projection);
  CHECK_EQ(fractional_depth.pz, 40000.25f);

  const auto clipped = project_view({32767.0f, -32768.0f, 0.0f}, projection);
  CHECK_EQ(clipped.px, 1023.0f);
  CHECK_EQ(clipped.py, -1024.0f);
  const auto zero = project_view({1.0f, -1.0f, 0.0f}, {160 << 16, 120 << 16, 0});
  CHECK_EQ(zero.px, 160.0f);
  CHECK_EQ(zero.py, 120.0f);
  CHECK_EQ(zero.pz, 0.0f);
}

static void test_diagnostic_float_inputs_remain_ir_values(void) {
  FixedAffine affine{};
  affine.m = {{{2048, 0, 0}, {0, -2048, 0}, {0, 0, 4096}}};
  affine.t = {{0, 0, 512}};
  const ProjectionParams projection{160 << 16, 120 << 16, 128};
  const auto unshifted = detail::project_gte_mode(affine, projection, {1, 1, 0}, 0, false);
  CHECK_EQ(unshifted.px, 672.0f);
  CHECK_EQ(unshifted.py, -392.0f);
  const auto limited = detail::project_gte_mode(affine, projection, {1, 1, 0}, 0, true);
  CHECK_EQ(limited.px, 672.0f);
  CHECK_EQ(limited.py, 120.0f);
  const auto shifted_limited = detail::project_gte_mode(affine, projection, {1, 1, 0}, 12, true);
  CHECK_EQ(shifted_limited.px, 160.0f);
  CHECK_EQ(shifted_limited.py, 120.0f);
}

} // namespace

int main() {
  GTE_Init();
  RUN(random_and_edges);
  RUN(forced_mismatch_other_answer);
  RUN(flag_output_and_negative_control);
  RUN(diagnostic_modes);
  RUN(fractional_endpoint_channels);
  RUN(continuous_view_projection_limits);
  RUN(diagnostic_float_inputs_remain_ir_values);
  return pt_summary();
}
