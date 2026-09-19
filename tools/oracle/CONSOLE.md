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
uv run --frozen python tools/oracle/test_console_observer.py
uv run --frozen python tools/oracle/console.py build --cc clang --cxx clang++ --jobs 6
```

The builder requires Git, Make, a C/C++ compiler, pkg-config and zlib development files. It exports
only the recorded Beetle gitlink, verifies required full-core inputs, and never modifies the vendor
checkout. Hardware rendering and Lightrec are disabled at build time. This diagnostic build explicitly selects
`PSX_PC_OBSERVER=1`; product and default upstream core builds omit the observer module and all
CPU-loop references. The framework verifier separately enables its CPU-only diagnostic regression. The manifest records the observer ABI. Uncommitted fork edits are not build inputs. The fork's GTE state declaration
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
{"command":"insert_card","card":"/abs/path/blank-formatted.mcr"}
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

### Memory card

`insert_card` replaces slot 1's contents with a 128 KiB image before the first `step`, so a
comparison can start both cores from the SAME card. It is refused after stepping, for any size other
than 128 KiB, and when the core exposes no save RAM; the reply carries the byte count, the image's
SHA-256 and its first two bytes, so a run names the card it used.

The card travels as a PATH, not as bytes: the control protocol bounds one command line at 65,536
characters and a 128 KiB card is 262,144 hex characters. Both ends are the same machine by
construction, since the controlling process spawns this one.

This is needed because card state selects a title's menu route, and the two cores do not otherwise
start from the same card. Note what is NOT the difference: this core comes up with a card that is
already FORMATTED (`InputDevice_Memcard_Ctor` in `mednafen/psx/frontio.c` calls
`InputDevice_Memcard_Format`). What it does carry is `presence_new`, the PSX device flag byte bit 3
that a card asserts from power-on until a frame is written to it, cleared only in the write-end
path. A title branches on that flag, not on the format -- Spyro 1's `CREATING SAVE FILE...` page is
the flag, not an unformatted card (spyro issue 0123).

`--console-card CARD.MCR` on `compare.py` and `picture.py` drives this. It defaults OFF: mirroring a
card by default would hide a product that cannot serve the card state the reference starts from.

## Bounded PC observations

The optional fork extension copies active CPU-local state before opcode semantics and pending-load
commit, after instruction fetch and cycle/read-absorb bookkeeping. Interrupt/halt dispatch is not a
guest-instruction observation. Registers are r0..r31 followed by LO and HI; PC, next PC, instruction,
branch-delay state, pending-load register/value, field-local CPU timestamp, CP0 Status/Cause/EPC and
requested RAM bytes are explicit. This is an observation record, not a resumable CPU/device snapshot.
A field label is the upcoming `retro_run` call; it does not mean display-phase alignment.

Configure only while the console is paused between commands. For example, with synthetic addresses:

```json
{"command":"observe","targets":[{"pc":"0x80001020","return":true}],"ranges":[{"address":"0x80002000","bytes":8}],"capacity":64}
{"command":"step","frames":1}
{"command":"observe_read"}
{"command":"observe_off"}
```

A return observation means arrival at the entry's saved RA with its saved SP and no branch delay,
before the caller's next opcode. It does not redirect execution or install a guest breakpoint.
Reentry while a return is pending is explicitly unsupported and makes the census incomplete. A
non-returning function leaves a pending pair. Select at most four unique aligned PCs, eight nonempty
main-RAM spans (physical, KSEG0 or KSEG1 only) totaling 512 bytes, and a capacity of 1..128 records. The C CPU loop copies RAM directly;
there are no per-instruction Python callbacks, device reads, writes, or forced pipeline changes.

`observe_read` drains copied records and reports cumulative scanned instructions, matched events,
retained records, dropped events, pairing errors and each target's entry/return counts. A full buffer
continues execution and counting, while dropped events make the result incomplete. Drain between
bounded steps to keep complete coverage. Zero matches explicitly report a nonzero scanned denominator
and incomplete status. Every configured target must be observed and every requested return paired;
all counters remain visible after `observe_off`. A valid new configuration starts a new census; an
invalid one preserves the previous configuration and census.

The canonical asset-free verifier (`tools/verify.py`, also used by hosted CI) explicitly configures
`PSXPORT_ORACLE_PC_OBSERVER=ON` through `tools/project.py`. The raw CMake option defaults off.
For focused qualification in the configured `build/ci` tree, build `test_console_pc_observer`
and run CTest `^oracle_pc_observer$`. It executes the actual Mednafen loop
and checks observer on/off state preservation, active pending-load capture, saved return arrival,
unreachable PC, buffer overflow and invalid-configuration preservation. This is CPU-boundary evidence;
it cannot qualify full-console devices. The option affects only the separately linked oracle library,
never the product CPU or its runtime linkage.

## Canonical on/off capture windows

```json
{"command":"hashes_begin"}
{"command":"step","frames":2}
{"command":"hashes"}
```

Hash windows consume the existing audio sink's actual samples. They hash canonical little-endian
signed 16-bit stereo samples, every completed field's full 2 MiB RAM, and its canonical RGB rows
(including zero row-filter bytes) preceded by little-endian width and height. Frame pitch padding and
native pixel representation are excluded. Counters report completed fields and stereo sample frames;
an empty window is incomplete. Hashing is opt-in and retains no growing frame/audio history.

To qualify observational behavior, use separate fresh full-console sessions with the same authentic
firmware, disc, starting persistence, options and input sequence. Start hash windows at the same
reached boundary, enabling the observer in only one run. Compare all three hashes and their counters,
and require complete positive observer entry/return coverage. Repeat with an unreachable PC to prove
`scanned > 0`, `matched = 0`, incomplete observation while hashes still agree. Host synthetic tests
prove hashing distinguishes changed RAM, pixels and samples; only this real full-console experiment
can establish that observer instrumentation preserves that scenario. Matching counts alone do not.

Qualified on Linux x86-64 with Clang 22.1.8 and fork commit
`5791d27a41e7ffd759f68e88114b50a1c6a168c0`: Spyro USA CHD SHA-256
`8fe0a6e735ee399a8251f2173cf61c6e20fa565611b934fa3d90788beab9d6cb`, authentic North American
BIOS SHA-1 `10155d8d6e6e832d6ea66db9bc098321fb5e8ebf`, empty saves and identical recorded inputs.
At Artisans fields 6739–6746, all 48 cumulative RAM/video/audio comparisons matched across observer
off, camera observation and unreachable-target sessions. Each window contained eight fields and
5,897 stereo sample frames. Camera observation scanned 2,822,621 instructions and retained four
entries at `0x80033c50` with four saved-return arrivals at `0x8001ede0`; drops, pairing errors and
pending returns were zero. The unreachable `0xfffffffc` target scanned the same instruction count,
matched zero and explicitly reported incomplete observation. This qualifies instrumentation in
that reached scenario; native-port parity and performance still require their own comparisons.

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
persistence, so saved-game progression parity is not qualified; `insert_card` sets the starting card
but nothing writes it back. Existing core-managed save filenames
are recorded in `run.json`; establish the same starting save state before any relevant comparison.

Build provenance and `run.json` are evidence metadata, not evidence that a game booted or played.
Only a reached and observed scenario can support those claims.
