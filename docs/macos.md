# macOS release

Owns the x86-64 macOS app of every title: the target libraries, MoltenVK, the bundle and its audits. Rules it
meets are the `release` skill's; `docs/windows.md` is the sibling platform and shares the target-platform
owner, and `docs/appimage.md` the title identity, version and content audit (`tools/port/release_package.py`).

| piece | role |
|---|---|
| `tools/port/target_platform.py` | macOS deployment target 13.3 (std::format of floating point), the target libraries and Lightning built once per pin, MoltenVK v1.4.2 (checksummed) |
| `tools/port/target_libraries.py`, `target-libraries.json` | SDL3 (fork), SDL3_image (stb backend) and FreeType, static |
| `tools/port/consumer_build.py --platform macos` | the title's build in `build/macos`, natively on an Intel Mac |
| `tools/port/macos_package.py` | `<Name>-<version>-macos-x86_64.zip` holding `<Name>.app`, signed ad hoc |
| `.github/workflows/title-release.yml` `macos` | the same build and package on `macos-15-intel`, uploaded to the title's release |

## Inputs and outputs
In: a clean title commit, its desktop entry (`Name=`, app id, SVG icon) and licence, the Lightrec and
Lightning pins in `cmake/lightrec_dependency.cmake`. Out:
`build/package/macos/out/<Name>-<version>-macos-x86_64.zip`.

## Invariants
- No game content: the shared content audit refuses disc images, PS-X EXEs and manifest-named files.
- The executable links only `/System/Library/` and `/usr/lib/`; the libraries are static.
- `Contents/Resources/assets` beside `Contents/MacOS` makes the app a package: it defaults to a window and
  reads those assets (`psx::config::Config`).
- SDL_GPU's Vulkan backend runs on the bundled `Contents/Frameworks/libMoltenVK.dylib`
  (`RenderDevice::useBundledMoltenVK`); psxport's shaders are SPIR-V only, so Metal is not a backend.
  MoltenVK's Apache-2.0 licence ships as `Resources/MoltenVK-LICENSE`.
- `--wrap=memcpy` is off: ld64 has no `--wrap` (`PSXPORT_WRAP_MEMCPY`).
- Lightrec on macOS x86-64 is qualified by Lightrec's own CI job on the Intel runner; Apple silicon runs the
  x86-64 app under Rosetta.

## Check
On an Intel Mac or the `macos-15-intel` runner, from a title root:
`consumer_build.py --platform macos --target <product>`, then `macos_package.py --binary
build/macos/bin/<product>`. The run prints the link and content audits and verifies the signature.

## Open
- Not yet run on a Mac; the hosted build and audits are the only evidence.
- Ad-hoc signature: Gatekeeper asks the player to allow the app on first open.
- No arm64 build: Lightrec is not qualified on Apple silicon.
