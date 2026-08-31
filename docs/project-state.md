# Project state

## Current focus

S004 is the current focus.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | MIPS R3000A executables are translated into deterministic generated C | verified | — | G001 |
| S002 | Native runtime owners provide GPU, SPU, GTE, MDEC, CD, XA/FMV, and BIOS services | partial | S001 | G001 |
| S003 | Shipping differential infrastructure compares independent guest-state snapshots and frame boundaries | verified | S001 | G001 |
| S004 | Game-owned native producers replace guest graphics while supporting widescreen and interpolation | partial | S002 | G001 |
| S005 | Multiple title repositories consume the framework through narrow title-owned seams | verified | S001, S002 | G001 |

## Capability details

### S001 — Static translation

Evidence: the shared build path emits C shards from recovered executable functions and is consumed by
multiple maintained PlayStation port repositories.

### S002 — Console service replacement

The framework has production GPU, SPU, GTE, MDEC, CD, XA/FMV, BIOS/SDK, and CHD-backed service owners.

Gap: service and title coverage remains incremental; unsupported semantics must still be grounded and
implemented as new titles reach them.

### S003 — Differential verification

Evidence: the shipping `ndiff_run` path retains independent complete snapshots and focused tests cover
nested snapshots, modeled returns, frame contracts, and negative controls.

### S004 — Native presentation

The framework exposes native scene producers, depth-aware rendering, interpolation, and title-owned
picture policy used by live ports.

Gap: each title still needs complete game-state producers; guest post-projection packets are not an
acceptable fallback.

### S005 — Multi-title consumption

Evidence: Tomba, Crash, Crash Bash, Mega Man X4, Spider-Man, Spyro, Tekken 3, Toy Story 2, and Vagrant
Story repositories consume the same framework while retaining title-specific policy.
