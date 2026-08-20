// test_audio_policy — headless implies NO AUDIO DEVICE, on EVERY audio path, not just the SPU one.
//
// THE BUG THIS PINS (USER, 2026-08-06: "a tomba gate plays audible fmv"). The rule "headless means no
// window and no audio" was implemented twice and one copy dropped the headless half:
//     spu_audio.cpp    if (cv_noaudio || !gpu_windowed())   correct
//     native_fmv.cpp   if (cv_noaudio)                      MISSING -> FMV sound on every gate run
// A gate is headless by construction, so every automated run in this workspace was playing movie
// audio out of the user's speakers.
//
// TWO ASSERTIONS, and the second is the one that stops the regression coming back:
//   1. the PREDICATE is right in all four input combinations (both classes, not just the failing one);
//   2. a SOURCE LINT: every SDL_OpenAudioDeviceStream call site is guarded by the shared predicate.
//      Testing only the predicate would leave a third copy free to be written tomorrow — which is
//      exactly how the second copy drifted from the first.
//
// Hermetic: no SDL, no device, no window, no disc. The lint reads the framework's own sources.

#include "../runtime/recomp/audio_policy.h"
#include "testutil.h"

#include <stdio.h>
#include <string>
#include <vector>

// ---- 1. the predicate, both classes ------------------------------------------------------------
static void test_headless_never_opens_audio(void) {
  // windowed and not silenced -> the ONLY case that may open a device
  CHECK_EQ(audio_may_open(/*noaudio=*/false, /*windowed=*/true), true);

  // headless: silent regardless of the knob. This is the arm native_fmv.cpp was missing.
  CHECK_EQ(audio_may_open(/*noaudio=*/false, /*windowed=*/false), false);
  CHECK_EQ(audio_may_open(/*noaudio=*/true, /*windowed=*/false), false);

  // explicit opt-out beats a window
  CHECK_EQ(audio_may_open(/*noaudio=*/true, /*windowed=*/true), false);
}

// ---- 2. the lint: no audio device is opened outside the shared predicate ------------------------
// Resolve a framework-relative path from THIS FILE's compile-time location, so the lint works from
// any cwd. ctest runs from the build dir, not tests/ — a relative path made the test fail there while
// passing by hand, which is a defect in the test, not in the code under test.
static std::string src_path(const char *rel) {
  std::string self = __FILE__; // .../psxport/tests/test_audio_policy.cpp
  const size_t cut = self.find_last_of('/');
  const std::string tests_dir = (cut == std::string::npos) ? std::string(".") : self.substr(0, cut);
  return tests_dir + "/../" + rel; // .../psxport/<rel>
}

static std::string slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return std::string();
  }
  std::string s;
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    s.append(buf, n);
  }
  fclose(f);
  return s;
}

static void test_every_audio_open_site_consults_the_shared_predicate(void) {
  // The framework's audio paths. Listed rather than globbed BECAUSE a new audio path must be a
  // deliberate edit here — that is the point of the lint, and a glob would silently absolve a file
  // nobody thought about.
  const char *srcs[] = {
      "runtime/recomp/spu_audio.cpp",
      "runtime/recomp/native_fmv.cpp",
  };

  int scanned = 0, sites = 0, guarded = 0;
  for (const char *rel : srcs) {
    std::string s = slurp(src_path(rel).c_str());
    if (s.empty()) { // REFUSE rather than pass vacuously
      printf("  FAIL could not read %s — the lint would have passed by seeing nothing\n", rel);
      CHECK(false);
      continue;
    }
    scanned++;
    // Every device-open call site must be preceded, in the same file, by the shared predicate.
    for (size_t p = s.find("SDL_OpenAudioDeviceStream"); p != std::string::npos;
         p = s.find("SDL_OpenAudioDeviceStream", p + 1)) {
      sites++;
      const size_t use = s.rfind("audio_may_open", p);
      if (use != std::string::npos) {
        guarded++;
      } else {
        printf("  FAIL %s: an SDL_OpenAudioDeviceStream site is not guarded by audio_may_open()\n", rel);
      }
    }
  }

  // Denominators, so a green line can never mean "the lint found nothing to check".
  printf("  [lint] audio: scanned %d file(s), %d device-open site(s), %d guarded\n", scanned, sites, guarded);
  CHECK_EQ(scanned, 2);
  CHECK(sites >= 2); // one per audio path; fewer means a path vanished and this list is stale
  CHECK_EQ(guarded, sites);
}

int main(void) {
  printf("test_audio_policy: headless implies no audio device, on every path\n");
  RUN(headless_never_opens_audio);
  RUN(every_audio_open_site_consults_the_shared_predicate);
  return pt_summary();
}
