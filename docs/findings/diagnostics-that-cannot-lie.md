# A diagnostic that cannot lie: five ways the instruments were wrong, and the rule they add

**WHY THIS DOCUMENT.** Measured across one session, 2026-09-27, working on four different titles. Five
separate defects, all the same shape: **an instrument returned an answer that looked like a measurement
and was not one.** None of them crashed, none of them printed an error, and three of them produced
numbers with real denominators attached. Two of the five had already been used to support a published
conclusion before they were found.

This is written down because the individual fixes live in commit messages, and a commit message is not
consulted by the next person reaching for the instrument. The rule below is the durable part.

## The rule

**An instrument's answer is only evidence if the instrument can be shown to produce the OTHER answer for
a case where the other answer is correct.** Concretely, three obligations, each of which caught a real
defect here:

1. **A negative case.** If the instrument reports a count, a match, or a difference, there must be a
   test in which the correct answer is zero/none/absent — and that test must be shown to FAIL when the
   matching is broken. An instrument with only positive cases cannot distinguish "measured nothing" from
   "cannot measure".
2. **A stated denominator, attached to the answer rather than to the run.** A count is only meaningful
   against what it was counted over, and the two must not be separable by the reader.
3. **The report must name what was actually compared.** If the words do not distinguish "this address was
   not written" from "no instruction at this address executed", a reader will pick the convenient one,
   and the wrong one will be published.

## The five instances

### 1. An armed instrument that never reported (Spyro 1, `PSXPORT_STORE_OBSERVE`)

`store_observe_report` was called from exactly one place — immediately after `native_boot`'s frame loop
returned. Spyro 1 does not return through that function: its run printed `Lightrec fallback telemetry
[shutdown]`, which comes from `~LightrecExecutor`, and never printed `frame loop done` at all.

The result, through two different drivers and a clean `exit 0`:

    [store-observe] watching 3 guest address(es) for stores:
    [store-observe]   [0] 0x800700F4

...and nothing. **An armed instrument that reports nothing is worse than an unarmed one**, because the log
has already said it is watching; the silence reads as "matched none of the stores" when the truth is
"never looked".

*Rule added:* report from the teardown every exit path reaches — `~LightrecExecutor`, beside the fallback
telemetry — and **delete** the single call site rather than leaving it to double-report on the paths that
do return, because a denominator printed twice stops being one.

### 2. A tautology published as a measurement, with a denominator (Spyro 1, same instrument)

The matcher is `target.guestPc != guestPc`: the armed list is **store instruction PCs**. Every line the
instrument printed said "address", and its header claimed it answers "which instruction wrote this guest
word". It cannot — the callback receives the store's PC and the register file, not its target address.

Acting on that text, `PSXPORT_STORE_OBSERVE=800700F4,800700F8,800700FC` was armed with **data
addresses** while investigating why the moby list at `0x800700F4` was never filled, and reported:

    report: armed=yes targets=3 jit_instructions=116056872
      [0] 0x800700F4 stores before=0 after=0 — MATCHED NONE of the 116056872 executed JIT instruction(s)

`0x800700F4` is not an instruction, so no store instruction can ever be AT it. **MATCHED NONE was
guaranteed before the game started.** The 116,056,872 denominator is what makes this the most dangerous
entry here: a real denominator attached to a tautology reads as a strong measurement, and it was
published as "the product never writes that word" in an issue and two commit messages.

*Rule added:* every line names what was compared — `no STORE INSTRUCTION at this PC executed … This says
NOTHING about any guest DATA address` — and a negative case now arms the guest word a store writes
(`0x40`) and asserts it sees nothing **while asserting the write happened**, because either half alone
misleads.

### 3. A short answer indistinguishable from a complete one (both control surfaces, `rw`)

`Repl` and `DbgServer` each carried their own literal `64` for words-per-line, each clamped the request,
and each returned the clamped answer silently.

Reached in real use: `tools/probe_moby_list.py` asked for 1408 words, got 64, and then **hung** waiting
for the rest. A less careful caller reads the 64 words it gets and treats the other 1344 as zeros — and
for that probe "zeros" is exactly the answer it exists to look for.

