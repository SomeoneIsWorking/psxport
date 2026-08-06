#pragma once

// ---- "May this run open an audio device?" — ONE definition, for every audio path -----------------
//
// USER RULE (coord/PROTOCOL.md): **headless means exactly two things — no window surface and no audio
// device.** Audio is not a separate opt-out; it is half of what headless already means, and
// config_vars.h has said so in words since PSXPORT_NOAUDIO was declared.
//
// WHY THIS HEADER EXISTS. The rule was implemented TWICE, and one copy was wrong:
//
//     spu_audio.cpp:95    if (cv_noaudio.get() || !gpu_windowed()) ...   correct
//     native_fmv.cpp:120  if (cv_noaudio.get()) ...                      MISSING the headless half
//
// So SPU audio went quiet on an automated run and FMV audio did not. USER, 2026-08-06: *"a tomba gate
// plays audible fmv"* — a gate is headless by construction, and it was playing movie sound out of the
// user's speakers while they worked. Every agent gate run in this workspace has been doing that.
//
// The defect is not the missing term. It is that a one-line policy was COPIED, so the second copy
// could drift from the first and nothing could notice. A predicate written once cannot disagree with
// itself, and this file is small precisely so that using it is easier than re-deriving it.
//
// Pure and hermetic on purpose (no SDL, no globals, no Core) so a test can drive BOTH classes without
// a device, a disc or a window — the same shape as pace_plan.h and video_plan.h.

// noaudio  = the PSXPORT_NOAUDIO knob (an explicit "stay silent" from the caller).
// windowed = there is a real on-screen window (gpu_windowed()). Headless is !windowed.
//
// Returns true only when a device may be opened. There is deliberately no third state: a caller that
// wants to know "why not" should ask the two inputs, not this function.
static inline bool audio_may_open(bool noaudio, bool windowed) {
  return !noaudio && windowed;
}
