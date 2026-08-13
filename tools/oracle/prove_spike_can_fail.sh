#!/usr/bin/env bash
# prove_spike_can_fail.sh — validate the INSTRUMENT, not the code under it.
#
# WHY THIS EXISTS. `oracle_spike` passing 18/18 tells you nothing on its own: a checker that has only ever
# seen the case it expects is not an instrument. The standing rule in this workspace is that a diagnostic is
# trusted only once it has shown the OTHER answer, and that the proof must live in the shipping artifact
# rather than in a session's memory of a `sed` someone once ran.
#
# WHAT IT DOES. For each MUTATION below, it takes the shipping `oracle_shim.c`, writes a THROWAWAY copy
# under scratch/ with exactly one thing broken, links the UNMODIFIED spike against it, and requires the
# spike to FAIL. The shipping file is never the thing broken: breaking the tree to prove it could break is
# the practice the USER rejected outright ("I'd rather verify that things work rather than break them and
# see they are broken"), so every mutation happens in a copy no build consumes.
#
# Each mutation targets a failure that would otherwise be SILENT — one where the oracle keeps running,
# keeps reporting a clean stop, and produces wrong answers:
#
#   fastmap   The FastMap population loop iterates zero times. cpu.c fetches opcodes directly out of
#             FastMap (cpu.c:794, 810), NOT through PSX_MemRead32, so an unpopulated map means the CPU
#             executes zeros, bus-errors to 0xBFC00180, and still reports a clean cycle-budget stop.
#             Measured 2026-08-13; it is what exposed a real weakness in the spike's own PC check, which
#             read a jump to the exception vector as "PC advanced past the entry".
#   clock     `oracle_slice` restarts the core's timestamp at 0 every slice instead of carrying it. A
#             bulk run is one slice so it is unaffected, but a single-step trace becomes a different
#             execution — which is the whole substrate of milestone 2's per-instruction differential. This
#             is the mutation the STEPPING checks exist to catch.
#   silent_hw The memory bus returns 0 for a device access instead of reporting it. This is the plausible
#             "graceful" version of the shim, and it is a lie: the NEGATIVE case would come back as a
#             clean window and a later compare would run over instructions nobody executed.
#
# EXIT: 0 = the instrument discriminates (real shim passes, EVERY mutation fails). 1 = it does not, and the
# results the spike has reported are suspect. 2 = the proof could not be run, which is NOT a pass.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-$REPO/build}"
OUT="$REPO/scratch/oracle"
SHIM="$REPO/tools/oracle/oracle_shim.c"

echo "prove_spike_can_fail: validating the oracle spike as an INSTRUMENT."
echo "  repo   $REPO"
echo "  build  $BUILD"

fail_setup() { echo "REFUSING (exit 2): $1"; echo "  Nothing was proven. This is not a pass."; exit 2; }

[ -f "$SHIM" ]  || fail_setup "no shipping shim at $SHIM"
[ -d "$BUILD" ] || fail_setup "no build directory at $BUILD — configure and build oracle_spike first"

SPIKE_OBJ="$(find "$BUILD/tools/oracle/CMakeFiles" -name 'oracle_spike.c.o' 2>/dev/null | head -1)"
[ -n "$SPIKE_OBJ" ] || fail_setup "no compiled oracle_spike.c.o under $BUILD — build the oracle_spike target first"

ORACLE_LIB="$(find "$BUILD" -name 'libpsxport_oracle.a' 2>/dev/null | head -1)"
[ -n "$ORACLE_LIB" ] || fail_setup "no libpsxport_oracle.a under $BUILD — build the psxport_oracle target first"

REAL_BIN="$(find "$BUILD" -name oracle_spike -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$REAL_BIN" ] || fail_setup "no built oracle_spike binary under $BUILD"

# Include/define flags come from the build's own compile_commands.json, so this proof cannot drift from how
# the shim is really compiled. A hardcoded -I list is how a proof ends up validating a different
# translation than the one that ships.
CC_JSON="$BUILD/compile_commands.json"
[ -f "$CC_JSON" ] || fail_setup "no compile_commands.json in $BUILD (CMAKE_EXPORT_COMPILE_COMMANDS is on in this repo — reconfigure)"

FLAGS="$(python3 - "$CC_JSON" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e["file"].endswith("tools/oracle/oracle_shim.c"):
        args = shlex.split(e["command"]) if "command" in e else e["arguments"]
        print(" ".join(a for a in args if a.startswith(("-I", "-D", "-f", "-O", "-std"))))
        sys.exit(0)
