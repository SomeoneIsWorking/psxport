// test_swapchain_present_mode.cpp — the sink must not stall the thread that produces the frames.
//
// THE BUG THIS PINS. gpu_vk.cpp claimed the window (SDL_ClaimWindowForGPUDevice) and never called
// SDL_SetGPUSwapchainParameters, so the swapchain kept SDL's DEFAULT present mode: VSYNC. Under VSYNC
// SDL_WaitAndAcquireGPUSwapchainTexture BLOCKS the calling thread until the next vblank, and its caller
// (GpuVkState::show_present_image) runs on the GUEST thread — vblank_advance -> gpu_present, reached from
// every VSync(), every field wait and every host turn. There is no I/O thread, so the CD pump, MDEC and
// DMA completion were blocked along with the guest. Headless never blocked (no window, acquire fails
// instantly), which is exactly why headless looked fine while the user's window was black.
//
// The testable unit is the present-mode CHOICE, extracted into runtime/recomp/gpu_vk_present_mode.h.
// It is a pure function of what the driver reports, so this test needs no GPU, no window and no disc.
//
// THE NEGATIVE THESE CASES WERE WRITTEN AGAINST: the pre-fix behaviour is "always VSYNC, the mode is
// never even requested". Every case below asserts something that behaviour cannot satisfy — see
// test_never_settles_for_a_blocking_mode_when_a_nonblocking_one_exists, which fails outright unless a
// non-blocking mode is actually chosen. VSYNC is asserted in exactly ONE case (neither alternative is
// offered), so a regression back to "always VSYNC" turns 3 of 4 cases red, not 1.
#include "../runtime/recomp/gpu_vk_present_mode.h"
#include "testutil.h"

// MAILBOX is the first choice whenever the driver offers it: newest-image-wins, no tearing, no wait.
static void test_mailbox_wins_when_available(void) {
  CHECK_EQ(preferred_present_mode(true, true), SDL_GPU_PRESENTMODE_MAILBOX);
  // ...and it is still first even if the driver somehow reports mailbox without immediate.
  CHECK_EQ(preferred_present_mode(true, false), SDL_GPU_PRESENTMODE_MAILBOX);
}

// No mailbox but immediate available -> IMMEDIATE. Tearing is a picture; a stalled guest thread is not.
static void test_immediate_when_no_mailbox(void) {
  CHECK_EQ(preferred_present_mode(false, true), SDL_GPU_PRESENTMODE_IMMEDIATE);
}

// Neither offered -> VSYNC, which the spec guarantees is always supported. This is the ONLY case in
// which the blocking mode is the right answer, and it is here so the fallback is pinned too: a
// "preference" that could return an UNSUPPORTED mode would fail SDL_SetGPUSwapchainParameters and leave
// the swapchain on its default — the original bug, one level up.
static void test_vsync_only_when_nothing_else_is_offered(void) {
  CHECK_EQ(preferred_present_mode(false, false), SDL_GPU_PRESENTMODE_VSYNC);
}

// The property the guest thread actually cares about, asserted over the WHOLE input space rather than
// mode by mode: if any non-blocking mode is available, the choice must be non-blocking. This is the
// case that is red against "always VSYNC".
static void test_never_settles_for_a_blocking_mode_when_a_nonblocking_one_exists(void) {
  int scanned = 0, nonblocking = 0;
  for (int mb = 0; mb < 2; mb++)
    for (int im = 0; im < 2; im++) {
      SDL_GPUPresentMode m = preferred_present_mode(mb != 0, im != 0);
      bool any_nonblocking_offered = (mb != 0) || (im != 0);
      // The choice must never be a mode the driver did not offer (VSYNC is always offered).
      CHECK(m != SDL_GPU_PRESENTMODE_MAILBOX || mb != 0);
      CHECK(m != SDL_GPU_PRESENTMODE_IMMEDIATE || im != 0);
      // ...and it must never block when it did not have to.
      CHECK_EQ(present_mode_blocks_caller(m), !any_nonblocking_offered);
      if (!present_mode_blocks_caller(m)) nonblocking++;
      scanned++;
    }
  CHECK_EQ(scanned, 4);        // the denominator: the full 2x2 input space, every combination checked
  CHECK_EQ(nonblocking, 3);    // 3 of the 4 combinations offer a non-blocking mode; all 3 must take it
}

// Sanity on the predicate itself, so the case above cannot pass by the predicate being vacuously false.
static void test_blocking_predicate_discriminates(void) {
  CHECK_EQ(present_mode_blocks_caller(SDL_GPU_PRESENTMODE_VSYNC), true);
  CHECK_EQ(present_mode_blocks_caller(SDL_GPU_PRESENTMODE_MAILBOX), false);
  CHECK_EQ(present_mode_blocks_caller(SDL_GPU_PRESENTMODE_IMMEDIATE), false);
}

// The log line the sink prints must name the mode, and must never hand std::format a null pointer.
static void test_mode_name_is_never_null(void) {
  CHECK_STREQ(present_mode_name(SDL_GPU_PRESENTMODE_VSYNC), "VSYNC");
  CHECK_STREQ(present_mode_name(SDL_GPU_PRESENTMODE_IMMEDIATE), "IMMEDIATE");
  CHECK_STREQ(present_mode_name(SDL_GPU_PRESENTMODE_MAILBOX), "MAILBOX");
  CHECK(present_mode_name((SDL_GPUPresentMode)9999) != NULL);   // out-of-range must still be printable
}

int main(void) {
  RUN(mailbox_wins_when_available);
  RUN(immediate_when_no_mailbox);
  RUN(vsync_only_when_nothing_else_is_offered);
  RUN(never_settles_for_a_blocking_mode_when_a_nonblocking_one_exists);
  RUN(blocking_predicate_discriminates);
  RUN(mode_name_is_never_null);
  return pt_summary();
}
