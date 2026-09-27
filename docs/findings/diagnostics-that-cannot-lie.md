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

## What this costs, and why it is worth a document

Each of these was found by **doing the measurement wrong and then being suspicious of the result** —
which is not a process anyone can be asked to follow reliably. The durable form is the rule above plus
the tests that now encode it, so the suspicion is not required: `test_control_read_limits` fails if a
short answer is silent, and `test_dynarec_contract` fails if a data address matches a store PC.

The general statement: **a diagnostic is not finished when it works, and it is not safe until it has been
observed to fail correctly.** An instrument that has only ever produced the answer you wanted is
indistinguishable from one that cannot produce the other.
