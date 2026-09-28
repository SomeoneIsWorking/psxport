# 0130 — three TITLE-SPECIFIC guest globals are read by bare address from framework code

**State:** recorded, not fixed. Found 2026-09-28 while naming the guest operations in
`runtime/psx/gpu_native.cpp`; the same work tried to fix it and was stopped by the gate that exists
specifically to stop it (below).

## The addresses

| address | read by | what the framework claims to know |
|---|---|---|
| `0x801FE00C` | `gpu_native.cpp` (`redpkt`, `[stagetl]`), `mem.cpp` (2 sites), `dbg_server.cpp` | the entry address of guest task 0. No framework consumer interprets it; every one only tests it for non-zero. The STRUCT, the FIELD, and what a zero means are all unattributed. |
| `0x801FE048` | `dbg_server.cpp` (`debug stage`) | nothing at all. It is printed as `sm48` and never read as a condition. |
| `0x800BE258` | `dbg_server.cpp` (`debug stage`) | a non-zero value means a scene is loaded. Width unknown: read as a 32-bit word and only tested for non-zero, so a one-bit flag and a counter are indistinguishable. |
| `0x800BF544` | `gpu_native.cpp` (`pool` channel) | the field overlay's packet-pool write cursor. **Its units are disputed:** the two tests that seed it (`test_zero_config_is_loud`, `test_render_noise_mask`) set it to the pool BASE, which is a byte offset, so it cannot be read as "how many packets were allocated". |

## Why it is a defect

`AGENTS.md` puts a title's address in the title's repository, and `tests/test_no_game_address_literals.py`
enforces that with a baseline of 95 known offenders. These are four of them. On any title other than the
one they came from, each read returns that title's unrelated RAM and every line built on it is
confidently wrong — a diagnostic that reports a value it did not measure.

## Why the obvious fix was rejected here

The attempted fix in this work was to name them in a framework header
(`runtime/psx/guest_scene_globals.h`), with each one's known-and-unknown stated in its comment. That is
the right NAME and the wrong PLACE, and the gate said so:

```
NEW game address in framework code: runtime/psx/guest_scene_globals.h 0x800bf544 x1 (baseline 0)
  -> move it into GameConfig (runtime/psx/game_iface.h) and read it from there;
```

Naming a title address in a framework file does not make it title-neutral; it makes it a named title
address, which is strictly worse than a hex literal because the name reads as an owned concept. The
header was deleted and the comments were reduced to the call sites, where a reader sees a hex literal and
a note that it is a title global.

The real fix is `GameConfig`, and that is a consumer migration: four fields, ten title repositories, and
an honest-zero shape for a title that declares none (the pattern `ot_attr.cpp`'s `pool_range()` already
uses). It also raises the question the `0x800BF544` row already exposes — what a title that declares
nothing should see. That is a design decision, not a refactor.

## Where

- `runtime/psx/gpu_native.cpp` — `0x801FE00C` (redpkt, `[stagetl]`), `0x800BF544` (pool channel).
- `runtime/psx/mem.cpp` — `0x801FE00C` (wwatch, spudma).
- `runtime/psx/dbg_server.cpp` — `0x801FE00C`, `0x801FE048`, `0x800BE258` (`debug stage`).
- `tests/test_no_game_address_literals.cpp` — the baseline rows that keep these visible.
