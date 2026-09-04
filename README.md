# psxport

psxport is the shared PlayStation runtime for native/dynarec game ports. It owns the PSX CPU
integration boundary, devices, HLE, rendering, audio, input, configuration, and title-neutral host
services. A consuming title owns game identity, native overrides, frame/task policy, enhancements,
and the legally obtained game files.

The product architecture has one CPU policy: host-native functions plus dynarec-translated R3000A
code from the authenticated user image. A backend-owned interpreter fallback is automatic only for
classified translation failures, counted, bounded by pre-execution admission, and never
player-selectable. Independent emulators may be built as test tools, but are never linked into
gameplay.

## Current state

`runtime/cpu/` provides
per-`Core` typed execution exits, execution control, image/generation identity, native and scoped
original calls, guest ABI helpers, centralized invalidation, and measurable executor counters.

The Linux x86-64 path consumes the maintained Lightrec fork at revision
`b764c4c9f4bc425a56bfc4c32333ff8200ce8ab9`. `LightrecExecutor` runs translated guest blocks,
synchronizes architectural state, intercepts image-scoped native/HLE calls at block boundaries,
stops original calls at their exact guest continuation, publishes exact execution/cache/fallback
telemetry, and participates in cache invalidation. Multi-`Core` backend qualification, complete
executable-writer coverage, and both AArch64 hosts remain open; this is not yet a playable title
claim.

See [project state](docs/project-state.md), [migration requirements](docs/migration.md), and the
[codemap](docs/codemap.md).

## Development

Framework verification uses the canonical Python owner, which configures a Clang/Ninja build and
runs the complete asset-free suite:

```sh
uv run --frozen python tools/verify.py
```

`uv run --frozen python tools/build.py` performs the same configure/build without running tests.
Hosted CI exercises the real synthetic Lightrec runtime on Linux x86-64 with full repository history
and the exact fork revision. Unsupported platforms are not represented by green placeholder jobs.

Consumers supply the runnable product and launcher. A consumer must authenticate user media, link
the dynarec-default backend, and register image-qualified native overrides before entering guest
code. No game file or BIOS is included in this repository.

## Platform contract

The maintained Lightrec fork must support x86-64 Linux, Apple Silicon macOS, and Android
`arm64-v8a`. Each platform requires native host code generation, executable-memory publication,
instruction-cache coherence, invalidation, and ABI-correct entry/exit transitions. A bounded fallback
may handle only named rare failures and must never replace AArch64 code generation. Desktop x86-64
evidence does not qualify either AArch64 platform.

## License

Framework code is provided for research and preservation. Vendored components retain their own
licenses. No copyrighted game assets are distributed.
