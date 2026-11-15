# Windows release

Owns the x86-64 Windows archive of every title: the cross toolchain, the target libraries, the package
and its audits. Rules it meets are the `release` skill's; `docs/appimage.md` is the Linux counterpart and
shares its title identity, version and content audit (`tools/port/release_package.py`).

| piece | role |
|---|---|
| `cmake/toolchains/llvm-mingw-x86_64.cmake` | llvm-mingw (UCRT) toolchain; `-static`, so the C++ runtime and winpthreads are linked in |
| `tools/port/windows_target.py` | llvm-mingw 20260922 (checksummed), the target libraries and Lightning, each built once per pin under the dependency root |
| `tools/port/target_libraries.py`, `target-libraries.json` | zlib, SDL3 (fork), SDL3_image and FreeType from pinned, checksummed sources; the AppImage image builds the same pins |
| `tools/port/consumer_build.py --platform windows` | the title's build in `build/windows` |
| `tools/port/windows_package.py` | `<Name>-<version>-windows-x86_64.zip`: `<Name>/<Name>.exe` (stripped), `assets/`, the licence |
| `.github/workflows/title-release.yml` `windows` | the same build and package on the Ubuntu runner, uploaded to the title's release |

## Inputs and outputs
In: a clean title commit, its desktop entry `Name=` and licence, the Lightrec and Lightning pins in
`cmake/lightrec_dependency.cmake`. Out: `build/package/windows/out/<Name>-<version>-windows-x86_64.zip`.

## Invariants
- No game content: the shared content audit refuses disc images, PS-X EXEs and manifest-named files.
- Every imported DLL ships with Windows 10 or later (system DLLs and the Universal CRT); anything else is
  refused.
- An executable beside `assets/` is a package: it defaults to a window, reads those assets and keeps its
  data in `%APPDATA%\<executable name>` (`psx::config::Config`, `docs/title-host.md`).
- Lightrec runs on Windows through the Lightning fork's Win64 support: Win64 ABI prolog keeps the full
  xmm6-15 (Lightrec `LR-HOST-4`).
- Host-only mechanisms have Windows owners: the watchdog's timer and fault hooks (`watchdog_win32.cpp`),
  backtraces (`host_backtrace.cpp`), diagnostic text capture (`text_capture.cpp`).

## Check
From a title root: `uv run --frozen python external/psxport/tools/port/consumer_build.py --platform windows
--target <product>`, then `windows_package.py --binary build/windows/bin/<product>.exe`. The run prints the
import and content audits. Under Wine a hidden window gets no surface, so frames come from a visible window on
a container's own Xvfb (Fedora 44, `wine-core`, `mesa-vulkan-drivers`), the archive's `<Name>.exe` started
with the disc and `PSXPORT_PRESENT_SHOT_AT`. Tomba 2 (2026-11-15): `PSXPORT_AUTO_SKIP=true` reaches free roam
at frame 764 and present 1000 shows Tomba outside his house, 0 Lightrec fallbacks, 0 faults.

## Problems met and their fixes
| problem | cause | fix |
|---|---|---|
| "Missing callbacks in lightrec_ops" on Windows only | Lightning restored xmm6-15 with `movsd`, zeroing their upper halves, where the compiler kept a live pointer | the fork's Win64 prolog saves and restores all 128 bits |
| No `mmap` in Lightning | the C runtime has none | the fork maps executable memory with `VirtualAlloc`/`VirtualProtect` |
| POSIX-only diagnostics | `execinfo.h`, `open_memstream`, `SIGALRM`/`sigaction`, `localtime_r`, `usleep` | per-platform owners named above; `SIGUSR1` snapshot trigger only where the signal exists |
| A wide path to libchdr | `std::filesystem::path::c_str()` is `wchar_t` | `Disc::openImage` takes a path and converts once |
| Hidden-window runs never present under Wine | Wine creates no surface for a hidden window, with either display driver | the container check above |

## Open
- The executable carries no icon resource.
- Unsigned: SmartScreen warns on first run.
- Not yet run on a Windows kernel; Wine 11 is the evidence.
