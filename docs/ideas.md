# Ideas

Maybes not yet asked for as work.

- **Direct demo entry for selector panels.** A panel could boot straight into its title's attract demo: once the
  guest's own init is done, call the demo-start owner the title screen's idle timeout calls, at a frame boundary like
  Tomba 2's warp, so no logo, intro or title screen runs in the panel. It needs the demo start read from each title's
  decomp and a `GameRuntime` hook each title implements. Today a panel runs its title's own Start skips in preview
  and reaches the demo when the title screen times out.
