# AnyGBA on Android

AnyGBA builds a GBA ROM you own into an Android app on your PC; the phone needs no compiler. There are
two ways to ship a game: one APK per game, or game packages for the AnyGBA launcher app. Both are
sideloaded; nothing here is for Google Play (the apps contain your ROM).

## What you need on the PC

- The AnyGBA package for your PC plus the Android runtime (`anygba-<version>-android-runtime.tar.gz`
  from a release, extracted into the same folder; it adds `android/<abi>/`). From source:
  `cmake --install <out> --prefix <prefix>` then
  `cmake -DPREFIX=<prefix> -P platform/android/build-runtime.cmake` (Ninja and an NDK required).
- An Android SDK with `platforms;android-35` (or newer), a `build-tools` version with `apksigner`, and an
  NDK, e.g. `sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;27.0.12077973"`. AnyGBA finds it
  through `--android-sdk`, `ANDROID_HOME`, `ANDROID_SDK_ROOT` or the usual per-user folder, and the NDK
  through `--android-ndk`, `ANDROID_NDK_HOME` or the newest `<sdk>/ndk/*`.
- JDK 17 or newer (`JAVA_HOME` or on `PATH`), CMake 3.28+ and Ninja.

Gradle is not used: the APK is assembled directly with aapt2, javac, d8, zipalign and apksigner.

## One APK per game

```sh
anygba build game.gba --target android [-o dist] [--icon icon.png] [--abi arm64-v8a,x86_64]
```

writes `dist/<Title>.apk` for arm64 phones (and x86_64 emulators), with the ROM inside. Install it with
`adb install -r dist/<Title>.apk` or by opening the file on the phone.

| Option | Meaning |
|---|---|
| `--abi LIST` | ABIs to compile (default `arm64-v8a,x86_64`) |
| `--package-id ID` | Android application id (default `io.anyport.anygba.g<first 12 hex digits of the ROM SHA-256>`) |
| `--icon FILE.png` | launcher icon, 192x192 recommended (default: a generic handheld) |
| `--name NAME` | app name (default: the title from the ROM header or config) |
| `--keystore FILE`, `--keystore-pass-env VAR`, `--key-alias NAME` | sign with your own key; the password is read from the environment variable |
| `--version-code N` | Android versionCode (default: minutes since 2024, so each rebuild installs as an update) |

Without `--keystore` the APK is signed with the Android debug key (`~/.android/debug.keystore`, created if
missing). Keep using the same key and package id, or Android refuses the update and you would have to
uninstall the app, which deletes its saves.

## The launcher app and game packages

```sh
anygba android-launcher -o dist                        # once: dist/AnyGBA-Launcher.apk
anygba build game.gba --target android-package -o dist  # per game: dist/<Title>.agp
```

Install the launcher, copy `.agp` files to the phone, and use **Import game package** (or open the file
with AnyGBA). The list shows each game's cover (the `--icon`), title and when it was last played. Tap to
play; long-press for per-game settings (starting speed) or to remove the game (saves are kept).

## Playing

- **Touch controls** appear on a touchscreen: d-pad (eight directions; slide your thumb), A, B, L, R,
  Start, Select and `>>` (fast-forward while held). In landscape they sit over the sides of the
  picture; in portrait the picture is at the top and the controls below it. Notches and rounded corners
  are kept clear.
- **The floating button** (the round button at the screen edge) opens the quick menu; the game waits
  while it is open. Drag the button anywhere: it settles against the nearest edge and remembers its
  place. It dims after a few seconds untouched. **Hold it** to quick-save to the current slot.
- **Quick menu**: Save (current slot), Load (pick a slot 1-9; filled slots say "saved"), Fast
  (fast-forward on/off), the fast-forward speed (tap to step through 150%-unlimited), Cheats (the cheat
  tree), Shot (screenshot to the gallery, `Pictures/AnyGBA`), Filter (none, LCD grid, scanlines, CRT),
  Wide (off, 16:10, 16:9), Layout (move and resize the touch buttons), Hide/Show pad, Menu (the full
  in-game menu), Setup (the touch settings) and Exit (writes the save and closes the game). Tap outside
  the menu to go back to the game.
- **Layout editor**: drag any button to move it. Tap one to select it, then Smaller/Bigger resize it and
  Reset puts it back; with nothing selected they apply to all buttons. Done saves the layout.
- The **in-game menu** (Menu in the quick menu) is the same as on the desktop
  ([USAGE.md](USAGE.md#menu)): tap a tab or a row, tap the arrows of a value, drag to scroll, tap outside
  it to close it. Rows are taller for fingers. Typing a cheat code or a search needs a keyboard; cheats
  from the database work by touch. The quick menu follows the menu's language and theme.
- **Touch settings** (Setup): size, opacity, position, vibration, the floating button, and save export
  and import. Changes apply while you play and are stored in `anygba-touch.ini` with the button's place
  and the layout. Turn the floating button off there to get the classic MENU button back (tap: in-game
  menu, hold for a second: touch settings).
- **Gamepads** (Bluetooth or USB) work out of the box; the touch controls hide while one is used and come
  back when you touch the screen. Remap buttons in MENU > Controls as on the desktop.
- The game runs full screen and turns with the phone.

## Saves

Saves, save states and cheat choices live in the app's private storage (`files/saves/<title>-<hash>.*`).
The battery save is written whenever the game changes it and again when the app goes to the background,
so switching apps or the system closing AnyGBA loses nothing the game had saved.

- **Export**: Setup in the quick menu (or hold MENU), then Export saves. The share sheet sends the
  `.sav`, `.ss1`-`.ss9` and `.cheats.toml` files anywhere (Drive, mail, Files).
- **Import**: Setup in the quick menu (or hold MENU), then Import saves and pick files. A `.sav` from the
  desktop build or from a backup works; the file name does not matter, only the extension. The game
  closes afterwards; open it again to use the imported saves.

Uninstalling a per-game APK deletes its saves; export them first.

## Testing in the emulator

```sh
adb shell am start -n <package>/io.anyport.anygba.GameActivity --esa args --headless,--frames,300,--dump-frame-hash
adb logcat -s anygba
```

`args` takes the same options as the desktop program; the output goes to logcat under the tag `anygba`.
`platform/android/emulator-smoke.sh` is the CI check that compares this hash with the desktop build.