sys.exit(3)
PY
)" || fail_setup "compile_commands.json has no entry for tools/oracle/oracle_shim.c"

mkdir -p "$OUT"

# ── the real shim must pass, or nothing below means anything ──────────────────────────────────────────
"$REAL_BIN" >"$OUT/real.log" 2>&1; REAL_RC=$?
REAL_FAILS="$(grep -c '^  FAIL' "$OUT/real.log" || true)"
echo
echo "  REAL shim      exit $REAL_RC, $REAL_FAILS failed check(s)   ($OUT/real.log)"
if [ "$REAL_RC" -ne 0 ] || [ "$REAL_FAILS" -ne 0 ]; then
  echo "FAILED: the REAL shim does not pass, so the oracle itself is broken. Read $OUT/real.log."
  exit 1
fi

# ── each mutation: name, anchor, replacement, and which check family must catch it ────────────────────
mutate() {   # $1 name  $2 anchor  $3 replacement  $4 which checks should catch it
  local name="$1" anchor="$2" repl="$3" expect="$4"
  local variant="$OUT/variant_$name.c"

  ANCHOR="$anchor" REPL="$repl" python3 - "$SHIM" "$variant" <<'PY'
import os, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
anchor, repl = os.environ["ANCHOR"], os.environ["REPL"]
n = s.count(anchor)
if n != 1:
    # 0 = the anchor is stale, so the "mutation" would be a NO-OP and the proof would certify a shim that
    # was never broken. >1 = ambiguous. Either way this is a defect in this script, not a pass.
    print(f"  anchor matched {n} time(s), expected exactly 1: {anchor!r}", file=sys.stderr)
    sys.exit(1)
open(dst, "w").write(s.replace(anchor, repl, 1))
PY
  [ $? -eq 0 ] || fail_setup "mutation '$name': its anchor is stale, so it would have proven nothing. Re-read oracle_shim.c and fix the anchor."

  cc -c $FLAGS "$variant" -o "$OUT/variant_$name.o" || fail_setup "mutation '$name' did not compile"
  # The variant's shim is linked FIRST so its symbols win over the archive's copies.
  cc -o "$OUT/spike_$name" "$SPIKE_OBJ" "$OUT/variant_$name.o" "$ORACLE_LIB" -lm \
    || fail_setup "mutation '$name' did not link"

  "$OUT/spike_$name" >"$OUT/$name.log" 2>&1; local rc=$?
  local fails; fails="$(grep -c '^  FAIL' "$OUT/$name.log" || true)"
  printf "  BROKEN %-10s exit %d, %2s failed check(s)   (expected to break: %s)\n" "$name" "$rc" "$fails" "$expect"

  if [ "$rc" -eq 0 ] || [ "$fails" -eq 0 ]; then
    echo
    echo "FAILED: mutation '$name' PASSED the spike. The spike cannot detect this failure, so it is not an"
    echo "  instrument for it — $expect would silently report success. Read $OUT/$name.log."
    exit 1
  fi
  # Name which checks actually caught it, so a mutation that only trips UNRELATED checks is visible rather
  # than counted as covered.
  grep '^  FAIL' "$OUT/$name.log" | sed 's/^  FAIL /      caught by: /' | cut -c1-96
}

echo
mutate fastmap \
  'for (uint32_t ma = 0; ma < 0x00800000u; ma += RAM_SIZE) {' \
  'for (uint32_t ma = 0; ma < 0x00000000u; ma += RAM_SIZE) {' \
  'the positive and stepping checks'

mutate clock \
  '  cpu_next_event_ts = s_ts + budget;
  s_ts              = CPU_Run(PSX_CPU, s_ts);' \
  '  cpu_next_event_ts = budget;
  s_ts              = CPU_Run(PSX_CPU, 0);' \
  'the stepping checks'

mutate silent_hw \
  '  hw_access(A, 0, 32);
  return 0;
}
// 24-bit access serves the unaligned-load path' \
  '  return 0;
}
// 24-bit access serves the unaligned-load path' \
  'the negative checks'

echo
echo "PROVEN: the spike passes on the real shim and FAILS on every mutation above — it has shown both"
echo "  answers for each failure mode, so a pass from it means something. This validates the INSTRUMENT"
echo "  only; it says nothing about psxport's own paths."
exit 0
