# OPEN DEFECT — `psxport/scripts/sync-submodules.sh` reports "in sync" about submodules it never saw

Diagnosed 2026-08-05, **not fixed** (a fix was written and the file was subsequently reverted).
Recorded here so the next session does not re-derive it.

## Symptom

The script prints:

    [submodules] 1 submodule(s) checked, all at this repo's recorded gitlinks

while `git submodule status` shows `+` on `vendor/lucent` — i.e. the checkout genuinely differs from
the recorded gitlink. It bit three builds in one day: `ot_attr.h` uses `lucent::Channel`, which needs
vendored lucent ≥ `07c5836`, and the stale checkout at `02ea34b` failed to compile every time.

## Cause

`git submodule status --recursive` **aborts partway** in this repo:

    fatal: no submodule mapping found in .gitmodules for path 'deps/lightning/gnulib'
    fatal: failed to recurse into submodule 'vendor/beetle-psx'
     904cc7d7... vendor/beetle-psx

beetle-psx registers a nested `deps/lightning/gnulib` with no URL, git gives up there, and **never
reaches `vendor/lucent`**. The script's `|| true` swallows the non-zero exit, so `status_now` holds a
single line with no `+`, and a PARTIAL ENUMERATION is reported as a clean bill of health.

This is the project's recurring failure class — a check that structurally cannot see the answer,
returning a confident negative — living inside the script written to prevent exactly that.

## The fix that was written (re-apply if wanted)

1. **Capture the exit status.** The script runs under `set -eu`, so
   `status_now="$(cmd)"; status_rc=$?` EXITS on the failing substitution and never reads `$?`. An
   assignment used as an `if` condition is exempt from errexit:

       status_rc=0
       if ! status_now="$(git submodule status --recursive 2>/dev/null)"; then status_rc=1; fi

2. **Cross-check the count against `.gitmodules`**, so a short enumeration is detectable:

       n_declared="$(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | grep -c .)"

3. **Fall back by walking the DECLARED paths directly.** `git submodule foreach --recursive` is not a
   usable fallback — it trips on the same unmapped nested path and reported 1 of 2. Read each gitlink
   with `git ls-tree HEAD <path>` and compare to `git -C <path> rev-parse HEAD`.

4. **Never claim sync over an incomplete enumeration.** When `n_listed < n_declared`, say so:

       N of M submodule(s) checked — CANNOT SEE the rest; not claiming they are in sync

   The intermediate version that only got as far as (1)+(2) already turned the lie into an honest
   refusal, which is the bulk of the value.

## Workaround until fixed

After ANY rebase or gitlink change, before building:

    git submodule update --init --recursive

## STATUS 2026-08-06: a fix EXISTS but is NOT LANDED

Written in `spyro/external/psxport` (the clean tree) under claim `coord/claims/sync-submodules/`.
It is a patch, not a commit — **all four trees still carry the defective script** (md5
`b9beccd28a15419773707e92c1b25587`) until the operator lands it.

    coord/patches/sync-submodules.diff
    coord/patches/sync-submodules.NEWFILE.tests_test_sync_submodules.cpp  -> psxport/tests/

Shape: the script no longer *relies* on `git submodule status --recursive` (it is kept only as a
cross-check in the other direction — anything IT lists that the walk missed is also a refusal). It walks the DECLARED
set itself out of each level's `.gitmodules` and compares the superproject index gitlink against the
checkout's HEAD, so an undeclared nested gitlink cannot truncate it. Anything it still cannot resolve
is named and the script REFUSES (`checked N of M … CANNOT SEE: <paths>`, exit non-zero).

Two things this write-up got incomplete, found while fixing it:

- **`git submodule foreach --recursive` truncates identically**, so the DIRT GUARD was blind to
  vendor/lucent too — "never clobber local work" did not cover it. Measured: 3 of 4 paths in spyro.
- **`git submodule update --recursive` does NOT truncate.** Measured on a fixture: it reached and
  checked out the nested path that `status --recursive` never listed, rc=0. Only the two enumerating
  commands abort. So the sync half was always able to do its job; only the reporting half lied.

Verified RED->GREEN by `ctest -R test_sync_submodules` (1/5 -> 5/5) plus real-tree A/B: on
Tomba2Engine at the same moment, the old script said "2 submodule(s) checked" and the new one says
"checked 3 of 3".
