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
mutation through the production registry. The product-boundary check rejects CPU-engine selectors
and explicit interpreter mode independently of configuration tests.
