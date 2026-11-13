# `psxport/tools/port/`: shared title tooling

Title-neutral engines that every title runs from its `external/psxport`. The data each reads stays in
the title repo; paths resolve against the working directory (the title root), never this file. An
engine refuses rather than passes when it reads nothing.

| engine | reads (per title) |
|---|---|
| `consumer_build.py` | a title root: one Ninja configure and build with the caller's Clang in a purpose tree (`--build build/agent` or `build/ci`, never a per-task tree; `--target`, `-DNAME=VALUE`) |
| `launcher.py` | a title's `tools/run.py`: dependency refusals, the `build/player` configure and build, the framework discdump in `build/player-tools`, the product exec |
| `remote_install.py` | a title root and an ssh host: the launcher's product exec recorded as a plan (`PSXPORT_LAUNCH_PLAN`) on the newest build, staged with its non-system and host-missing libraries, the UI assets, the `.env` and a `play.sh` into `~/Games/<product>/` there, disc paths moved by `--rom-map LOCAL=REMOTE`, plus the title's desktop entry |
| `launch_environment.py` | the process environment at a launcher's product exec (`player_environment`, `agent_environment`) |
| `present_geometry.py` | a capture the caller names, read with Pillow |
| `widescreen_pair.py` | a 4:3 and a 16:9 capture of one frame; reuses `present_geometry.py`'s reader |
| `looks_right.py` | a built product it drives through the debug server |
| `fps60_check.py` | an fps60 dump capture |

The title gets `external/psxport` from psxport's `tools/psxport_fetch.py`, run from psxport `main` by
its `bootstrap.py` and CI. Maintainer builds use `CC=clang CXX=clang++`.

The RE-frontier tracker is re-harness `tools/re_frontier.py`.
