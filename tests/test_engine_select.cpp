// test_engine_select — the execution-engine selection policy (I001).
//
// psxport chose its execution substrate with a single `int use_interp`, branched on in
// dispatch.cpp (4 entry points), guest_call.h (5 call sites) and overlay_router.cpp. A boolean
// cannot name a third engine, and "any non-zero means interpreter" is exactly the shape that makes
// a newly added engine silently fall into the substrate arm.
//
// This gates the replacement: ONE total policy over a named enum, where an unknown value REFUSES
// rather than defaulting. The negative case is the point — a routing table that quietly picks the
// substrate for an engine it does not know is indistinguishable from one that works, right up until
// the JIT lands.
#include "engine_select.h"
#include "testutil.h"

using namespace psx::exec;

// Every engine the enum declares, so the totality check below has a denominator rather than
// asserting over whichever values happened to come to mind.
static const Engine kAllEngines[] = {Engine::Substrate, Engine::Interpreter, Engine::Jit};
static const int kEngineCount = (int)(sizeof(kAllEngines) / sizeof(kAllEngines[0]));

// The policy is total: every declared engine routes to its own arm, and no two share one.
static void test_route_is_total_and_injective(void) {
  CHECK_EQ(kEngineCount, (int)Engine::kCount);

  int seen[(int)Route::kCount] = {};
  for (int i = 0; i < kEngineCount; ++i) {
    Route r = route(kAllEngines[i]);
    CHECK(r != Route::Refuse);
    seen[(int)r]++;
  }
  CHECK_EQ(seen[(int)Route::Substrate], 1);
  CHECK_EQ(seen[(int)Route::Interpreter], 1);
  CHECK_EQ(seen[(int)Route::Jit], 1);
}

// The named routes, spelled out — so a table edit that swaps two arms fails here and not in a game.
static void test_route_mapping(void) {
  CHECK(route(Engine::Substrate) == Route::Substrate);
  CHECK(route(Engine::Interpreter) == Route::Interpreter);
  CHECK(route(Engine::Jit) == Route::Jit);
}

// THE NEGATIVE: a value outside the enum must REFUSE, not fall through to the shipping substrate.
// A silent default here is how "the JIT is selected" and "the JIT never ran" become the same run.
static void test_unknown_engine_refuses(void) {
  int refused = 0;
  const int kProbes = 8;
  for (int v = (int)Engine::kCount; v < (int)Engine::kCount + kProbes; ++v) {
    if (route((Engine)v) == Route::Refuse) {
      refused++;
    }
  }
  CHECK_EQ(refused, kProbes); // denominator: every out-of-range value probed, not "no hits"
}

// Every engine has a distinct, non-empty name. Refusal messages and the `cvars`/SBS diagnostics
// print these; two engines sharing a name makes a divergence report unreadable.
static void test_names_are_distinct(void) {
  for (int i = 0; i < kEngineCount; ++i) {
    const char *ni = name(kAllEngines[i]);
    CHECK(ni != nullptr);
    CHECK(ni[0] != '\0');
    for (int j = i + 1; j < kEngineCount; ++j) {
      CHECK(strcmp(ni, name(kAllEngines[j])) != 0);
    }
  }
  CHECK(name((Engine)((int)Engine::kCount + 1)) != nullptr); // never returns null
}

// name() and engine_parse() must be inverses over the WHOLE enum. A new engine that gains a route
// but no spelling is an engine nobody can select from PSXPORT_ENGINE, which reads at runtime exactly
// like an engine that was selected and never ran.
static void test_name_parse_round_trips(void) {
  CHECK_EQ(kNameCount, (int)Engine::kCount);
  for (int i = 0; i < kEngineCount; ++i) {
    Engine got = Engine::kCount;
    CHECK(engine_parse(name(kAllEngines[i]), &got));
    CHECK(got == kAllEngines[i]);
  }
}

// Case-insensitive, matching PSXPORT_RENDER_PATH — a knob that works only in lower case is a knob
// that silently did nothing for whoever shouted it.
static void test_parse_ignores_case(void) {
  Engine got = Engine::kCount;
  CHECK(engine_parse("INTERPRETER", &got));
  CHECK(got == Engine::Interpreter);
  CHECK(engine_parse("Jit", &got));
  CHECK(got == Engine::Jit);
}

// THE SECOND NEGATIVE: nothing unrecognised may resolve. `*out` must also survive untouched, so a
// caller that ignores the false still cannot end up running an engine it did not ask for.
static void test_unparseable_names_are_rejected(void) {
  const char *kBad[] = {nullptr, "", " ", "substrat", "substratee", "0", "1", "true", "interp", "recomp", "lightrec"};
  const int kBadCount = (int)(sizeof(kBad) / sizeof(kBad[0]));
  int rejected = 0;
  for (int i = 0; i < kBadCount; ++i) {
    Engine got = Engine::Jit; // a sentinel that is NOT the default, so a silent write is visible
    if (!engine_parse(kBad[i], &got) && got == Engine::Jit) {
      rejected++;
    }
  }
  CHECK_EQ(rejected, kBadCount); // denominator: every bad spelling probed
}

// The shipping default must remain the substrate: this refactor changes no behaviour.
static void test_default_engine_is_substrate(void) {
  CHECK(kDefault == Engine::Substrate);
  CHECK_EQ((int)Engine::Substrate, 0); // the old `use_interp == 0` value, preserved
}

int main(void) {
  RUN(route_is_total_and_injective);
  RUN(route_mapping);
  RUN(unknown_engine_refuses);
  RUN(names_are_distinct);
  RUN(name_parse_round_trips);
  RUN(parse_ignores_case);
  RUN(unparseable_names_are_rejected);
  RUN(default_engine_is_substrate);
  return pt_summary();
}
