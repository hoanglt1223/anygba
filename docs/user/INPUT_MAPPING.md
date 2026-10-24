# Input mapping

A game looks for `anygba-input.ini` next to its executable, then in the per-user data folder
(`%APPDATA%\anyport\anygba\`, `~/.local/share/anyport/anygba/`, or
`~/Library/Application Support/anyport/anygba/`). `--input FILE` names a file explicitly. Without a
file the defaults below apply.

Write the current mapping to a file to start from:

```sh
<Title> --write-input anygba-input.ini
```

## Format

```ini
# Lines starting with # or ; are comments.
[keyboard]
A = X
B = Z
Select = Backspace
Start = Return
Right = Right
Left = Left
Up = Up
Down = Down
R = S
L = A
FastForward = Space

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
```

- Keys are the GBA buttons `A`, `B`, `Select`, `Start`, `Right`, `Left`, `Up`, `Down`, `R`, `L`, plus
  `FastForward` (keyboard only).
- A value is a comma-separated list: `A = X, Space`. An empty value unbinds the button for that device.
- A button you do not mention keeps its default, so a file can hold just the lines you change.
- Keyboard names are SDL key names: letters and digits, `Return`, `Space`, `Backspace`, `Tab`,
  `Left Shift`, `Right Ctrl`, `Up`/`Down`/`Left`/`Right`, `Keypad 8`, `F1`... (the names SDL's
  `SDL_GetScancodeFromName` accepts).
- Gamepad names are positional (SDL gamepad layout): `South`, `East`, `West`, `North`, `Back`, `Guide`,
  `Start`, `LeftStick`, `RightStick`, `LeftShoulder`, `RightShoulder`, `DpadUp`, `DpadDown`,
  `DpadLeft`, `DpadRight`, `LeftTrigger`, `RightTrigger`. Every connected gamepad uses the same mapping.

A misspelled section, button, key or gamepad name stops the program with the file name and line, so a
typo cannot silently leave a button unbound.
