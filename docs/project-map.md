# Project map — read this before grepping around

The recurring time-sink in this repo is re-discovering *where things are* and *how to build*.
This is the map. Keep it current when the layout changes.

## Build & run cheat-sheet
| Want | Command | Output |
|------|---------|--------|
| Build+run the **port** (full) | `./run.sh [disc.chd]` | `scratch/bin/tomba2_port` (then runs) |
| Rebuild the **port** (incremental, no run) | `tools/build_port.sh [files… \| all]` | `scratch/bin/tomba2_port` |
| Build the **oracle** (Beetle reference) | `make -C runtime` | `runtime/wide60rt` |
| Drive the port or oracle interactively | `tools/drive.py start native\|oracle` … | — |
| Inspect BGM/libsnd state of a RAM dump | `tools/bgm.py dump <ram>` | — |

- **`make` builds the ORACLE, not the port.** The port has no Makefile (see CLAUDE.md "TWO binaries").
- `tools/build_port.sh` keeps a `scratch/obj/` object cache; one changed file relinks in ~0.5s.
  Its SRC list must mirror `run.sh` step 4 — add new `runtime/recomp/*.c` to **both**.
- `tools/drive.py` does NOT build. Build first, then drive. It sets `PSXPORT_NO_FMV=1`,
  `PSXPORT_BGMDBG=1`, headless (`PSXPORT_NOWINDOW`/`NOAUDIO`) by default; pass `headed` for a window.

## The port's runtime (`runtime/recomp/*.c`) — module responsibilities
- `native_boot.c` — hand-coded boot + the per-frame loop (replaces the PSX scheduler). Frame loop
  calls the GPU present, `spu_audio_frame()`, pad poll, etc.
- `native_stub.c` — runs SCUS_944.54 (SCEA + LoadExec) as the real entry, hands off to MAIN.
- `mem.c` — bus dispatch (routes MMIO ranges to gpu/spu/cdc/pad/timer handlers).
- `gpu_native.c` — GP0/GP1, VRAM, present. `spu_beetle.c` — lifts Beetle `spu.c` (the mixer);
  `spu_audio.c` — SDL output sink + `PSXPORT_WAV` capture (advances SPU one NTSC frame/video frame).
- `mdec_beetle.c`/`gte_beetle.c` — lift Beetle `mdec.c`/`gte.c`. `native_fmv.c` — STR/MDEC FMV player
  **and** the shared `xa_decode_sector()` / BS VLC decoder (mednafen-parity, both non-static).
- `pad_input.c`, `memcard.c`, `timing.c`, `threads.c`, `interp.c`, `hle.c`, `games_tomba2.c`.

## CD path — the part that's easy to get wrong
The port does NOT emulate the CD controller for the game; `cd_override.c` replaces libcd/engine
read primitives with synchronous native disc reads (`disc.c` → libchdr). **There are TWO CD-command
wrappers, and the cutscene XA path uses the second one:**
- `FUN_8008AC34` (libcd `CdControl`) → `ov_cd_command`. Boot/menu uses this.
- `FUN_8001CE90` (the **engine's streaming** CD-command wrapper, used by the streaming reader
  `FUN_8001cfc8`/task) → `ov_cd_cmd_stream`. **In-game cutscene XA-ADPCM streaming goes through
  HERE** (Setmode 0xC8 = Speed+ADPCM+SF-filter, Setfilter file/chan, Setloc, ReadS).
- Both wrappers route the streaming-relevant commands to the native XA engine `xa_stream.c`
  (Setmode/Setfilter/Setloc/ReadS/Pause). Debug both with `PSXPORT_CDCMD_DBG=1` (logs every command
  + params from both wrappers).
- `ov_cd_cmd_stream` also fakes `GetlocL` (cmd 0x10) drive position — the streaming reader polls it.
  See its comment; during XA it must report the **advancing** play position (else the cutscene,
  which waits on the head reaching the clip end, never advances).

### In-game XA-ADPCM audio (`xa_stream.c`) — added 2026-06-16
Decodes CD-XA from the ReadS-streamed sectors and feeds the SPU's CD-audio input via
`CDC_GetCDAudioSample()` (Beetle `spu.c` calls it once per 44.1kHz sample, scaled by the game's
`CDVol` + gated on `SPUControl` bit0 — both game-set). Pull-driven (decode on consumption →
self-paces to realtime). `xa_decode_sector()` lives in `native_fmv.c`. The SPU mixes XA only when
the game enabled CD audio; sequenced (libsnd) BGM is a SEPARATE working path. Debug: `PSXPORT_XA_DBG=1`.

## Where state/notes live
- `docs/journal.md` — chronological findings + dead ends (read the head before re-deriving).
- `docs/diff-driver.md` — the drive.py/bgm.py oracle-comparison workflow (committed copy of the skill).
- `<local-notes>/.../memory/MEMORY.md` — cross-session pointers (machine-local; in-repo docs are canonical).
- `scratch/` — gitignored artifacts by type (`bin/ wav/ screenshots/ raw/ logs/ state/`). Never `/tmp`.
