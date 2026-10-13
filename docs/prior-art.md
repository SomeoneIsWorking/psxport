# Prior art

Outside work that bears on the framework or on more than one title. Per-title decomps, symbol maps and
ports live in each title's `docs/prior-art.md`; check those before RE on a title. Where a reference and a
measurement disagree, the measurement wins.

| project | URL | kind | license | what we take | notes |
|---|---|---|---|---|---|
| OpenDriver2/PsyCross | https://github.com/OpenDriver2/PsyCross | native reimplementation of the Psy-Q SDK (`libgte`, `libgpu`, `libspu`, `libcd`) | MIT | behaviour reference for SDK semantics | Submodule `external/psycross`, never built. Different architecture from our SDK layer. |
| sozud/psy-q-decomp | https://github.com/sozud/psy-q-decomp | matching decomp of the PS1 Psy-Q libraries | not checked | names for identifying stock library functions | Check the license before taking anything. |
| decomp.dev | https://decomp.dev | progress tracker for matching decomps | — | discovery | Lists Tomba! 1 (`hansbonini/psx_tomba`) and Vagrant Story. psxport measures RAM parity, not object identity, so it has nothing for `objdiff` to report. |

## Per-title docs

| title | doc | strongest source |
|---|---|---|
| Spyro 1/2/3 | `spyro/docs/prior-art.md` | TheMobyCollective `spyro-1`/`spyro-2`/`spyro-3`, CC0, same executables |
| Crash 1/2/3 | `crash/docs/prior-art.md` | `khasinski/crash-bandicoot-decomp` (Crash 1, no license: names only) |
| Crash Team Racing | `ctr/docs/prior-art.md` | CTR-ModSDK `gcc-syms926.txt`, GPL-3.0, matches `SCUS_944.26` |
| Crash Bash | `crashbash/docs/prior-art.md` | |
| Tomba! 1/2 | `Tomba2Engine/docs/prior-art.md` | `psx_tomba` (no LICENSE), tomba2-decomp, Tombi-Decomp, mstan recompilations: names and boundaries only |
| Spider-Man 1/2 | `spider1/docs/prior-art.md` | |
| Toy Story 2 | `toystory2/docs/prior-art.md` | PeriBluGaming/ToyStory2Recomp seeds match our entry `0x80082D60` |
| Tekken 3 | `tekken3/docs/prior-art.md` | Tek3Ex (MIT) patterns locate the `TEKKEN3.BNS` table on our USA build |
| Vagrant Story | `vagrant/docs/prior-art.md` | vendored CC0 `rood-reverse` |
| Mega Man X4 | `megamanx4/docs/prior-art.md` | vendored AGPL `mmx4`; never lift it into psxport |
| C-12 | `c12/docs/prior-art.md` | |
