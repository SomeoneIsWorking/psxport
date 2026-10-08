# The Linux setup action duplicated the Lightrec pin and drifted behind the framework

- status: resolved
- discovered: 2026-09-14
- resolved: 2026-09-14

## Cause

`cmake/lightrec_dependency.cmake` is the single authority for the maintained Lightrec revision, but
`.github/actions/setup-linux/action.yml` carried its own hardcoded `ref:` literal. `51df140f` bumped
the framework pin to `9a982a6475884f7059edb74a73d9a22c0060f18f` (selected-store observer
prerequisite) without — and could not mechanically see — the action's copy. Consumers building
against older pins kept passing; the mismatch only surfaced at CMake's exact-revision check the
first time a consumer pinned a post-`51df140f` framework (measured: C-12 hosted run 34896800544
failing `expected 9a982a64..., found b1457137...`). Local builds never see it because the resolver
prefers the shared sibling checkout.

## Fix

`setup.py` gained `framework_revisions()`, parsing the one `set(PSXPORT_LIGHTREC_REVISION ...)`
declaration from the framework's own cmake file (absent or doubled declarations refuse), exposed as
`--print-revisions` and consumed as the checkout step's `ref` via a step output. The action now has
no revision literal of its own.

## Not covered

The action's GNU Lightning `ref:` has no cmake-side counterpart yet; it remains the action's own
single source. Spider-Man 1's workflow inlines its own Lightrec checkout literal (old revision)
rather than consuming this action and will fail its own pin bump the same way; that consumer fix
belongs to its next touched change.