*Rule added:* the cap lives in one header (`control_surface_limits.h`, because two transports with two
literals is two places to drift unnoticed); the data line's format is unchanged so existing parsers keep
working; a short answer is announced **on its own line**, naming served-vs-asked and stating the missing
words are not zero. Mutation-verified: removing the notice fails the test.

### 4. A knob read with `cfg_str` and declared nowhere (`PSXPORT_RAMDUMP`)

`PSXPORT_RAMDUMP` and `PSXPORT_RAMDUMP_FRAME` were read but had no declaration, so no help text and no
env-audit description. Setting `PSXPORT_RAMDUMP=path` produced no file, and "the RAM dump does not work"
was the conclusion — wrong: it writes after the frame loop returns, a path Spyro 1 does not take, while
`PSXPORT_RAMDUMP_FRAME=N` writes from inside the loop and works.

*Rule added:* declare it once in `config.cpp`, declare it in `config_vars.h`, say in the help text which
of two similar knobs to reach for, and log an error when `fopen` fails instead of writing no file and no
message. The same "undeclared means unaskable" defect as instance 1, in a different guise.

### 5. A warning that said "this is not what you set" nowhere (two ports, the RAMDUMP paths)

`PSXPORT_RAMDUMP` (end-of-run) and `PSXPORT_RAMDUMP_FRAME` (mid-run) are different tools with
near-identical names, and the failure mode of choosing wrong is silence. Both help texts and both
`fopen` failure paths now name the alternative.

### 3b. The consumer side, and the reference pattern for fixing it

A cap is only half a contract; the CLIENT has to survive it. Two shapes exist in this workspace today:

- **Wrong (Spyro 1, `tools/drive.py`):** send `rw ADDR COUNT` once, then wait for a line whose word count
  equals COUNT. Over the cap, the port returns 64, the wait never matches, and the reader falls out of its
  loop reporting **"the port exited before answering rw"** — which is false. The port answered; it
  answered partially, and with the framework fix above it also said so on a line the regex ignored. A
  reader that turns a short answer into "the port died" sends the next person to debug the product.
- **Right (Spider-Man 1, `tools/probe_wide_geometry.py`):** loop, re-requesting from
  `address + 4 * len(out)` until the requested count is assembled. The cap becomes an iteration count
  rather than a ceiling, and a genuinely refused read still surfaces as a refusal with the reply text.

The second shape is the pattern to copy, and it costs nothing: a client that loops until it has what it
asked for cannot be confused by a cap, because it never assumes one reply was sufficient.

### 6. A gate hole that is real — in seven of the ten copies of the gate

`psxport_resolved.txt` is written by CMake at CONFIGURE time, so a plain `cmake --build` never refreshes it.
A pin check that reads only that file compares a STALE SNAPSHOT to the pin, and a tree rebuilt against
newer framework code still reports the old commit, matches its pin, and passes. A fresh clone would then
build a different framework than the one just tested — the single failure the pin exists to prevent.

**I got this wrong twice, in opposite directions, and both errors are worth keeping.**

*First attempt:* I saw that `psxport_resolved.txt` is configure-time-only, concluded the check "would pass",
and published that in two documents.

*Retraction:* I read `crash`'s copy of the tool, found it compares the resolved snapshot against the
framework's CURRENT head, and concluded the hole was not real at all — the whole finding reduced to "`crash`
registers no pin test". **That was wrong, and wrong in the mirror image of the first error:** I verified the
guard on a copy that HAS it and generalised to all ten.

*The experiment that settles it.* `tools/psxport_sync.py` is copied into every port so each builds from a
bare clone, and the copies have DIVERGED. With `psxport_resolved.txt` naming a repo's own recorded pin while
the shared framework sat eight commits later, same input, two answers:

    crash       (guarded):   check FAILED — framework .../psxport is dirty or changed since configure
                              (configured 436c3762, current ba48b103)
    crashbash   (unguarded): check OK — built against e0485d33, which is the recorded pin

**So the hole is real, and it is a divergence artefact: the guard was present in 3 of 10 copies and absent
from 7.** A second, quieter half sat in the same function — an absent receipt printed "Asserting nothing"
and returned 0, so an unconfigured tree PASSED a provenance check that had asserted nothing. Five of the
seven did that.

