# Android target: design notes

User guide: [../user/ANDROID.md](../user/ANDROID.md).

## Pieces

| Where | What |
|---|---|
| `platform/android/build-runtime.cmake` | NDK cross-build of the runtime, installed as `<prefix>/android/<abi>` (also installs SDL's Java classes to `share/anygba/android/sdl-java`) |
| `platform/android/{game,launcher}/AndroidManifest.xml`, `java/`, `res/` | APK template, installed to `<prefix>/share/anygba/android` |
| `tools/anygba/android.cpp` | `--target android`, `--target android-package`, `android-launcher` |
| `tools/anygba/zip.cpp` | stored-entry ZIP writer that also copies an existing archive (aapt2 output) |
| `runtime/src/player/touch.cpp` | touch layout and hit testing (unit tested, no SDL) |
| `runtime/src/player/assist.cpp` | floating menu button: edge snapping, tap/long-press/drag, quick menu and layout-editor geometry (unit tested, no SDL) |
| `runtime/src/player/touch_overlay.cpp` | SDL glue: finger events, drawing (controls, floating button, quick menu, layout editor), haptics, settings reload and write-back |
| `runtime/src/player/lifecycle.cpp` | writes the save on `SDL_EVENT_WILL_ENTER_BACKGROUND` / `TERMINATING` |
| `runtime/src/android/log_forward.cpp` | stdout/stderr to logcat |
| `platform::Window::set_overlay` | lets the overlay place the picture and draw on top |

## Floating button and quick menu

The overlay turns quick-menu taps into `TouchAction`s that `Player::handle_touch` carries out with the
same functions as the hotkeys and the in-game menu (`save_state`, `load_state`, `apply_video`,
`Menu::open_cheats`), and the player sends back a `TouchStatus` for the labels. While the quick menu or
the layout editor is open `TouchOverlay::paused()` is true and the player runs `run_touch_pause` (no
emulation, the last picture redrawn). Both the game and the Java settings dialog write
`anygba-touch.ini`: the game rewrites the whole file, the dialog merges its own keys into what is on
disk. Screenshots go to `files/screenshots/`; message `0x8002` makes `GameActivity` move the newest one
to `Pictures/AnyGBA` through MediaStore (Android 10+; older versions or a refusal fall back to the
app's external Pictures folder).

## APK assembly without Gradle

The generated C and the whole runtime link into one `libmain.so` per ABI (`WHOLE_ARCHIVE` so `SDL_main`
and SDL's `JNI_OnLoad` survive; 16 KB page alignment for Android 15 devices). The driver then runs
`aapt2 compile/link` (manifest, resources, the ROM as asset `game.gba`), `javac` (SDL's Java classes plus
`GameActivity`, `SaveProvider`, `TouchSettings`) and `d8`, adds `classes.dex` and the libraries stored
uncompressed (`extractNativeLibs="false"`), aligns with `zipalign -P 16` and signs with `apksigner`.
This needs only SDK command-line packages, works offline, and takes seconds; a Gradle project would add a
network download, a wrapper and a plugin version to keep current for no feature we use.

`GameActivity` copies the ROM asset to app storage on first start after an install or update and passes
its path as the first player argument, so `main.cpp` reads it like any ROM file. Player arguments can be
added with the intent extra `args` (string array).

## Launcher packaging decision

Options considered:

1. **Dynamic feature modules**: delivered only through Google Play, which AnyGBA cannot use (the apps
   contain ROMs). Rejected.
2. **One plugin APK per game, discovered by the launcher**: still one install per game, which is what the
   launcher is meant to avoid. Rejected.
3. **Game packages loaded with `dlopen` from app storage**: chosen.

A game package (`.agp`) is a ZIP with `package.properties` (name, title, ROM SHA-256, save stem),
`game.gba`, `cover.png`, `cheats/*.toml` and `lib/<abi>/libmain.so`. The library is the same one a
per-game APK carries, so there is one runtime build and one code path. The launcher unpacks a package
into `files/games/<sha>/` (whitelisted entry names, size limit, validated before it is moved into place)
and starts `GameActivity` in a separate `:game` process, which `System.load`s the package library instead
of the APK's. The process exits when the game ends, because SDL cannot initialise twice in one process.

Cost of this choice: Android forbids mapping code from app-writable storage for apps that target API 29
or later (W^X), so the launcher targets API 28 (`kLauncherTargetSdk`). Android 14+ installs it with a
note that it was built for an older version; sideloading is unaffected. Verified on an API 35 emulator:
the package library loads from `files/games/` and the frame hash matches the desktop build. Per-game
APKs target API 35. If a future Android drops support for target API 28, the fallback is option 2.

## Verified on the emulator (API 35, x86_64, headless)

- `tonc/obj_demo` frame 300 hash equals the desktop build (headless and windowed, per-game APK and
  launcher package).
- Holding the touch d-pad right with `adb shell input touchscreen swipe` gives the same frame as the
  desktop run with `--keys right:60-200`: the touch controls move the sprite.
- `gba-tests/save/sram`: passes, the save file survives `am force-stop` and a restart, and exists after
  HOME (background) followed by a kill.
- Portrait and landscape layouts, immersive full screen.

Not verifiable here: haptics and real gamepads (the emulator has neither), and real-device performance
(the emulator renders with SwiftShader).
