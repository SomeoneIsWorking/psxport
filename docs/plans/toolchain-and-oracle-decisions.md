# Standing decisions: clang-format, and what was reversed

**USER, 2026-08-20**, in order. The reversals are as authoritative as the originals — this file
records the FINAL position, not the history of it.

| decision | status |
|---|---|
| adopt **clang-format**, and *"accomodate to the formatter not the other way around"* | **STANDS** — done, whole tree |
| move the build to **clang** | **STANDS** — done; the normal gate rejects non-Clang C++ compile commands |
| add **clang-tidy** | **STANDS** — done; the normal gate checks every compile-backed first-party C++ TU |
| drop **`extern "C"`** | **REVERSED** — *"revert ... extern C"*. Kept. |
| drop **beetle** | **REVERSED** — *"revert 'Don't use beetle'"*. Kept, GPU oracle included. |
| **never duplicate code, no matter the reason** | new, standing — see `AGENTS.md` |

## clang-format — applied, and now enforced

The whole first-party tree is formatted: 280 files, which held **56,735 violation sites** because
`check_cpp_style.py` only ever checked the six files in its `FILE_CAPS` dict. That gate now
format-checks **every** first-party source and prints its own denominator; `vendor/` stays excluded
deliberately, since reformatting it would make every future upstream diff unreadable.

Three things to know before repeating this anywhere:

- **`.clang-format` sets no `PointerAlignment`, so LLVM's `Right` default applies** and the tree is
  now `Core *c`, not `Core* c`. That flip is most of the diff. It was NOT overridden, on the USER's
  instruction to accommodate the formatter rather than bend it — the config is the authority.
- **The sweep needs two passes to converge.** One `clang-format -i` pass left 8 violations; a second
  reached 0.
- The sweep itself:
  ```sh
  git ls-files '*.cpp' '*.h' '*.c' '*.hpp' | grep -v '^vendor/' | xargs clang-format -i   # twice
  ```

## clang as the compiler — WORKS, out of the box

**The blocker recorded here earlier was wrong.** This file used to say "expect real work; the vendored
beetle C is only known to build under GCC here". That was asserted without testing, and testing it
took one configure:

```sh
cmake -S . -B build_clang -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
```

| | result |
|---|---|
| psxport library + tools + tests | **0 errors, 0 warnings** — vendored beetle C included |
| psxport hermetic suite | 62 of 63, the same single `game_iface.h` cap failure GCC gives |
| Tomba!2 game (incl. ~200 MB generated substrate) | **0 errors** |
| behaviour, `vram_oracle.py` f1120 psx path | **identical to GCC**: 368 = 368 prims, 0/524,288 differing |

**ONE TRAP, and it is worth knowing because it looks like a clang bug and is not.** Configure without
`CMAKE_BUILD_TYPE` and you get no `-DNDEBUG`, so vendored asserts go live — and one of them fires
immediately:

```
gpu.c:2058: GPU_Update: Assertion `GPU.sl_zero_reached == false' failed.
```

That was a REAL defect in our beetle GPU oracle (kanban #116), not a compiler difference. The adapter
used a synthetic `GPU_Update` clock merely to drain input; a large upload could cross scanline zero
multiple times inside one guest frame. `GPU_WriteCB` already drains synchronously after draw time is
granted, so the adapter now advances no scanout clock and calls `GPU_StartFrame` only at the real guest
boundary. An assert-enabled Clang build passes the reproducer; keep that build class around, because a
Release build compiled out the only signal that exposed the violation.

## What the formatter exposed — and why it was never the formatter's fault

Include sorting broke the build, which looked like a formatter problem and was not. `rec_coro_run`
was declared **twice**: in `core.h` inside its `extern "C"` block, and in `scheduler.h` with ordinary
C++ linkage. The two disagreed about linkage, and which one won depended on include order. It
compiled only by luck, and sorting the includes spent that luck.

The fix is the code's, not the config's: **one owner per declaration**. A sweep for the same shape —
every function declared in more than one first-party header — found three more, two of them carrying
the identical latent mismatch:

| duplicate | where | fixed by |
|---|---|---|
| `rec_coro_run` | `core.h` (in `extern "C"`) + `scheduler.h` | deleted from `scheduler.h`; all 8 users already include `core.h` |
| `rec_dispatch` | `core.h` (in `extern "C"`) + `guest_abi.h` + `guest_call.h` | deleted from both; both already `#include "core.h"` |
| `xa_decode_sector` | `c_subsys.h` + `fmv_decode.h` | `fmv_decode.h` now includes `c_subsys.h` |

`reset_for_test` also appears twice but legitimately — `config_var.h`'s is a `friend` declaration.

Re-run the finder after any header churn; it is 15 lines of Python over `git ls-files '*.h'`, matching
declarations by name and reporting any name declared in more than one header.

## `extern "C"` — KEPT

The earlier reasoning for dropping it ("the shards are C++, no C translation unit includes `core.h`")
was correct as far as it went, and is now moot: the USER reversed it. It stays.

Note for whoever revisits this: it was never load-bearing for the recompiled substrate, but it **is**
load-bearing for the beetle adapters (`gte_beetle.cpp`, `gpu_beetle.cpp`, `spu_beetle.cpp`,
`hw_bind.cpp`, and `mdec_beetle.c`, a genuine C file), which link against vendored code compiled as
real C. Removing it there is a link error, not a cleanup.

## beetle — KEPT, oracle included

Vendored for four subsystems: the **GPU oracle** (`gpu_beetle.cpp`, a tee, off by default behind
`PSXPORT_GPU_BEETLE`) and the **GTE / MDEC / SPU** backends, which are the port's actual geometry, FMV
and audio.

The oracle earned its keep the day this was written: kanban #110 (calibration), #111 (a black screen
recorded as "psx_render draws literally nothing", actually a correct picture being cleared away),
#112 (interpolated colour truncated where hardware rounds), #113 (dither disabled by a primitive's
texpage word), #114 (the named residual floor). Two of four measured screens are now pixel-identical
to real hardware.