**Now fixed in 7 of 10** (`crashbash`, `spider1`, `megamanx4`, `tekken3`, `toystory2`, `vagrant`,
`Tomba2Engine`), each verified by re-running the experiment above against that repo's own copy. The eighth,
`spyro`, is the same one-hunk change and is deliberately NOT applied: that tree is being worked by another
agent. `crash` and `ctr` already had it.

*Rules this earns, which are the same rule twice:*
- **A passing check on one copy is not evidence about the other nine.** Verify the claim against the copy
  that is SUSPECTED, not the one that is known-good.
- **Absence of evidence is not a pass.** A check with nothing to compare must exit non-zero; "asserting
  nothing" must never be spelled `return 0`.
- **Duplication is only free when the copies cannot drift.** Ten copies of a safety gate is ten gates. The
  duplication is a deliberate consequence of "a port must build from a bare clone", so the obligation moves
  to keeping them in step — and the check for that is itself duplicated, which is the next thing to fix.

## What this costs, and why it is worth a document

Five of these were found by **doing the measurement wrong and then being suspicious of the result**, which
is not a process anyone can be asked to follow reliably. The sixth was found the other way round: by
suspecting a gate, writing the suspicion down, and only then reading the gate's code — which showed the
suspicion was wrong. **Being suspicious is not the durable part; testing the suspicion is.** The durable
form is the rule above plus the tests that now encode it, so neither the luck nor the scepticism is
required: `test_control_read_limits` fails if a short answer is silent, `test_dynarec_contract` fails if a
data address matches a store PC, and `test_wide_left_margin` fails if the two spellings of the margin are
ever allowed to drift apart.

The general statement: **a diagnostic is not finished when it works, and it is not safe until it has been
observed to fail correctly.** An instrument that has only ever produced the answer you wanted is
indistinguishable from one that cannot produce the other.

## MEASURED 2026-10-01 — a stale compiler-cache entry silently changed what nine source-reading tests
were measuring

**The symptom.** Nine `psxport` tests failed in a worktree and passed in the shared checkout:
`test_audio_policy`, `test_fmv_watchdog`, `test_frame_loop_shell`,
`test_guest_program_image_ownership`, `test_guest_vram_policy_ownership`,
`test_no_game_address_literals`, `test_render_path_cycle`, `test_rml_text_encoding`, `test_watchdog`.
They are the source-reading and ownership tests: they resolve a file relative to `__FILE__` and read
it. The first plausible explanation was a working-directory problem, and it was wrong — `ctest` does
run them from `build/tests`, but the shared checkout's binary passes from that same directory.

**The real cause, one command's worth of evidence.** Those nine binaries were the only ones carrying
a RELATIVE `__FILE__` (`strings build/tests/test_fmv_watchdog` → `../tests/test_fmv_watchdog.cpp`),
so a path built from it resolved under `build/`, where no source lives. The shared checkout's binary
carries the ABSOLUTE path and passes. Nothing in the worktree's *current* build command was wrong:
`ninja -t commands` shows the absolute source path on the command line right now, and deleting the
object and rebuilding reproduces the relative `__FILE__` again. The variable that changes the answer
is `CCACHE_DISABLE=1`, which turns the stale object into a correct one on the first try. So a
**ccache entry produced by an earlier configure that used a relative `-S`** was being returned for a
current configure that uses an absolute one, and the cache key does not separate them.

**Why it survived a full rebuild, which is the part worth remembering.** A stale object is the
ordinary, boring failure, and the ordinary repair is to touch the sources and rebuild. That was
tried, all nine files touched, `cmake --build` recompiled them, and the relative `__FILE__` came
straight back. The evidence that the compile was not the problem is that the same command under
`CCACHE_DISABLE=1` produced the correct object — so "it recompiled" proved nothing, and a gate that
rebuilds cleanly can still be measuring a cached answer.

**The rule this earns, and it is the same one as `__FILE__` deserves:** a test that reads the SOURCE
TREE resolves that tree through the compiler's own record of where it was compiled from, so the
question "does this test see the right files" has a second answer outside the source tree, in the
build cache. `strings <binary> | grep <own source name>` is the one-line discriminator, and it
distinguishes the two cases that look identical from the outside — a test that is looking in the wrong
directory, and a test that is looking at a path the compiler was given a long time ago.
