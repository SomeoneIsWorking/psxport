// sbs_raylib_stub.cpp — the raylib SBS two-pane presenter (sbs_raylib.cpp) is DROPPED from the SDL_GPU
// build (raylib is a dead end — see docs/render-backend-port.md). sbs.cpp still references these symbols,
// so they are stubbed as inert no-ops. PSXPORT_SBS (g_sbs) defaults off; the SDL_GPU SBS composite is a
// later pass. should_close()==1 so any accidental SBS loop exits immediately instead of spinning.
extern "C" {
void sbs_rl_init(void) {}
int  sbs_rl_should_close(void) { return 1; }
unsigned short sbs_rl_poll_input(void) { return 0xFFFF; }   // active-low: nothing pressed
void sbs_rl_present(const unsigned char* a, int wA, int hA, const unsigned char* b, int wB, int hB) {
  (void)a; (void)wA; (void)hA; (void)b; (void)wB; (void)hB;
}
void sbs_rl_shutdown(void) {}
}
