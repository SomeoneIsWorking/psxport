# Standing decisions: clang, clang-format, no `extern "C"`, no beetle

**USER, 2026-08-20**, ending the session mid-change: *"apply clang format and use clang from now on"*,
*"I don't think we need extern \"C\""*, *"Don't use beetle"*, and — for the tree as it stands —
*"leave it in a broken state, note that we'll use clang and clang-format and we'll drop beetle and
extern C"*.

These are USER decisions. Do not re-open them as questions. This file is the note they asked for.

## 1. clang and clang-format

Format with the repo's own `.clang-format` and stop hand-formatting. Two things measured while
starting this, both worth knowing before anyone repeats it:

- **The config was never applied to the tree.** 280 first-party files (everything outside `vendor/`),
  56,735 violation sites. `check_cpp_style.py` only ever formatted the six files in its `FILE_CAPS`
  dict, so the rest of the codebase drifted freely.
- **`.clang-format` sets no `PointerAlignment`, so LLVM's default `Right` applies** — the formatted
  tree is `Core *c`, while essentially all existing code is written `Core* c`. That flip is the bulk
  of the diff (265 files, ~35.8k insertions). If `Core* c` is the intended house style, add
  `PointerAlignment: Left` to `.clang-format` BEFORE reformatting; otherwise the first run rewrites
  every pointer declaration in the tree.
- **It needs two passes to converge.** One `clang-format -i` sweep left 8 violations across 6 files;
  a second pass reached 0.

The whole sweep is one command, so nothing is lost by not committing it:

```sh
git ls-files '*.cpp' '*.h' '*.c' '*.hpp' | grep -v '^vendor/' | xargs clang-format -i   # twice
```

`vendor/` is excluded deliberately — it is third-party and a reformat there would make every future
upstream diff unreadable.

**Also asked for: use clang as the compiler.** Not started, not measured. The build is GCC today
(`/usr/lib64/ccache/cc`). Expect real work: clang is stricter about several things GCC accepts, and
the vendored beetle C is only known to build under GCC here.

## 2. Drop `extern "C"`

**It buys nothing in this codebase and it cost a real bug.** The assumption behind it was that the
recompiled substrate is C. It is not: the shards are ".c files holding C++ content" compiled with
`PROPERTIES LANGUAGE CXX` (a game's `tomba2_port.cmake`), and **no C translation unit includes
`core.h`**.

What it cost: `rec_coro_run` is declared in `core.h` inside the `extern "C"` block AND in
`scheduler.h` with ordinary C++ linkage. The two conflict as soon as an include order puts
`scheduler.h` first — which is exactly what clang-format's include sorting did. Same failure one file
later for `cpu_div` / `cpu_divu` in `mem.cpp`.

Removed so far (uncommitted, see §4): the `core.h` block wrapping `rec_dispatch` … `gpu_dma2_block`,
and the two `mem.cpp` definitions.

**THE ONE EXCEPTION, and it is not optional.** The beetle adapters (`gte_beetle.cpp`,
`gpu_beetle.cpp`, `spu_beetle.cpp`, `hw_bind.cpp`, and `mdec_beetle.c`, which is a genuine C file)
link against vendored code compiled **as C**. `extern "C"` there is load-bearing. Removing it is a
link error, not a cleanup. If §3 lands and beetle goes, this exception goes with it — which is the
tidy order to do the two in.

## 3. Drop beetle

**SCOPE IS NOT YET SETTLED and must be before anyone starts.** "Don't use beetle" was said in the
context of the GPU oracle, but beetle is vendored for four subsystems here:

| what | where | how entangled |
|---|---|---|
| GPU oracle | `gpu_beetle.cpp` | newest, self-contained, a pure tee — cheapest to remove |
| GTE | `gte_beetle.cpp` | the port's actual geometry transform, not a diagnostic |
| MDEC | `mdec_beetle.c` | FMV decode |
| SPU | `spu_beetle.cpp` | audio |

Removing the GPU oracle is a small, clean revert. Removing the GTE/MDEC/SPU backends is a port-wide
change with no replacement written. **Ask which is meant before touching anything below the GPU.**

What removing the GPU oracle costs, stated plainly and not as an argument against the decision — it
is the user's call and it is made: it found five real defects in one session that a
presented-frame comparison structurally could not (kanban #110, #111, #112, #113, #114), including a
black screen that had been recorded as "psx_render draws literally nothing" and was actually a
correct picture being cleared away. Whatever replaces it needs to answer "is this our rasterizer or
the game's packets", which is the question our own rasterizer cannot answer about itself.

The oracle is off by default (`PSXPORT_GPU_BEETLE`), so nothing needs to be rushed.

## 4. The tree as it was left — DELIBERATELY BROKEN, uncommitted

`psxport` at `2a0820a5` plus **265 uncommitted modified files**. It does NOT build:

```
mem.cpp:1312  conflicting declaration of 'void cpu_div(Core*, uint32_t, uint32_t)' with 'C' linkage
```

(that one is fixed in the working tree; the build had not been re-run when the session ended, so
expect more of the same class.)

**None of it was committed, on purpose.** The formatting is one command to regenerate (§1) and
committing a non-building tree to `main` would break every other port that builds off this checkout.
To get back to a known-good tree: `git checkout -- .` in `psxport`. To resume instead: fix the
remaining linkage conflicts by deleting the `extern "C"` (§2), keeping the beetle-adapter exception.

Tomba2Engine is untouched by this and still builds; its only dirty files are the unrelated
`fx_rope_strip.cpp` work in progress.
