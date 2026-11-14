# psxport

psxport is the shared PlayStation runtime for native/dynarec game ports. It owns the PSX CPU
integration boundary, devices, HLE, rendering, audio, input, configuration, and title-neutral host
services. A consuming title owns game identity, native overrides, frame/task policy, enhancements,
and the legally obtained game files.

## What it does

![Spyro the Dragon: the original 30 fps beside psxport's 60 fps interpolation](https://raw.githubusercontent.com/SomeoneIsWorking/psx-spyro/main/docs/media/spyro1-interpolation.webp)

*60 fps interpolation: Spyro the Dragon's opening flyby at the original 30 fps (left) and interpolated (right).*

![Spyro the Dragon: the original 4:3 picture beside 16:9 widescreen](https://raw.githubusercontent.com/SomeoneIsWorking/psx-spyro/main/docs/media/spyro1-widescreen.png)

*Widescreen: the same Spyro the Dragon frame at the original 4:3 and at 16:9, where the margins show more of the level.*

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
`1daea1aaf0373fa861dc77a58e072b7b63bc49ca`. `LightrecExecutor` runs translated guest blocks,
synchronizes architectural state, intercepts image-scoped native/HLE calls at block boundaries,
stops original calls at their exact guest continuation, publishes exact execution/cache/fallback
telemetry, and participates in cache invalidation. Multi-`Core` backend qualification, complete
executable-writer coverage, and both AArch64 hosts remain open; this is not yet a playable title
claim.

See [project state](docs/project-state.md), [migration requirements](docs/migration.md), and the
[codemap](docs/codemap.md).

## Development

The canonical Python owner configures Ninja and builds the framework. Maintainers select Clang
explicitly; the project accepts supported compilers through CMake and the usual CC/CXX environment:

```sh
CC=clang CXX=clang++ uv run --frozen python tools/build.py
```

Hosted CI builds the framework on Linux x86-64 with full repository history
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

Third-party assets and code:

| asset | source | licence |
|---|---|---|
| `assets/rml/FiraSans-*.ttf`, `FiraSansCondensed-Bold.ttf` | Fira Sans, Mozilla Foundation and Telefonica S.A. | SIL OFL 1.1, `assets/rml/FiraSans-OFL.txt` |
| `assets/rml/promptfont.ttf` | PromptFont v1.10, Yukari Hafner and Severin Meyer (github.com/Shinmera/promptfont) | SIL OFL 1.1, `assets/rml/PromptFont-OFL.txt` |
| `assets/rml/Fredoka.ttf` | Fredoka (variable), The Fredoka Project Authors, from google/fonts `ofl/fredoka` at a60a77e1 | SIL OFL 1.1, `assets/rml/Fredoka-OFL.txt` |
| `assets/rml/NotoSansJP-Symbols.ttf` | Noto Sans JP, Google LLC and Adobe, from google/fonts `ofl/notosansjp` at 66a36c8c: the wght 700 instance, subset to the symbols Tomba! 2's fonts draw (command below) | SIL OFL 1.1, `assets/rml/NotoSansJP-OFL.txt` |
| `vendor/msdfgen` | msdfgen v1.13.1, Viktor Chlumsky (github.com/Chlumsky/msdfgen) | MIT, `vendor/msdfgen/LICENSE.txt` |

`NotoSansJP-Symbols.ttf` is made from `NotoSansJP[wght].ttf` with the locked environment's fontTools:

    uv run --frozen fonttools varLib.instancer "NotoSansJP[wght].ttf" wght=700 -o NotoSansJP-700.ttf
    uv run --frozen pyftsubset NotoSansJP-700.ttf --text='※⇨⇦⇧⇩、。，ー‐＼〜…‥‘’“”「」☆★○●◎◇◆□■△▲▽▼♪♂♀℃『』♦♣♥♠⁉‼©' \
        --layout-features= --no-hinting --name-IDs='*' --output-file=assets/rml/NotoSansJP-Symbols.ttf

Built with AI assistance (Claude and other LLMs).
