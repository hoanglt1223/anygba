# Input mapping

A game looks for `anygba-input.ini` next to its executable, then in the per-user data folder
(`%APPDATA%\anyport\anygba\`, `~/.local/share/anyport/anygba/`, or
`~/Library/Application Support/anyport/anygba/`). `--input FILE` names a file explicitly. Without a
file the defaults below apply.

The menu's Controls page (Escape, then *Controls*) rebinds buttons and hotkeys in the game and writes the
file it was loaded from, or `anygba-input.ini` in the per-user data folder when there was none.

Write the current mapping to a file to start from:

```sh
<Title> --write-input anygba-input.ini
```

## Format

```ini
# Lines starting with # or ; are comments.
[keyboard]
A = Z
B = X
Select = Backspace
Start = Return
Right = Right
Left = Left
Up = Up
Down = Down
R = S
L = A
# SaveState2-9 and LoadState2-9 follow the same pattern up to F9.
SaveState1 = Shift+F1
LoadState1 = F1
SpeedUp = =
SpeedDown = -
Menu = Escape
FastForward = Space
CheatButton = F12
AiControl = F10
Journal = F11
Stats = Ctrl+F

[gamepad]
A = South
B = East
Select = Back
Start = Start
Right = DpadRight
Left = DpadLeft
Up = DpadUp
Down = DpadDown
R = RightShoulder
L = LeftShoulder
SaveState1 = Back+RightShoulder
LoadState1 = Back+LeftShoulder
SpeedUp = Back+DpadUp
SpeedDown = Back+DpadDown
Menu = Back+Start
FastForward = RightTrigger

[player]
# Starting speed: 100, 200, 300 or 400.
Speed = 100
# Speed while FastForward is held: 150, 200, 300, 400, 500, 800 or unlimited (no sound).
FastForwardSpeed = 300
# The stats overlay (Stats hotkey) is shown: on or off.
Stats = off
# Left stick travel ignored before it presses the d-pad (0-32767).
StickDeadzone = 12000
```

- Names are the GBA buttons `A`, `B`, `Select`, `Start`, `Right`, `Left`, `Up`, `Down`, `R`, `L` and the
  hotkeys `SaveState1`-`SaveState9`, `LoadState1`-`LoadState9`, `SpeedUp`, `SpeedDown`, `Menu`,
  `FastForward` (held), `CheatButton` (held; the cheat device's button that some codes test), `AiControl`
  and `Journal` (switch AI agent control and the play journal on and off, [AI-CONTROL.md](AI-CONTROL.md)),
  and `Stats` (the stats overlay, [USAGE.md](USAGE.md)).
- A value is a comma-separated list: `A = X, K`. An empty value unbinds the name for that device.
- A name you do not mention keeps its default, so a file can hold just the lines you change.
- Keyboard names are SDL key names: letters and digits, `Return`, `Space`, `Backspace`, `Tab`,
  `Left Shift`, `Right Ctrl`, `Up`/`Down`/`Left`/`Right`, `Keypad 8`, `F1`... (the names SDL's
  `SDL_GetScancodeFromName` accepts). A hotkey may add `Ctrl+`, `Alt+` and `Shift+`; it then fires only
  with exactly those modifiers held, so `F1` and `Shift+F1` stay apart. GBA buttons ignore modifiers.
- Gamepad names are positional (SDL gamepad layout): `South`, `East`, `West`, `North`, `Back`, `Guide`,
  `Start`, `LeftStick`, `RightStick`, `LeftShoulder`, `RightShoulder`, `DpadUp`, `DpadDown`,
  `DpadLeft`, `DpadRight`, `LeftTrigger`, `RightTrigger`. Every connected gamepad uses the same mapping.
  A gamepad hotkey can be a chord of buttons held together (`Back+Start`); while a chord is held, its
  buttons do not also reach the game. The left stick presses the d-pad past `StickDeadzone`.
- Controllers SDL does not recognise can be described in a `gamecontrollerdb.txt` next to the program or
  in the `SDL_GAMECONTROLLERCONFIG` environment variable.

A misspelled section, name, key or gamepad button stops the program with the file name and line, so a
typo cannot silently leave a button unbound.
