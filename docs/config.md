# Configuration and logging

`runtime/psx/config.cpp` is the only owner of product configuration. Runtime code reads declared,
typed variables through `runtime/psx/config_var.h`; C compatibility call sites use `cfg_on`,
`cfg_int`, and `cfg_str` from `runtime/psx/cfg.h`. No CPU-engine selector is a product setting.

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
`runtime/psx/dbg_server.h`, and both of its readers use it: `DbgServer::start` binds the port, and
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
zero limit executes zero interpreter instructions. Lightrec shutdown and explicit turn-end reports
name executor calls, executed blocks/instructions, admitted and refused fallback blocks, every
admitted/refused reason count, and the policy applied to the most recent execution.

## Logging

Runtime code logs through `cfg_logi`, `cfg_logw`, `cfg_loge`, or channel-gated `cfg_logf`. A call
site does not wrap a logger invocation in its own debug conditional. Expensive diagnostic work may
be guarded with `cfg_dbg`, then emits each complete line through the configured logger.

## Player settings

Player-facing settings are declared in `runtime/psx/config_vars.h` and exposed by the runtime UI.
Persistent settings use the platform user-data location supplied by the consuming title. The
checkout, current directory, and environment are not player-storage defaults.

## Verification

A run that asked for a wide picture and did not get one is told so, by name and by reason, in the same
`[wide]` announcement: `classifyWide()` in `runtime/psx/picture_announce.h` decides between "widened",
"nobody asked", "this Core is PURE", "ASPECT_AUTO resolved to a sink that is not wide", and "a wide
aspect was allowed and the width still did not grow", and a refused outcome is a warning rather than a
number to be noticed later. This exists because two titles each published a body of widescreen evidence
that was not widescreen, for the ASPECT_AUTO reason, and corrected the claim in their own docs rather
than at the moment it happened.

`tests/test_config_cvar.cpp` exercises precedence, invalid input, environment auditing, and runtime
mutation through the production registry. `tests/test_debug_server_port.cpp` pins the endpoint's port
contract, including the `1` sentinel and every shape of text that must not bind a port.
`tests/test_picture_announce.cpp` drives the shipping `classifyWide` over the exact geometries two
repositories recorded, and the mutation that silences the AUTO verdict fails it.
`tests/test_diagnostic_run.cpp` proves product,
comparison, nesting, and invalid-role behavior through the shipping enhancement gate;
`tests/test_dynarec_contract.cpp` proves zero/nonzero telemetry and both sides of fallback threshold
enforcement. The product-boundary check rejects CPU-engine selectors and explicit interpreter mode
independently of configuration tests.
