#!/usr/bin/env bash
# prove_spike_can_fail.sh — validate the INSTRUMENT, not the code under it.
#
# WHY THIS EXISTS. `oracle_spike` passing 12/12 tells you nothing on its own: a checker that has only ever
# seen the case it expects is not an instrument. The standing rule in this workspace is that a diagnostic
# is trusted only once it has shown the OTHER answer, and that the proof must live in the shipping
# artifact rather than in a session's memory of a `sed` someone once ran.
#
# WHAT IT DOES. Takes the shipping `oracle_shim.c`, makes a THROWAWAY copy under scratch/ with one specific
# thing broken — the FastMap population loop, which is how cpu.c fetches opcodes (cpu.c:794, 810) — links
# the UNMODIFIED spike against it, and asserts the spike FAILS. The shipping file is never the thing
# broken: breaking the tree to prove it could break is the practice the USER rejected outright
# ("I'd rather verify that things work rather than break them and see they are broken"), so the mutation
# happens in a copy that no build consumes.
#
# WHY THE FASTMAP IS THE RIGHT THING TO BREAK. It is the failure mode that would otherwise be SILENT: with
# an unpopulated FastMap the CPU fetches from `DummyPage`, executes zeros, takes a bus error and lands at
# 0xBFC00180. It still runs. It still reports a clean cycle-budget stop. Every "did it work" check that
# looks only at liveness passes. Measured 2026-08-13, and it is what exposed a real weakness in the
# spike's own PC check, which read a jump to the exception vector as "PC advanced past the entry".
#
# EXIT: 0 = the instrument discriminates (real shim passes, broken variant fails). 1 = it does NOT, and
# every result the spike has ever reported is now suspect. 2 = the proof could not be run at all, which is
# NOT a pass.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${1:-$REPO/build}"
OUT="$REPO/scratch/oracle"
SHIM="$REPO/tools/oracle/oracle_shim.c"
SPIKE_OBJ_DIR="$BUILD/tools/oracle/CMakeFiles"

echo "prove_spike_can_fail: validating the oracle spike as an INSTRUMENT."
echo "  repo   $REPO"
echo "  build  $BUILD"

# ── refuse rather than return empty: every input this proof needs must be present and named ──────────
fail_setup() { echo "REFUSING (exit 2): $1"; echo "  Nothing was proven. This is not a pass."; exit 2; }

[ -f "$SHIM" ] || fail_setup "no shipping shim at $SHIM"
[ -d "$BUILD" ] || fail_setup "no build directory at $BUILD — configure and build oracle_spike first"

SPIKE_OBJ="$(find "$SPIKE_OBJ_DIR" -name 'oracle_spike.c.o' 2>/dev/null | head -1)"
[ -n "$SPIKE_OBJ" ] || fail_setup "no compiled oracle_spike.c.o under $SPIKE_OBJ_DIR — build the oracle_spike target first"

ORACLE_LIB="$(find "$BUILD" -name 'libpsxport_oracle.a' 2>/dev/null | head -1)"
[ -n "$ORACLE_LIB" ] || fail_setup "no libpsxport_oracle.a under $BUILD — build the psxport_oracle target first"

# The include flags come from the build's own compile_commands.json, so this proof cannot drift from how
# the shim is really compiled. Hardcoding a -I list here is how a proof ends up validating a different
# translation than the one that ships.
CC_JSON="$BUILD/compile_commands.json"
[ -f "$CC_JSON" ] || fail_setup "no compile_commands.json in $BUILD (CMAKE_EXPORT_COMPILE_COMMANDS is on in this repo — reconfigure)"

FLAGS="$(python3 - "$CC_JSON" <<'PY'
import json, shlex, sys
db = json.load(open(sys.argv[1]))
for e in db:
    if e["file"].endswith("tools/oracle/oracle_shim.c"):
        args = shlex.split(e["command"]) if "command" in e else e["arguments"]
        keep = [a for a in args if a.startswith(("-I", "-D", "-f", "-O", "-std"))]
        print(" ".join(keep))
        sys.exit(0)
sys.exit(3)
PY
)" || fail_setup "compile_commands.json has no entry for tools/oracle/oracle_shim.c"

mkdir -p "$OUT"
VARIANT="$OUT/variant_nofastmap.c"

# ── the mutation, in a copy, asserted to have actually applied ───────────────────────────────────────
python3 - "$SHIM" "$VARIANT" <<'PY' || fail_setup "the FastMap loop was not found in the shim — this script's anchor is stale, so it would have 'proven' a no-op. Re-read oracle_init() and fix the anchor."
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
anchor = "for (uint32_t ma = 0; ma < 0x00800000u; ma += RAM_SIZE) {"
if s.count(anchor) != 1:
    sys.exit(1)                       # 0 = anchor stale (a no-op mutation proves nothing); >1 = ambiguous
open(dst, "w").write(s.replace(anchor, "for (uint32_t ma = 0; ma < 0x00000000u; ma += RAM_SIZE) {", 1))
PY
echo "  mutated a COPY at $VARIANT: FastMap population loop iterates zero times."

cc -c $FLAGS "$VARIANT" -o "$OUT/variant.o" || fail_setup "the mutated copy did not compile"

# Link the variant's shim FIRST so it wins over the archive's copy of the same symbols.
cc -o "$OUT/variant_spike" "$SPIKE_OBJ" "$OUT/variant.o" "$ORACLE_LIB" -lm \
  || fail_setup "the variant did not link"

# ── run both classes and require them to DISAGREE ────────────────────────────────────────────────────
REAL_BIN="$(find "$BUILD" -name oracle_spike -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$REAL_BIN" ] || fail_setup "no built oracle_spike binary under $BUILD"

"$REAL_BIN" >"$OUT/real.log" 2>&1;    REAL_RC=$?
"$OUT/variant_spike" >"$OUT/variant.log" 2>&1; VAR_RC=$?

REAL_FAILS="$(grep -c '^  FAIL' "$OUT/real.log"    || true)"
VAR_FAILS="$(grep -c  '^  FAIL' "$OUT/variant.log" || true)"

echo
echo "  REAL shim      exit $REAL_RC, $REAL_FAILS failed checks   ($OUT/real.log)"
echo "  BROKEN variant exit $VAR_RC, $VAR_FAILS failed checks   ($OUT/variant.log)"
echo

if [ "$REAL_RC" -ne 0 ] || [ "$REAL_FAILS" -ne 0 ]; then
  echo "FAILED: the REAL shim does not pass, so the oracle itself is broken. Read $OUT/real.log."
  exit 1
fi
if [ "$VAR_RC" -eq 0 ] || [ "$VAR_FAILS" -eq 0 ]; then
  echo "FAILED: the BROKEN variant PASSED. The spike cannot detect a CPU that fetches nothing, so it is"
  echo "  not an instrument and every 12/12 it has ever reported is worthless. Read $OUT/variant.log."
  exit 1
fi

echo "PROVEN: the spike passes on the real shim ($REAL_FAILS failures) and fails on a shim whose FastMap"
echo "  is unpopulated ($VAR_FAILS failures) — it has now shown BOTH answers, so a pass from it means"
echo "  something. This validates the instrument only; it says nothing about psxport's own paths."
exit 0
