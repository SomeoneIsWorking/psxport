# Linux AppImage release

Owns the x86_64 AppImage of every title: the build host, the package and its audits, and the GitHub
release that publishes it. The rules it meets are the `release` skill's (AppImage catalog checks); this
doc is how psxport meets them.

| piece | role |
|---|---|
| `tools/port/appimage-build.Containerfile` | Ubuntu 22.04 build image: clang-20 with libc++ 20, glslc, SDL3, SDL3_image, FreeType, Lightning and Lightrec |
| `tools/port/appimage_container.py` | runs the title's build (`build/appimage`) and the packager in that image; podman locally, docker in CI |
| `tools/port/appimage.py` | AppDir, audits, AppImage, update information and `.zsync` |
| `tools/port/appimage-tools.json` | pinned linuxdeploy, appimagetool, type-2 runtime and patchelf |
| `.github/workflows/title-release.yml` | called by each title's `release.yml` on a `v*` tag or by hand; uploads to the title's release |

## Inputs and outputs
In: a clean title commit, its `packaging/linux/<app-id>.desktop` and SVG, its `LICENSE` or `COPYING`,
optionally `<app-id>.metainfo.xml` (required when the licence is not MIT), and the pins in
`cmake/lightrec_dependency.cmake`. Out: `build/release/<Name>-<version>-x86_64.AppImage` and its `.zsync`.

## Invariants
- No game content: the packager refuses disc-image suffixes, PS-X EXE headers and any file whose hash a
  title manifest names.
- glibc 2.35 at most: every bundled ELF is read with `readelf`; a newer `GLIBC_` need is refused.
- No C++ runtime dependency: libc++ is linked statically.
- Update information `gh-releases-zsync|<owner>|<repo>|latest|<Name>-*-x86_64.AppImage.zsync`, read from
  the title's `origin`.
- Zero arguments open a window: a packaged run keeps its data in `<user data>/<executable name>` and shows
  the first-run screen when no disc is known (`docs/title-host.md`).

## Check
From a title root: `uv run --frozen python external/psxport/tools/port/appimage_container.py --product <product>`.
The run prints the content audit, the glibc audit and the extracted `AppRun --help`. Then
`<AppImage> --appimage-updateinformation`, and the catalog's window test in a stock `ubuntu:22.04`
container: Xvfb, `LANG=C.UTF-8`, `xvfb xdotool x11-utils mesa-vulkan-drivers` installed, the AppImage started
with no arguments and `xdotool search --onlyvisible --name .` polled for 30 s. Tomba (2026-11-15): the window
appears after 2 s with `WM_NAME(STRING) = "Tomba! setup"` and shows the first-run disc screen.

## Problems met and their fixes
| problem | cause | fix | xmen2 |
|---|---|---|---|
| A Fedora-built package needs glibc 2.43 | glibc symbol versions follow the build host | build in Ubuntu 22.04 and audit every ELF | same container approach (`packaging/appimage-build.Containerfile`) |
| Bundled libraries segfault the loader | linuxdeploy's own patchelf 0.15 corrupts `DT_INIT` | pinned patchelf 0.19.3 through `PATCHELF` | refuses patchelf below 0.19 and swaps it into linuxdeploy |
| No SDL3 or glslc in 22.04 | not packaged for jammy | SDL3 and SDL3_image from source; glslc from the LunarG Vulkan SDK, checksummed | same SDL; glslc from pinned shaderc source |
| `lucent::format` missing | libstdc++ 11 has no `<format>` | static libc++ 20 (`-stdlib=libc++ -static-libstdc++`) | stays on libstdc++ with a `GLIBCXX 3.4.30` ceiling; it does not need `<format>` |
| `strlcpy` undeclared in Beetle's `state.c` | `HAVE_STRL` asked the C library, which has it only from glibc 2.38 | compile libretro's `compat_strl.c`, drop `HAVE_STRL` | not applicable |
| `cmake_minimum_required` fails | Ubuntu 22.04's CMake is 3.22; titles need 3.24 | Kitware CMake 3.31.6 release tarball, checksummed | not hit |
| The packager finds no binary | titles set the output directory themselves, and two never did | `psxport_add_product` puts every product in `<build>/bin` | not applicable |
| msdfgen configure fails in CI only | CI's fresh framework clone initialised a hand-kept submodule list that lacked `vendor/msdfgen` | `psxport_fetch.py` initialises every `.gitmodules` path under `vendor/`; a failed dependency step prints its output | not applicable |
| Missing `<cstddef>` in title code | libstdc++ includes it transitively, libc++ does not | include what is used | not applicable |
| `libfreetype.so.6` missing on a bare system | linuxdeploy's exclude list never bundles FreeType | FreeType 2.14.1 built static, optional codecs off | same: static FreeType from source |
| The catalog sees no window | SDL's X11 `WM_NAME` is `UTF8_STRING` under a UTF-8 locale, which `xdotool` cannot match | SDL from the fork branch `psxport/release-3.4.20` (release-3.4.20 plus the ICCCM `WM_NAME` commit) | the same commit on `xmen2/release-3.4.16` (xmen2 issue 198) |
| No `.zsync` beside the AppImage | appimagetool writes it into its working directory, and only when `zsyncmake` exists | run appimagetool from the output directory; `zsync` in the image | not hit: its packager runs from the output's parent |
| Zero-argument run must open a window | a missing disc must not exit | packaged selector and one-title runs show the first-run screen | the shared setup-ui screen, also after xmen2 issue 137 (the AppImage read the project `.env`) |

## Open
- Name consistency: the catalog compares the repository, AppImage, README title and desktop `Name=`; the
  repositories are `psx-<title>` while the packages are named after the game.
- macOS and Android packages do not exist (`project-state.md` S021); Windows is `docs/windows.md`.
- clang-20 and libc++ 20 come from apt.llvm.org, which is neither pinned nor checksummed; the pinned
  alternative is LLVM's 2 GB `LLVM-<version>-Linux-X64.tar.xz` release.
