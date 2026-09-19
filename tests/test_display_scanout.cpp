// test_display_scanout — the presented row count and the scanned row count are different numbers.
//
// REGRESSION UNDER TEST (Tomba! 2, 2026-09-19). The first presented-picture comparison against the
// Beetle console reference read 70,958 of 76,800 pixels differing (92.39%) and reported a second
// "defect": the land/horizon boundary at row 87 in the product against row 94 on the reference, so
// "the world is drawn 7 pixels high". Neither was real.
//
// The native render path deliberately presents MORE rows than the console scanned out. Tomba! 2
// declares guestDisplayHeight = 224 while the framework's decoded height stays at its 240-line
// default (the title HLEs its display setup, so GP1(07) is never written), and the extra rows are
// framebuffer the console never showed. The decision is recorded in gpu_native.cpp (USER
// 2026-08-19: "PC is fine, oracle isn't"). Comparing the raw frames measured that difference and
// called it a defect; cropping to the scanned rows dropped the same comparison to 47.13% with a
// MEDIAN per-pixel difference of zero.
//
// So the two numbers are asserted separately here, in every combination that produced a wrong
// answer once: a guest path presents what it declares, a native path presents more, and BOTH agree
// on how many of those rows a console would have scanned.
//
// Hermetic: the rule is one pure constexpr function. No GPU, no window, no disc.
#include "display_scanout.h"

#include "testutil.h"

#include <initializer_list>

using psxport::display::scanout;

static void test_tomba2_native_presents_more_rows_than_a_console_scans(void) {
  // Tomba! 2's real numbers: decoded 240 (the default, never written), declared 224.
  const auto guest = scanout(240, 224, false);
  CHECK_EQ(guest.presented, 224); // a guest path claims to show what the console showed
  CHECK_EQ(guest.scanned, 224);

  const auto native = scanout(240, 224, true);
  CHECK_EQ(native.presented, 240); // the native renderer owns its frame and draws more
  CHECK_EQ(native.scanned, 224);   // ... but a console would still have scanned only 224

  // The two must not be conflated: this inequality IS the retracted defect above.
  CHECK(native.presented != native.scanned);
}

static void test_an_undeclared_height_gives_the_decoded_one_for_both(void) {
  // Returning 0 or the framework default as a "scanned" count would make a picture oracle crop to
  // nothing or to a number nobody stated.
  for (const bool is_native : {false, true}) {
    const auto undeclared = scanout(240, 0, is_native);
    CHECK_EQ(undeclared.presented, 240);
    CHECK_EQ(undeclared.scanned, 240);
    const auto shorter = scanout(224, 0, is_native);
    CHECK_EQ(shorter.presented, 224);
    CHECK_EQ(shorter.scanned, 224);
  }

  // A negative or absurd declaration is treated as no declaration rather than propagated into a
  // crop: a picture tool that cropped to a negative row count would refuse with a confusing reason
  // instead of the real one.
  const auto negative = scanout(240, -8, true);
  CHECK_EQ(negative.presented, 240);
  CHECK_EQ(negative.scanned, 240);
}

static void test_a_declaration_that_agrees_changes_nothing(void) {
  // The ordinary case, on both paths, so a port that declares the truth is never penalised for it.
  for (const bool is_native : {false, true}) {
    const auto agreeing = scanout(240, 240, is_native);
    CHECK_EQ(agreeing.presented, 240);
    CHECK_EQ(agreeing.scanned, 240);
  }

  // A port may also declare MORE than was decoded (an interlaced field doubled, say). The scanned
  // count is what the port said, and a guest path presents it; nothing here silently clamps.
  const auto taller = scanout(240, 480, false);
  CHECK_EQ(taller.presented, 480);
  CHECK_EQ(taller.scanned, 480);
}

int main(void) {
  RUN(tomba2_native_presents_more_rows_than_a_console_scans);
  RUN(an_undeclared_height_gives_the_decoded_one_for_both);
  RUN(a_declaration_that_agrees_changes_nothing);
  return pt_summary();
}
