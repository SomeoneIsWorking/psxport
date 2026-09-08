# Full-console diagnostic reference

`console.py` drives the pinned Beetle **full software console** through the public libretro C ABI.
It is separate from the CPU-window tools described in [README.md](README.md), and from the shipping
PSXPort runtime. The reference executes the Mednafen interpreter with CD, GPU, SPU, timers, DMA and
controller devices. It creates no window and consumes every audio sample without playback.

The shared re-harness tooling has no reusable libretro host. This owner keeps the ABI declarations,
session callbacks, framebuffer conversion, firmware admission and build policy in separate modules.
Its callbacks retain their Python owners for the complete native lifetime. A callback exception
poisons the session and raises at the next foreign-call return; execution never resumes afterward.

## Build and test

From the framework checkout, use its locked Python environment:

```sh
uv run --frozen python tools/oracle/test_console_host.py
uv run --frozen python tools/oracle/console.py build --cc clang --cxx clang++ --jobs 6
```

The builder requires Git, Make, a C/C++ compiler, pkg-config and zlib development files. It exports
only the recorded Beetle gitlink, verifies required full-core inputs, and never modifies the vendor
checkout. Hardware rendering and Lightrec are disabled at build time. The fork's GTE state declaration
is supplied from `runtime/psx/gte_state.h`; its digest is part of the build identity. Source exports,
objects, library and identity manifests live in `build/oracle-console`. Linux `.so` and macOS `.dylib`
are recognized; other platforms refuse explicitly. Recognizing a platform is not runtime qualification.
The activity lock prevents overlapping host builds/runs. The application launcher does not call this tool.

`test_console_host.py` uses synthetic buffers and invokes the actual C callback trampolines. It checks
ownership after garbage collection, copied video memory, padded rows, all three software pixel formats,
PNG bytes, callback failure propagation, options, controls, audio consumption and firmware refusal.
It has no game assets and does not certify core compatibility or game conformance.

## Explicit firmware and disc inputs

Supply a self-contained CHD and an explicit firmware choice:

```sh
uv run --frozen python tools/oracle/console.py run \
  --disc /path/to/Spyro.chd --region na --bios /path/to/SCPH1001.BIN
```

Or choose `--openbios` instead of `--bios`. The controlled system directory is then empty, selecting
the pinned core's built-in OpenBIOS firmware. OpenBIOS is an explicit reference limitation: it does
not establish authentic BIOS boot, services or timing parity. BIOS instructions execute normally;
this is not BIOS HLE. No BIOS or disc bytes are tracked or packaged.

Authentic firmware must be exactly 512 KiB and match `console_firmware.py`'s region/identity catalog.
The core's regular SCPH-5500/5501/5502 identities are admitted, as is the North American v2.2 image
with SHA1 `10155d8d6e6e832d6ea66db9bc098321fb5e8ebf`. Its MD5
`924e392ed05558ffdb115408c263dccf` is independently identified as SCPH-1001/5003/DTL-H1201/H3001,
v2.2 12-04-95 A, NTSC-U in [DuckStation's firmware identity table](https://github.com/stenzek/duckstation/blob/master/src/core/bios.cpp).
The filename alone is never trusted.

Beetle searches for the North American filename `scph5501.bin`. The host places a symlink under that
search name, while metadata preserves the admitted image's actual model, revision and SHA1. This
alias does not relabel SCPH-1001 as SCPH-5501. The pinned core accepts a full-sized alternative BIOS
with an expected-revision warning, then copies all 512 KiB. Before reporting ready, the host checks
the core's observed SHA1 against the admitted identity and refuses a missing/mismatched digest or
short read. A warning for the admitted v2.2 image remains visible in `core.log` and run metadata.
Skip-BIOS and BIOS override are disabled; the CHD route avoids the raw-executable boot patches.

The run records a SHA-256 of the complete CHD. Use `--disc-sha256 EXPECTED` to require an independently
known disc identity; merely calculating a digest does not authenticate an unknown disc. A sibling
`.toc` is refused because Beetle would prefer it to the explicitly selected CHD.

## Live control

After one JSON `ready` reply, send one JSON object per line on stdin. Each command returns JSON on
stdout; native core messages go to the stable `scratch/oracle-console/core.log`.

```json
{"command":"status"}
{"command":"buttons","buttons":["start"]}
{"command":"step","frames":1}
{"command":"buttons","buttons":[]}
{"command":"step","frames":120}
{"command":"read","address":"0x800757d8","bytes":4}
{"command":"capture"}
{"command":"ram"}
{"command":"quit"}
```

Buttons remain held until replaced. Names are `cross`, `square`, `select`, `start`, `up`, `down`,
`left`, `right`, `circle`, `triangle`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`; only digital pad port 0 is
driven. Each step is bounded to 1..3600 core fields. State reports field/video/input/audio counters,
frame dimensions, timing and all queried option values. Unknown commands or fields fail explicitly.
Read commands allow 1..256 bytes from main RAM, including its guest aliases. Capture writes only the
latest copied framebuffer as `frame.png`; RAM writes the current 2 MiB as `ram.bin`. These stable paths
are overwritten on request, with hashes in the reply. There are no per-frame image/RAM disk writes.

## Comparison validity

Record the exact disc, BIOS, core revision, options, input sequence and reached game-state checkpoint
on both sides. Field numbers are not state alignment: a port's native loading and presentation can
advance differently. Match a demonstrably equivalent camera/scene/state before comparing images or
movement. Native resolution, 100% CPU, native 2x CD speed, no PGXP, no widescreen and no overscan/image
crop are the reference settings. Differences caused by deliberate port enhancements require separate
criteria; this host does not infer alignment or claim pixel parity from a screenshot.

The reference has independent CPU execution and full-console scheduling, but shares the pinned
Beetle device implementation lineage used by PSXPort. Agreement cannot independently validate a bug
shared by those devices. Main-RAM output is a diagnostic dump, not an exact console save state; it
omits CPU registers, device state and timing. The host does not implement libretro save-RAM
persistence, so saved-game progression parity is not qualified. Existing core-managed save filenames
are recorded in `run.json`; establish the same starting save state before any relevant comparison.

Build provenance and `run.json` are evidence metadata, not evidence that a game booted or played.
Only a reached and observed scenario can support those claims.
