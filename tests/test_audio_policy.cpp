// test_audio_policy — headless implies NO AUDIO DEVICE, on EVERY audio path, not just the SPU one.
//
// THE BUG THIS PINS (USER, 2026-08-06: "a tomba gate plays audible fmv"). The rule "headless means no
// window and no audio" was implemented twice and one copy dropped the headless half:
//     spu_audio.cpp    if (no_audio() || !gpu_vk_windowed())  correct
//     native_fmv.cpp   if (no_audio())                      MISSING -> FMV sound on every gate run
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

#include "audio_policy.h"
#include "source_tree.h"
#include "testutil.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace {

// ---- 1. the predicate, both classes ------------------------------------------------------------
void test_headless_never_opens_audio(void) {
  // windowed and not silenced -> the ONLY case that may open a device
  CHECK_EQ(psx::audio::mayOpenOutput(/*noaudio=*/false, /*windowed=*/true), true);

  // headless: silent regardless of the knob. This is the arm native_fmv.cpp was missing.
  CHECK_EQ(psx::audio::mayOpenOutput(/*noaudio=*/false, /*windowed=*/false), false);
  CHECK_EQ(psx::audio::mayOpenOutput(/*noaudio=*/true, /*windowed=*/false), false);

  // explicit opt-out beats a window
  CHECK_EQ(psx::audio::mayOpenOutput(/*noaudio=*/true, /*windowed=*/true), false);
}

// ---- 2. the lint: no audio device is opened outside the shared predicate ------------------------
void test_every_audio_open_site_consults_the_shared_predicate(void) {
  // The framework's audio paths. Listed rather than globbed BECAUSE a new audio path must be a
  // deliberate edit here — that is the point of the lint, and a glob would silently absolve a file
  // nobody thought about.
  const char *srcs[] = {
      "runtime/psx/audio/spu_audio.cpp",
      "runtime/psx/movie/native_fmv.cpp",
  };

  int scanned = 0, sites = 0, guarded = 0;
  for (const char *rel : srcs) {
    std::string s = psx::test::readRepoFile(rel);
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
      const size_t use = s.rfind("psx::audio::mayOpenOutput", p);
      if (use != std::string::npos) {
        guarded++;
      } else {
        printf("  FAIL %s: an SDL_OpenAudioDeviceStream site is not guarded by psx::audio::mayOpenOutput()\n", rel);
      }
    }
  }

  // Denominators, so a green line can never mean "the lint found nothing to check".
  printf("  [lint] audio: scanned %d file(s), %d device-open site(s), %d guarded\n", scanned, sites, guarded);
  CHECK_EQ(scanned, 2);
  CHECK(sites >= 2); // one per audio path; fewer means a path vanished and this list is stale
  CHECK_EQ(guarded, sites);
}

} // namespace

int main(void) {
  printf("test_audio_policy: headless implies no audio device, on every path\n");
  RUN(headless_never_opens_audio);
  RUN(every_audio_open_site_consults_the_shared_predicate);
  return psx::test::summary();
}
