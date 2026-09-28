---
id: 0137
title: Two undefined-behaviour defects that Clang tolerated and GCC 16.2.1 did not
status: fixed
symptom: the framework's own gate disagreed with itself about the same source. `test_settings_persistence`
  is 4/4 green in a Clang build and 2/4 red in the gate's GCC build, from identical sources and
  identical flags bar the compiler. Nothing about the checkout guard was compiler-conditional; both
  failures were the same class of bug — a pointer into storage that had stopped existing.
tags: diagnostics,ub,toolchain,gate,settings
created: 2026-09-28
updated: 2026-09-28
---

## What was measured, before anything was changed

`tools/verify.py` failed on one test. The obvious reading is "the clock change broke the framework",
and the reason that reading is wrong is the whole content of this issue.

Both build trees configure the same source root, the same build type, and the same preprocessor
defines:

| | `build/` | `build/ci` (what `verify.py` uses) |
|---|---|---|
| compiler | `ccache/clang++` | `ccache/c++` → **GCC 16.2.1** |
| `test_settings_persistence` | **4/4 passed** | **2/4 passed** |

Same source file, same defines, same `-O2 -DNDEBUG -std=gnu++20`. So the guard's behaviour was
compiler-dependent, and the project's own test was the instrument that said so. The stale-artifact
explanation was checked and **refuted**: both objects were rebuilt, and the two `mods.cpp.o` files
differ while compiling the identical path, which a stale tree cannot explain.

## Defect one — `runtime/psx/mods.cpp`, the checkout guard returned a dangling pointer

`insideCheckout` returned `const char *` pointing at its own local `char buffer[4096]`. The object
dies at return, so the pointer is dangling — undefined behaviour, and GCC says so:

    w.c:10:28: warning: address of local variable 'buffer' returned [-Wreturn-local-addr]

Extracted into a standalone replica and run under both compilers at `-O2`:

| compiler | result |
|---|---|
| `clang++ -O2` | `guard returned: /tmp/.../checkout/tools` — refuses |
| `c++ -O2` (GCC 16.2.1) | `guard returned: (nullptr)` — **save NOT refused** |

GCC concludes the return value can never be usefully non-null — the object it would point at is dead —
and optimizes the **entire ancestor walk away**. The guard then silently stops guarding, and
`Mods::save()` writes settings into a source checkout, which is precisely the incident `006eb917`
introduced this function to stop.

**Fixed at the root: the function returns `bool`.** It is now impossible to hand back a pointer into a
dead frame, and the one call site already discarded the value (`(void)root`), so nothing is lost. The
GCC build went 2/4 → **4/4** on the change alone.

## Defect two — `tests/test_segment_clock.cpp`, a `Game` built before its runtime was installed

`Core::Core` reads the process-global installed runtime (`core.cpp:31`). The fixture declared:

    std::unique_ptr<Game> game = std::make_unique<Game>();   // NSDMI — runs BEFORE the body
    Fixture() {
      psxport_install_game(runtime);                          // runs AFTER
    }

**Member initialisers run before the constructor body**, so every `Game` was constructed against
whatever runtime the *previous* test had installed. For the first test in the file that was null and
the null branch saved it; for every later test it was a pointer to an already-destroyed `Runtime`.

Symptom: `test_segment_clock` **segfaulted under GCC and passed 15/15 under Clang.**

    Program received signal SIGSEGV
    #0 Core::Core (this=0xbbd230) at runtime/psx/core.cpp:31
      guestProgramImage = runtime ? runtime->guestProgramImage() : nullptr;
    #2 std::make_unique<Game> ()
    #3 (anonymous namespace)::Fixture::Fixture ()

**Fixed at the root: construct the `Game` in the constructor body, after the install** — the ordering
the dependency actually has. GCC went from SIGSEGV to **15/15, 204 checks**.

## Why this is the same finding twice, and what it says about the gate

Both defects are **undefined behaviour that the agents' mandated toolchain hides**. `AGENTS.md` says
agents build with Clang, so by construction every Clang-tolerated UB is invisible to the default
workflow — and the first sign of it is a *mismatch* between two builds, not a failure in either.

The lesson is the standing one from `docs/findings/diagnostics-that-cannot-lie.md`, arriving from a new
direction: **an instrument that has only ever been run one way cannot report the disagreement, because
there is nothing to disagree with.** Here the disagreement was the whole measurement. The two
toolchains are not redundant coverage of one subject; disagreement between them is itself a finding, and
it found a guard that had stopped guarding.

**Recommended, not done here:** `verify.py` should build one toolchain and the combined gate should
assert agreement, or at minimum record which compiler produced the receipt. Leaving `build/ci` on the
default `c++` is what made this visible **by accident** — it was configured for convenience, not as a
deliberate second opinion. The fix here makes both toolchains agree; the gate arrangement is a separate
decision and is not taken in this issue.

## Falsifiers

- If `insideCheckout` returning `bool` changed anything beyond the compiler's view of the dangling
  return, some caller was using the directory after all — there is one caller and it discarded the
  value, so a regression here would show as a compile error, not a silent behaviour change.
- If a GCC build of a tree containing the pre-fix `insideCheckout` ever refuses a checkout save, then
  the miscompilation is not unconditional and the defect is narrower than stated here.
- If the three other fixtures that construct a `Game` as a member initialiser
  (`test_pad_slot1_policy`, `test_host_turn_guest_clock`, `test_host_turn_irq`) ever stop being safe,
  the claim that they are latent-but-currently-harmless is wrong. They install no runtime, so the global
  stays null and the null branch is taken; **they are fragile, not fixed**, and that is a known
  residual.

## NOT established

That no other Clang-tolerated UB exists in the framework. This issue found two instances of one class
in the course of one gate run; a sweep for the same class — pointers into dead storage, member-init
ordering against installed singletons — has not been done and is the obvious next step.

## Also in this change, and unrelated

The in-segment emulated-clock commitment (`runtime/cpu/segment_clock.*`, `lightrec_executor.*`) is a
separate root cause with its own rationale and is **not** part of this issue. It is what the gate run
that surfaced these two was verifying.
