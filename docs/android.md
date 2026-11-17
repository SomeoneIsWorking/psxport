# Android

Owns the Android APK of every title: the arm64-v8a build, the Gradle project, the packaged assets, the
first-run disc and the touch controls. Rules it meets are the `release`, `android` and `port-release`
skills'; `docs/macos.md` is the sibling platform and shares the title identity and content audit
(`tools/port/release_package.py`). Platform mechanics come from the shared android-port.

| piece | role |
|---|---|
| `tools/port/target_platform.py` | NDK 28.2.13676358, API 24 (Vulkan), arm64-v8a, and android-port's native prefix (SDL3, SDL3_image, FreeType, fmt) built once per pin |
| `tools/port/consumer_build.py --platform android` | the title's build in `build/android`; the product is `bin/libmain.so`, whose `SDL_main` SDL's Activity calls (`cmake/psxport.cmake`, `product_main.cpp`) |
| `tools/port/android/` | the one Gradle project every title uses: AGP 9.2.1, Gradle 9.4.1, `PsxportActivity` over android-port's `AndroidActivity` |
| `tools/port/android_apk.py` | stages that project with the title's identity, android-port's runtime, the assets and the icons, and assembles the APK |
| `runtime/psx/input/touch_pad.*` | the on-screen pad (`docs/ui-architecture.md` Touch controls) |

## Inputs and outputs
In: `build/android/bin/libmain.so` and the title's desktop entry (`Name=` as the label, the app id as the
application id, the SVG icon as launcher icons at each density). Out:
`build/android/package/out/<Name>-<version>-android-arm64-v8a[-debug].apk`.

## Invariants
- No game content: the shared content audit runs over the packaged assets; the player's disc is picked on
  the device.
- `PsxportActivity` unpacks the APK's `psxport/` assets into `files/assets` once per installed build;
  `psx::config::Config` treats the files directory as the package (assets there, fullscreen, a shown window)
  and as Lucent's app-private root, under which each title keeps its own directory.
- First run: SDL's picker answers with a `content://` document, and libchdr reads a path, so
  `FirstRunSetup` copies the image into the title's directory (`disc.chd`) before judging it.
- Landscape only: SDL turns a resizable window's unset orientation into `FULL_USER`, overriding the manifest,
  so `RenderWindow::create` sets `SDL_HINT_ORIENTATIONS` to both landscapes.
- A default build is signed with the SDK's local debug key; `--release` needs the maintainer key in
  `PSXPORT_ANDROID_KEYSTORE`, `_KEY_ALIAS`, `_STORE_PASSWORD`, `_KEY_PASSWORD`.

## Check
From a title root: `consumer_build.py --platform android --build build/android --target <product>`, then
`android_apk.py`. Install on the shared emulator under its lock (android-port `with-emulator-lock`) and
screenshot with `adb exec-out screencap -p`. On a Linux host the AVD needs android-port's
`docs/android-emulator.md` recipe (`-gpu host` on a private display); SwiftShader crashes it.

## Open
- Launcher icons are legacy PNGs, not the adaptive icon the `android` skill describes.
- The disc copy runs on the setup screen's thread without progress.
- The RmlUi menu has no touch path: with the pad shown every finger is the pad's.
- No on-device performance evidence yet.
- The product's own data directory is named after `lucent::platform::executable_path()`, which on Android is
  the zygote's `app_process64`; titles' discs are unaffected (`files/<slug>/`).
