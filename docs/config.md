# Configuration and logging

`runtime/psx/config/config.cpp` is the only owner of product configuration. Runtime code reads declared,
typed variables through `runtime/psx/config/config_var.h`; C compatibility call sites use `cfg_on`,
`cfg_int`, and `cfg_str` from `runtime/psx/config/cfg.h`. No CPU-engine selector is a product setting.

## Precedence

Declared variables resolve in this order, from lowest to highest precedence:

1. compiled default;
2. the user settings file;
3. a `PSXPORT_*` environment override;
4. a runtime control-channel override.

Environment values are read only by the configuration owner and are never persisted. Runtime
overrides last for the process lifetime only. Invalid values and unknown names fail or are reported
at the configuration boundary; callers do not add local `getenv()` fallbacks.

The resolved value, source layer, and complete `PSXPORT_*` environment denominator are available
through `psx::config::report()` and the `cvars` runtime command. `PSXPORT_LOG_FILE` selects the log
sink. `PSXPORT_DEBUG` is the comma-separated diagnostic-channel set.

## The live debug endpoint

`PSXPORT_DEBUG_SERVER` names a loopback TCP port for the live, non-blocking debug endpoint, where `1`
asks for the default port 5959 and a number asks for that port; unset, empty, `0` and anything that is
not a number in range leave it off. The text is interpreted once, by `debug_server_port()` in
`runtime/psx/debug/dbg_server.h`, and both of its readers use it: `DbgServer::start` binds the port, and
`debug_server_live()` tells a boot spine that a client will drive the run, which is why such a run is
not frame-capped.

The endpoint is a framework service, not a property of one boot spine. A title that owns its own frame
driver still has to call `DbgServer::start()` once, `DbgServer::honourPause()` before each frame, and
`DbgServer::service()` after it; the pause policy itself lives in the framework so the spines cannot
disagree about what a pause does. A title that omits those calls has no endpoint, and the symptom is a
connection refused against a product that is otherwise running normally.

`tools/dbgclient.py` is the client: a one-shot CLI, and `LiveClient` for a driver that keeps a session.
Two commands exist so a running product can be QUERIED rather than read out of its log: `cvars` answers
the effective configuration with the layer each value came from, and `guest` answers the dynarec's own
denominators (translated and executed blocks and instructions, cache hit/miss, host dispatches,
invalidations, faults, and interpreter fallback by every reason it counts).

## Diagnostic runs and bounded fallback

`PSXPORT_DIAGNOSTIC_RUN` accepts `product`, `compare-candidate`, or `compare-reference`. It labels
the role of the same dynarec/native runtime; it is not a CPU-engine selector. Consumer code reads
`psx::config::diagnostic_run_mode()` and harnesses use the typed, nestable
`psx::config::ScopedDiagnosticRun`. Both comparison roles suppress requested enhancements through
the shared `psx::config::enh()` / `enh_named()` gate so title code does not duplicate comparison
configuration semantics. Invalid roles fail closed at that gate and are logged by name.

