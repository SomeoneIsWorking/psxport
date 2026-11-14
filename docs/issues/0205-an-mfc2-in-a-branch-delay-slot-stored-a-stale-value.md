# 0205: an MFC2 in a branch delay slot stored a stale value

Status: fixed. Owner: Lightrec fork `lightrec.c:lightrec_mfc_cb` and `emitter.c:rec_mfc`, fork commit
`5aa9bdfcf330ce4140898cc89c8516c78dcdff8e`, pinned by `cmake/lightrec_dependency.cmake`.

## Observed
Crash 3, `warp scene 0x0E` (and 0x1C), guest world renderer: polygons whose last corner is depth-cued show
bright green and blue corners (psx-crash issue 0030). The drawers read the DPCS result with an `mfc2` in the
delay slot of the branch back to the default drawer (`0x800429DC`/`0x800429E0` -> `0x80041768`,
`0x80042B3C`/`0x80042B40` -> `0x80041A3C`). At the colour stores `0x80041888` (GT3 + 0x1C) and `0x80041B40`
(GT4 + 0x28), 6 s after the warp, 67,328 of 98,589 stored words equalled neither GTE RGB2 (the DPCS result) nor
RGBC (the undimmed colour the bypass path stores).

## Cause
psxport sets `cop2_notify`, so `rec_cp2_do_mfc2` and `rec_cp2_basic_CFC2` emit `rec_mfc`, a call to the C
wrapper `lightrec_mfc_cb`, which wrote `gpr[rt]` even when the opcode carries the load-delay flag. The branch's
end of block then jumps to the load-delay check, which writes `gpr[rt]` from `temp_reg`, never loaded by that
path. The JIT-only path (no `cop2_notify`) already wrote `REG_TEMP`. Upstream `pcercuei/lightrec` has the same
code at 465e82e.

## Resolution
`rec_mfc` clears `in_delay_slot_n` for a load-delay MFC/CFC (as `rec_io` does for loads) and drops a cached
`REG_TEMP`; `lightrec_mfc_cb` then writes `temp_reg` and re-arms the flag, the same contract as
`lightrec_rw_helper`.

## Shown
- Same stores, new pin: 0 stale; every store equals RGB2 or RGBC (90,098 and 41,566).
- `PSXPORT_OVERRIDE_DIFF=0x80040ED8`, every call compared, 6 s a scene: 0x0E 603 of 603 equal (old pin 455 of
  456 differed), 0x1C 646 of 646 equal. The guest-rendered 0x0E frame has no wrong corner colours.
- Tomba 2 (`warp 1/0` at step 1000, then `0/0`, `PSXPORT_AUTO_SKIP=1`) and Spyro 3 (`warp 0/1/0`, gameplay at
  step 1046) land step for step in the same area, state and position as the old pin. All runs unpaced.
