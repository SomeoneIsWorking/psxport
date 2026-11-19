# macOS release

Owns the macOS app of every title: the target libraries, MoltenVK, the bundle and its audits. Rules it
meets are the `release` skill's; `docs/windows.md` is the sibling platform and shares the target-platform
owner, and `docs/appimage.md` the title identity, version and content audit (`tools/port/release_package.py`).

| piece | role |
|---|---|
| `vendor/target-platform` (shared `target-platform` repo) | macOS deployment target 13.3 (std::format of floating point), SDL3 (fork), SDL3_image (stb backend) and FreeType, static, built once per pin; MoltenVK v1.4.2 (checksummed) |
| `tools/port/release_platform.py` | that provisioning under psxport's dependency root |
| `tools/port/consumer_build.py --platform macos` | the title's build in `build/macos`, natively for the Mac's own architecture (arm64 on Apple silicon) |
| `tools/port/macos_package.py` | `<Name>-<version>-macos-<arch>.zip` (`arm64` or `x86_64`) holding `<Name>.app`, signed ad hoc |
| `.github/workflows/title-release.yml` `macos` | the same build and package on `macos-15-intel` (so `x86_64`), uploaded to the title's release |

## Inputs and outputs
In: a clean title commit, its desktop entry (`Name=`, app id, SVG icon) and licence. Out:
`build/package/macos/out/<Name>-<version>-macos-<arch>.zip`, where `<arch>` is the building Mac's.

## Invariants
- No game content: the shared content audit refuses disc images, PS-X EXEs and manifest-named files.
- The executable links only `/System/Library/` and `/usr/lib/`; the libraries are static.
- `Contents/Resources/assets` beside `Contents/MacOS` makes the app a package: it opens fullscreen and
  reads those assets (`psx::config::Config`).
- `tools/build.py --platform macos` leaves out the mipsel test program (`PSXPORT_TESTPROG=OFF`): it is a
  host check, and AppleClang has no MIPS backend.
- SDL_GPU's Vulkan backend runs on the bundled `Contents/Frameworks/libMoltenVK.dylib`
  (`RenderDevice::useBundledMoltenVK`); psxport's shaders are SPIR-V only, so Metal is not a backend.
  MoltenVK's Apache-2.0 licence ships as `Resources/MoltenVK-LICENSE`.
- `--wrap=memcpy` is off: ld64 has no `--wrap` (`PSXPORT_WRAP_MEMCPY`), so `memcensus.cpp` compiles no
  `__wrap_*` and `PSXPORT_MEMCENSUS` only warns.
- The app is built for the Mac that builds it. An Apple-silicon Mac makes an arm64 app; distributing an
  Intel build needs an Intel Mac (or the `macos-15-intel` runner).

## Check
On a Mac or the `macos-15-intel` runner, from a title root:
`consumer_build.py --platform macos --target <product>`, then `macos_package.py --binary
build/macos/bin/<product>`. The run prints the link and content audits and verifies the signature.

## Open
- Not yet run on a Mac; the hosted build and audits are the only evidence.
- Ad-hoc signature: Gatekeeper asks the player to allow the app on first open.
