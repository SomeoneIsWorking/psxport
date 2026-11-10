# 0198 — Restructure leftovers

Status: open. Owner: `config/` (registry), `cpu/gte_beetle.cpp` (GTE globals), `.clang-tidy`.

The October restructure plan is finished except these items; the plan itself is retired.

- `bugprone-easily-swappable-parameters` findings are not fixed.
- The config registry is accessors over the CVar ladder, not one immutable `Config` reached through `Game`;
  hot paths cache knobs in function-local statics.
- `ProjParams::sCurrent` and Beetle's `GTE_BindState`/`GTE_CurState` are process globals.
