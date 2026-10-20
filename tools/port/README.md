# `psxport/tools/port/`: shared title tooling

Title-neutral engines that every title runs from its `external/psxport`. The data each reads stays in
the title repo; paths resolve against the working directory (the title root), never this file. An
engine refuses rather than passes when it reads nothing.

| engine | reads (per title) |
|---|---|
| `consumer_build.py` | a title root: one Ninja configure and build with the caller's Clang (`--build build/agent`, `--target`, `-DNAME=VALUE`) |
| `launch_environment.py` | the process environment at a launcher's product exec (`player_environment`, `agent_environment`) |
| `present_geometry.py` | a capture the caller names (PPM or PNG) |
| `widescreen_pair.py` | a 4:3 and a 16:9 capture of one frame; reuses `present_geometry.py`'s reader |
| `looks_right.py` | a built product it drives through the debug server |
| `fps60_check.py` | an fps60 dump capture |

The title gets `external/psxport` from psxport's `tools/psxport_fetch.py`, run from psxport `main` by
its launcher and CI. Maintainer builds use `CC=clang CXX=clang++`.

The RE-frontier tracker is re-harness `tools/re_frontier.py`.
