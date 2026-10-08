# Beetle PSX fork (psxport runtime)

Beetle PSX is GPL-2, so we keep our changes as a **committed fork** in the
vendored submodule rather than a separate patch file. The submodule
(`vendor/beetle-psx`) is checked out on the `psxport` branch and the parent
repo's gitlink points at that fork commit; just build it as-is.

All changes are guarded by `-DPSXPORT_HOOKS`:
- `mednafen/psx/cpu.c` — per-instruction PC hook/override dispatch
  (`psxport_on_pc`): PC-keyed native overrides with instruction-signature checks
  (overlay-safe) + the executed-PC coverage bitmap.
- `mednafen/psx/cdc.c` — instant-CD data reads/seeks (consumer-conditional
  pacing); CDDA/XA streaming left native.
- `mednafen/psx/gte.c` — GTE control-register tap (rotation matrix + translation
  + OFX/OFY/H) and RTP vertex tap (RTPS/RTPT: local vertex + screen coords).
- `mednafen/psx/gpu.c` — GP0 polygon tap + GP1(0x05) display-flip tap for the
  wide60 reprojecting renderer.

The registry/consumers live in `runtime/psxport_hooks.*`, `runtime/wide60.*`,
and `runtime/games/`.

Note: with no parent remote configured yet, the fork commit lives in this repo's
submodule object store. When a remote is set up, push the `psxport` branch to a
beetle fork so the gitlink SHA is fetchable elsewhere.
