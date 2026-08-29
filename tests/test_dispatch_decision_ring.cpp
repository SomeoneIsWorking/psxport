// test_dispatch_decision_ring.cpp — the crashbash-0018 discriminator's memory side.
//
// The flaky first-boot recomp-MISS at 0x80012840 printed an address whose main_dispatch case EXISTS,
// so the miss diagnostic provably misreports its own cause — and the flake never fires under channel
// instrumentation, so a printf-at-decision-time probe changes the timing it is meant to observe. The
// fix is a fixed per-core ring (InterpDiag::dispdec) recorded unconditionally with NO I/O, dumped
// only when the fatal miss fires. These tests pin the ring's contract:
//
//   1. record+dump reproduce the entries OLDEST FIRST, in order, with kind/addr/ra/aux intact;
//   2. the ring WRAPS: after more than DISPDEC_CAP entries the dump shows exactly the newest
//      DISPDEC_CAP entries (a miss diagnostic needs the recent past, not history);
//   3. the dump is the ONLY output — recording itself touches no sink (that is the whole point:
//      a recording run must have the same output shape as a plain run).

#include "interp_diag.h"
#include "testutil.h"

#include <lucent/log.h>

#include <string>
#include <vector>

namespace {

void test_dump_is_oldest_first_with_fields_intact() {
  InterpDiag d;
  d.dispdecRecord(0x80012420u, 0x80012340u, InterpDiag::DISPDEC_ENTER);
  d.dispdecRecord(0x80012840u, 0x80012428u, InterpDiag::DISPDEC_MAIN);
  d.dispdecRecord(0x800C0B94u, 0x80078D10u, InterpDiag::DISPDEC_MISS);

  std::vector<std::string> lines;
  lucent::set_sink([&](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });
  d.dumpDispdec();
  lucent::set_sink(nullptr);

  CHECK(lines.size() == 4); // header + 3 entries
  CHECK(lines[0].find("3 of 128 slots") != std::string::npos);
  CHECK(lines[1].find("[0] ENTER addr=0x80012420 ra=0x80012340") != std::string::npos);
  CHECK(lines[2].find("[1] MAIN addr=0x80012840 ra=0x80012428") != std::string::npos);
  CHECK(lines[3].find("[2] MISS addr=0x800C0B94 ra=0x80078D10") != std::string::npos);
}

void test_ring_wraps_and_keeps_the_newest_cap_entries() {
  InterpDiag d;
  for (int i = 0; i < InterpDiag::DISPDEC_CAP + 10; i++) {
    d.dispdecRecord(0x80010000u + (uint32_t)i, 0x80009000u, InterpDiag::DISPDEC_MAIN, (uint32_t)i);
  }
  CHECK(d.dispdec_n == InterpDiag::DISPDEC_CAP);

  std::vector<std::string> lines;
  lucent::set_sink([&](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });
  d.dumpDispdec();
  lucent::set_sink(nullptr);

  CHECK(lines.size() == (size_t)InterpDiag::DISPDEC_CAP + 1);
  // The oldest SURVIVING entry is #10 (the first 10 wrapped away).
  CHECK(lines[1].find("[0] MAIN addr=0x8001000A") != std::string::npos);
  // Exact newest line: aux carries the write counter, which pins that aux survives wrapping too.
  // Writes 0..137; the newest surviving is #137: addr 0x80010000+137 = 0x80010089, aux 137 = 0x89.
  CHECK(lines[lines.size() - 1].find("[127] MAIN addr=0x80010089") != std::string::npos);
  CHECK(lines[lines.size() - 1].find("aux=0x00000089") != std::string::npos);
}

void test_recording_emits_nothing() {
  InterpDiag d;
  std::vector<std::string> lines;
  lucent::set_sink([&](lucent::Level, std::string_view line) {
    lines.emplace_back(line);
  });
  for (int i = 0; i < 1000; i++) {
    d.dispdecRecord(0x80012840u, 0x80012428u, InterpDiag::DISPDEC_MAIN);
  }
  lucent::set_sink(nullptr);
  CHECK(lines.empty());
}

} // namespace

int main(void) {
  RUN(dump_is_oldest_first_with_fields_intact);
  RUN(ring_wraps_and_keeps_the_newest_cap_entries);
  RUN(recording_emits_nothing);
  return pt_summary();
}
