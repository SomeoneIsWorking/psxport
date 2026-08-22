#pragma once
// THE DISPLAY FIELD RATE — one definition, in milli-hertz.
//
// WHY MILLI-HERTZ. NTSC's field rate is 60000/1001 Hz = 59.94005... — not representable in integer
// Hz, and not equal to 60. The framework's other field-clock consumer already speaks milli-hertz
// (`rec_host_turn_register(core, fn, fps_millihz)`), so this is the unit that already existed rather
// than a new one.
//
// WHY THIS HEADER EXISTS AT ALL. The frame pacer computed its sleep deadline as `quota * 1000.0 /
// 60.0` — a literal 60.000 Hz — while the port counting the fields it was pacing against used the
// real rate. Two clocks at different rates across one wait loop is a beat, and a beat in a wait loop
// is what reaches the screen. Fixing that by writing 59940 at the pacer would have been the same
// bug with a different number, so the rate has exactly ONE spelling per standard, here, and the
// framework READS which standard the game is in from the game itself: GP1(0x08) bit 3, decoded in
// GpuState::gpu_gp1 into `s_disp_pal` and served by `frame_pacer.cpp`.
//
// PAL IS UNTESTED IN THIS WORKSPACE — all three consuming games are NTSC discs, so the PAL arm has
// never executed on real data. It is stated as the documented hardware rate rather than measured,
// and that is exactly what it should say here rather than being quietly presented as verified.
enum : unsigned {
  FIELD_RATE_NTSC_MILLIHZ = 59940u, // 60000/1001 Hz
  FIELD_RATE_PAL_MILLIHZ = 50000u,  // 50 Hz
  // Nominal non-interlaced field geometry used by the PSX GPU and HBlank-clocked root counter 1.
  // Interlaced hardware alternates adjacent line counts; the deterministic framework clock does
  // not yet model field parity, so it retains the standard's nominal complete-field denominator.
  DISPLAY_LINES_NTSC = 263u,
  DISPLAY_LINES_PAL = 314u,
};

inline unsigned field_rate_millihz(bool pal) {
  return pal ? FIELD_RATE_PAL_MILLIHZ : FIELD_RATE_NTSC_MILLIHZ;
}

inline unsigned display_lines_per_field(bool pal) {
  return pal ? DISPLAY_LINES_PAL : DISPLAY_LINES_NTSC;
}
