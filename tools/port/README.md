# `psxport/tools/port/`: shared title tooling

Title-neutral engines that every title runs from its `external/psxport`. The data each reads stays in
the title repo; paths resolve against the working directory (the title root), never this file. An
engine refuses rather than passes when it reads nothing.

| engine | reads (per title) |
|---|---|
| `consumer_verify.py` | a title's `ConsumerVerifyConfig`: one Ninja configure, build, focused CTest and the execution-boundary checks; a test filter matching zero tests fails |
| `launch_environment.py` | the process environment at a launcher's product exec (`player_environment`, `agent_environment`) |
| `present_geometry.py` | a capture the caller names (PPM or PNG) |
| `widescreen_pair.py` | a 4:3 and a 16:9 capture of one frame; reuses `present_geometry.py`'s reader |
| `looks_right.py` | a built product it drives through the debug server |
| `fps60_check.py` | an fps60 dump capture |
| `override_differential_gate.py` | the JSON report written by `runtime/cpu/override_differential.h` |
| `title_link_isolation.py` | a multi-title product's per-title object lists, from `psxport_add_title_link_isolation_test` |

A title's verifier imports `port.consumer_verify` once `external/psxport` exists. The title gets that
link from psxport's `tools/psxport_fetch.py`, run from psxport `main` by its launcher and CI.
Maintainer runs use `CC=clang CXX=clang++`; the verifier keeps the caller's toolchain.

The RE-frontier tracker is re-harness `tools/re_frontier.py`.