`PSXPORT_LIGHTREC_FALLBACK_BLOCK_LIMIT` is the maximum automatic interpreter-fallback blocks one
bounded executor call may admit. Its default is `1`, matching the verified difficult-block escape;
a second fallback block in the same call is a typed execution fault. `0` disables automatic
fallback admission without creating a selectable interpreter mode. Negative values are invalid and
fault before guest execution. Lightrec asks the executor before entering any fallback block, so a
zero limit executes zero interpreter instructions of a block fallback. The one exemption is the
cross-block load-delay hazard (`load_delay_hazard` in the telemetry): a taken branch whose delay slot
loads `$r` into a block whose first instruction reads `$r` must see the OLD `$r`, and Lightrec
resolves that by interpreting at most three guest instructions (the first instruction, or a first
branch with its delay slot). It is architected R3000 behaviour rather than a refused compilation,
hand-scheduled loops hit it once per iteration (Toy Story 2's GTE clipper at `0x800202F0`), and a
per-call block limit would fault correct guest code, so it is admitted without consuming the limit
and reported by its own counters against `executed_instructions`. Lightrec shutdown and explicit turn-end reports
name executor calls, executed blocks/instructions, admitted and refused fallback blocks, every
admitted/refused reason count, and the policy applied to the most recent execution.

## Logging

Runtime code logs through `cfg_logi`, `cfg_logw`, `cfg_loge`, or channel-gated `cfg_logf`. A call
site does not wrap a logger invocation in its own debug conditional. Expensive diagnostic work may
be guarded with `cfg_dbg`, then emits each complete line through the configured logger.

## Player settings

Player-facing settings are declared in `runtime/psx/config/config_vars.h` and exposed by the runtime UI.
Persistent settings use the platform user-data location supplied by the consuming title. The
checkout, current directory, and environment are not player-storage defaults.

## Verification

A run that asked for a wide picture and did not get one is told so, by name and by reason, in the same
`[wide]` announcement: `classifyWide()` in `runtime/psx/present/picture_announce.h` decides between "widened",
"nobody asked", "this Core is PURE", "ASPECT_AUTO resolved to a sink that is not wide", and "a wide
aspect was allowed and the width still did not grow", and a refused outcome is a warning rather than a
number to be noticed later. The announced `render_width` is the width the presenter will actually
draw, derived with the presenter's own `present_display_width` from the framebuffer being handed
over — because there are TWO widening mechanisms and only one of them is visible from the host wide
engine. The host PC enhancement widens a native-render title; the title-owned
`GuestWidescreenProjection` widens a GTE-path title, which is every widescreen-only title, since those
declare `RenderCapabilities::widescreenOnly()`. Reading only the first is how two titles measured a
false negative, and how the warning above then declared a correct run's claim void.

`runtime/psx/wide_2d_layout.{h,cpp}` owns the matching 2D question. The layout rule itself is
`rq_2d_xform` (centring authored-4:3 coordinates by (ww − native_w)/2, with a uniform untextured fill
stretching instead) and it is correct; what was wrong is the question its application site asked, which
was answered from the host wide engine alone and so was false on every frame of a Gte-path title. The
owner asks about both mechanisms in the order the presenter prefers, treats them as alternatives rather
than a sum, and exposes the decision as a pure function so it can be asked without a product.

`tests/test_config_cvar.cpp` exercises precedence, invalid input, environment auditing, and runtime
mutation through the production registry. `tests/test_debug_server_port.cpp` pins the endpoint's port
contract, including the `1` sentinel and every shape of text that must not bind a port.
`tests/test_picture_announce.cpp` drives the shipping `classifyWide` over the exact geometries two
repositories recorded, and the mutation that silences the AUTO verdict fails it.
`tests/test_wide_2d_layout.cpp` drives the shipping 2D-layout decision over both mechanisms at the
geometries that were affected, and the mutation that restores the pre-fix host-engine-only question
fails it.

## PSXPORT_OVERRIDE_DIFF — does this native override equal its original?

`PSXPORT_OVERRIDE_DIFF` arms the per-function override differential (`runtime/cpu/override_differential.h`)
for a comma- or space-separated list of **registered override names** or **`0x`-prefixed guest entry
addresses**. Empty (the default) disarms. For each selected override it shadows the first
`PSXPORT_OVERRIDE_DIFF_FIRST` calls (default 16) and every `PSXPORT_OVERRIDE_DIFF_EVERY`th call after
that (default 64; 0 = none): the original runs live, the native runs against the restored entry state
with the original's device traffic replayed to it, the two post-states are compared under the MIPS O32
contract, and the run continues from the **original's** state. `PSXPORT_OVERRIDE_DIFF_DEAD_STACK`
(default 8192) bounds the callee-frame window below the entry `sp` whose differences are counted but not
judged. `PSXPORT_OVERRIDE_DIFF_REPORT` (default `scratch/override_differential.json`, cwd-relative) is the
JSON report; gate on it with

    uv run --frozen python tools/port/override_differential_gate.py scratch/override_differential.json --require <name>

which exits nonzero on any mismatch, on a requested selector that sampled zero calls or only
incomparable ones, and on an incomplete report. An unparsable list is refused by name and arms nothing,
so the gate then fails on the missing report. What is and is not compared is written into the report
itself (`compared`, `not_observed`).

## PSXPORT_STORE_OBSERVE — which instruction wrote this guest word

`PSXPORT_STORE_OBSERVE` is a comma- or space-separated list of hex **guest addresses of store
instructions**, up to `kMaxObservedStoreTargets` of them, e.g. `0x80083884,0x8007DB3C`. Empty is the
default and disarms, so an ordinary run pays one string compare and nothing per instruction. It arms the
dynarec store observer, which names the **guest** PC of a translated store together with the full
register file.

**THESE ARE STORE PCs, NOT DATA ADDRESSES — and that is the whole usability question.**
`LightrecExecutor::Impl::observeStore` matches `target.guestPc != guestPc`, the pc of the translated
STORE. So the instrument answers "what does this store instruction write, and with what registers", NOT
"which instruction wrote this word". Arming a data address matches nothing and the resulting zero is not
evidence. Measured 2026-09-27 on Spyro 1: arming the two store PCs `0x8007DB3C` and `0x8007DB54` produced
**24,332 correctly attributed callback lines** with both positive controls firing, while arming the data
address `0x80078AE0` produced `MATCHED NONE of the 9,366,306 executed JIT instruction(s)` on a word that
demonstrably changes every frame. The per-target report row echoes the armed value in a column that
reads like an address, which is exactly what makes the mistake easy. **To find which instruction wrote a
word you must already know the instruction**: use `PSXPORT_CW` for host-reaching stores, or read the
store out of the listing. An earlier version of this section claimed the surface named "the instruction
that wrote this word", and that claim was wrong in the way that matters — it invited arming a data
address and reading the silence as a result.

**A correction, because the belief it replaced was measured false and that belief is what cost time.**
This section previously said `PSXPORT_CW` "sees host-side stores only" and therefore "cannot attribute a
guest-executed one". That is wrong: `PSXPORT_CW` *does* fire for guest stores that Lightrec routes
through its slow `lightrec_rw` path, because that path re-enters `Core::writeGuestMemory` — measured on
Spyro 1, where watching `g_Spyro + 0x88` logged 38 host-reaching stores with a guest PC, 35 of them
`= 00000001`. What is true is narrower: *most* stores to a hot word are inlined by Lightrec and never
reach the host at all, so `PSXPORT_CW`'s coverage is **unbiased toward the slow path** and a quiet watch
means "no store took the slow path", not "no store happened". A store observer that watches the
translated block has no such bias, which is what this surface is for. An earlier note here also claimed
`PSXPORT_STORE_OBSERVE` was blocked because nothing could arm it — true of the *first* version, which
was compiled, documented, and reachable from nothing; it is now called from `native_boot.cpp` beside the
live endpoint, and the product's own symbol table was checked to prove it.

Two honest limits, both stated in `runtime/cpu/store_observe.cpp` rather than papered over:

- `StoreObservation` does **not** carry the address that was written. With several addresses armed, run
  them one at a time, or read `store_observe_report`, whose per-target rows do carry each address's
  store counts and the guest PC of its last one. The callback line therefore does not invent a target.
- An unarmed observer reports **nothing**, so its silence is not evidence. `store_observe_report` prints
  the executor's own `executedJitInstructions` / `fallbackInstructions` beside the per-target counts, and
  the arming lines print how many addresses were accepted — so "ran and matched nothing" is
  distinguishable from "never ran". An unparsable or over-long list is REFUSED with the offending token
  named, rather than silently watching a prefix, which is the failure this area has already produced
  once.

A title wanting richer handling than a log line calls
`LightrecExecutor::configureStoreObserver` itself with its own callback; this is the
configuration-driven path, so an investigation needs no product edit.
`tests/test_diagnostic_run.cpp` proves product,
comparison, nesting, and invalid-role behavior through the shipping enhancement gate;
`tests/test_dynarec_contract.cpp` proves zero/nonzero telemetry and both sides of fallback threshold
enforcement. The product-boundary check rejects CPU-engine selectors and explicit interpreter mode
independently of configuration tests.
