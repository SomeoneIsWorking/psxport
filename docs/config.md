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

`tests/test_config_cvar.cpp` exercises precedence, invalid input, environment auditing, and runtime
mutation through the production registry. `tests/test_diagnostic_run.cpp` proves product,
comparison, nesting, and invalid-role behavior through the shipping enhancement gate;
`tests/test_dynarec_contract.cpp` proves zero/nonzero telemetry and both sides of fallback threshold
enforcement. The product-boundary check rejects CPU-engine selectors and explicit interpreter mode
independently of configuration tests.
