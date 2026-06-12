# Beetle PSX patches (psxport runtime)

The submodule gitlink stays pinned to clean upstream; modifications live here.
After `git submodule update --init`, apply with:

```
cd vendor/beetle-psx && git apply ../../patches/beetle-psx/0001-psxport-hooks.patch
```

- `0001-psxport-hooks.patch` — per-instruction hook point in the CPU
  interpreter (`mednafen/psx/cpu.c`, guarded by `-DPSXPORT_HOOKS`): PC-keyed
  native overrides with instruction-signature checks (overlay-safe) and the
  executed-PC coverage bitmap. The registry lives in `runtime/psxport_hooks.*`.

Beetle PSX is GPL-2: the combined runtime is distributable with source.
