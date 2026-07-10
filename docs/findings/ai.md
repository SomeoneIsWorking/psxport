# Findings — AI / enemy behavior

## ActorMeleeEngage::doIt — scratchpad 0x1F80009C stale-write on the "arm-directly" path (RESOLVED, 2026-07-10)

- **symptom:** two independent frontier-convergence agents (2026-07-10) reproduced an SBS-full
  divergence around f1019 at scratchpad `0x1F80009C`, surfacing "inside `Trig::ratan2`
  (0x80085690, already-owned) with mismatched return addresses between core A/B" — one via
  `PSXPORT_DEBUG=ovhit PSXPORT_REPL=1 PSXPORT_SBS_AUTONAV=1` during a melee encounter, one via
  git-stash A/B on unmodified `main`. Both agents correctly flagged it as pre-existing and out of
  their pass's scope (see `docs/findings/render.md`'s "PutDrawEnv cluster" entry, "unrelated
  timing-sensitive finding"). The standard 95s `SBS_MODE=full SBS_AUTONAV=1` gate (no `ovhit`)
  does NOT hit it — it only manifests once autonav timing happens to walk into a melee fight,
  which the extra `ovhit` bookkeeping overhead was observed to perturb into reaching earlier.
- **root cause (line-by-line RE cross-check, `generated/ov_a00_shard_1.c:3527`,
  `ov_a00_gen_80112188`):** ground truth's approach-angle scratch store,
  `mem_w32((r20 + 156), r4)` (i.e. `mem_w32(0x1F80009C, angle)`), sits in the **delay slot** of
  the `bne` that branches on `margin < bandWidth` (gen line ~3603-3606):
  ```c
  { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[20] + (uint32_t)156), c->r[4]); if (_t) goto L_80112320; }
  ```
  Per MIPS delay-slot semantics the store executes UNCONDITIONALLY every time control reaches this
  branch — on BOTH the "reposition" path (`margin < bandWidth`, jumps to `L_80112320`) AND the
  "margin >= bandWidth" fallthrough that goes on to test `margin < 3` (armed-directly tail,
  `L_801124C0`, when that's also false). The native port (`game/ai/actor_melee_engage.cpp`,
  `ActorMeleeEngage::doIt`) had gated this write inside `if (margin < bandWidth)`, so on the
  "arm-directly" branch (`margin >= bandWidth && margin >= 3`) the write was silently dropped,
  leaving scratchpad `0x1F80009C` stale on core A (pc_faithful) while core B (gen/oracle) always
  refreshed it on every call that got past the XZ-distance and Y-band early-outs. Content-
  dependent: only bites when a melee AI actor's `doIt()` call takes that specific branch, matching
  the "only reproduces mid-fight, not on generic autonav" symptom. The "mismatched RA inside
  Trig::ratan2" framing from the original repro was a downstream artifact of the SBS write-watch
  attributing the (missing) write to whichever call frame happened to touch that scratch word next
  — not a bug in `Trig::ratan2` itself (independently line-traced against `gen_func_80085690`,
  `generated/shard_4.c:13206`, and found byte-faithful for the branches walked: x/y sign-strip,
  the `a0&0x7FE00000`/`a1&0x7FE00000` overflow-guard split, and the `a0/(a1>>10)` vs
  `(a0<<10)/a1` division selection all match).
- **fix:** moved the `c->mem_w32(0x1F80009Cu, (uint32_t)angle)` store to execute unconditionally
  right after computing `angle`/`margin`/`bandWidth` (matching the delay-slot's unconditional
  semantics), collapsing the old if/else into `const bool doReposition = (margin < bandWidth) ||
  (margin < 3);`. `game/ai/actor_melee_engage.cpp`.
- **sibling checked, no bug found:** `MeleeProximity::isAtApproachAnchor`
  (`game/ai/melee_proximity.cpp`, `gen_func_8001F9DC`, `generated/shard_2.c:795`) writes the same
  scratch word, but ground truth's store there (`generated/shard_2.c:848`) is a plain sequential
  instruction on the success path only (not a delay-slot write) — the native port already matches.
- **verification:**
  - Line-by-line RE cross-check of `gen_func_80112188` against the native port (this session) —
    confirmed the delay-slot bug above; no other discrepancies found on the paths traced (kindZBias
    polarity, dz/dx computation, Y-band test, reachHi/reachLo split, ratan2 argument order/rsin-
    rcos ordering all independently re-verified and matched the existing "BUG FIX" comments from
    the prior wide-RE pass).
  - Standard gate: `timeout 95 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1` —
    0-diff through f8550+, SIGINT-terminated by the watchdog, no crash (no regression from the fix).
  - Repro invocation: `PSXPORT_DEBUG=ovhit PSXPORT_REPL=1 PSXPORT_SBS_MODE=full
    PSXPORT_SBS_AUTONAV=1 PSXPORT_SBS_NOPAUSE=1` (150s) and the same + `PSXPORT_SBS_POSTDRIVE=1`
    (300s) — 0-diff through f11100+ / f12540+ respectively; the divergence did not reproduce in
    either window. **Caveat, stated honestly:** neither run's autonav actually reached a melee
    encounter — a standalone `PSXPORT_MIRROR_VERIFY=0x80112188,0x8001F9DC` pass (15000 frames,
    `PSXPORT_SBS_EXIT_FRAME=15000` for a clean atexit dump) confirms `0x80112188`
    (`ActorMeleeEngage::doIt`) and `0x8001F9DC` (`MeleeProximity::isAtApproachAnchor`) both show
    `NEVER HIT` under the current autonav+postdrive script on this route — the fixed-script
    "hold Right + periodic Cross-tap" autonav does not currently walk into an enemy on this level
    within the windows tested. The fix is verified by the RE cross-check + no-regression gates, NOT
    by re-observing the original divergence disappear live — a future session with autonav that
    actually reaches a melee fight (or a scripted pad-replay into one) should re-confirm 0-diff
    live. This is a genuine gap in the fleet's autonav tooling (it never exercises enemy combat),
    worth flagging for a future workflow improvement, not just this bug.
- **refs:** `game/ai/actor_melee_engage.cpp`, `game/ai/melee_proximity.cpp`,
  `generated/ov_a00_shard_1.c:3527`, `generated/shard_2.c:795`, `generated/shard_4.c:13206`,
  `docs/findings/render.md` ("PutDrawEnv cluster" entry, original repro note).
