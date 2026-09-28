---
id: 0136
title: 134 area claims had no falsifier and were blocking the locks they were meant to coordinate
status: closed
symptom: `coord/claims/` held 142 directories, **136 of them still carrying an active-looking
  `claim.md`**, and only **9 of any** recorded an expiry or a falsifier. `PROTOCOL.md` requires one:
  "A claim needs a falsifier (`--expires-on`); when one falls, fix what relied on it."
tags: protocol,claims,hygiene,coordination
created: 2026-09-28
updated: 2026-09-28
---

## What this cost, concretely, and it was not hypothetical

The product slot is a single shared resource, so a stale claim on it blocks **every** live run for
every title. That is not abstract: on 2026-09-28 the slot's `claim.md` had been written at 10:25 and
nothing had rewritten it in 44 minutes, no `spyro_port`/`drive.py`/`demo_run.py` process existed, the
holder's tree was clean, and it still had to be taken **by force** to run the one capture that
verifies the pool-water fix. The claim carried no expiry, so nothing in the protocol declared it
dead.

This is the same defect class as the instruments in this workspace, and it is worth naming the
similarity precisely: **an absent answer reads as a negative answer.** A census that never ran prints
zero; `depth_cov.py` reported "0 primitives carried depth" for a run whose counters were never
reached; a lock that outlives its holder reads as a live holder.

## The census, with its denominator

| measure | value |
|---|---|
| claim directories under `coord/claims/` | **142** |
| still carrying an active-looking `claim.md` | **136** |
| recording an expiry or a falsifier | **9 of any** |
| newest `claim.md` at the time of the sweep | **7 hours** old |
| second newest | 13 hours old |
| everything else | **9 days or older** |

No claim had been taken in 7 hours except the operator's own, which was released the same session.
**The 9-day cluster is abandoned work, not work in progress.**

## What was done, and what was deliberately NOT done

**Released 134 claims older than 24 hours**, each preserved as
`claim.RELEASED-<YYYYMMDD>.md` **inside its own directory**, so the content and the area name both
survive and a returning agent can read what was claimed and why. **Nothing was deleted**, so this is
reversible by renaming back.

**Two claims were KEPT and left exactly as they are:** `crashbash-boot-handoff` (13 h) and
`spider1-black-frame-product` (7 h). Both are recent enough that an agent may genuinely still be in
them, and **a claim is cheap to hold and expensive to steal** — the asymmetry argues for keeping them.

**No product or framework file was touched**, and no repository was edited. `coord/` is untracked and
machine-local by design.

## The rule this exposes, which is the durable part

`mkdir` is atomic, so the lock itself is sound — the failure is entirely in **liveness**. A lock with
no expiry is not a lock; it is a wall with no door. Two changes make the mechanism work as intended:

1. **Every claim records `--expires-on`.** `PROTOCOL.md` already says so and it is not being followed
   in 127 of 136 cases.
2. **A claim whose holder has not touched its file or its tree within the expiry is stale by
   definition**, and the recovery is to take the slot and leave a note naming who has it and why —
   which is what was done for the product slot, and is the same action recorded in
   `coord/claims/product-slot/claim.operator-pool-verification.RELEASED.md`.

**Falsifier for this issue:** a future session finding that a released claim's area was in fact being
worked on, which would mean 24 hours was too short an expiry for that agent and the threshold should
rise. That is the observation that would move the number, and it is cheap to make.
