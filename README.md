# FocusBird

FocusBird is a small, child-friendly Flappy Bird-inspired reaction game. It is a
desktop **C++17 + Raylib** application with optional **Arduino/Grove** controls.
The player guides a bird through gates while answering sound and covered-light
prompts. The game records gates passed, correct answers,
wrong responses, missed prompts, and play time.

The game has no external art or sound-asset dependency: its graphics are drawn
with Raylib primitives. It can be played with keyboard controls even when the
Arduino is not connected, which is useful for development and demonstrations.

## Features

- Side-scrolling game with bird physics, obstacle gaps, and collision detection.
- Two prompt types: **YELL** and **COVER THE LIGHT**.
- Correct prompt answers mark that gate green and play a positive buzzer tone.
- Wrong answers, missed prompts, ground/ceiling contact, and contact with any
  gate (including a green one) use a three-life allowance.
- Easy and Challenge presets, with different obstacle speed and gap size.
- Meadow, Sunset, and Night background themes, selectable on the title screen.
- Five bird colors, selectable on the title screen.
- HUD shows score, remaining lives, correct responses, current prompt, average
  correct-response time, and whether the Arduino is connected.
- Results screen reports correct, incorrect, and missed prompts separately, as
  well as average and fastest correct reaction time.
- Non-blocking serial communications: loss of the Arduino connection does not
  stop the game; the selected port reconnects automatically.
- Keyboard fallback for flight (`Space`/`Up`), sound (`Y`), and light (`C`).
- Grove button starts/restarts a run and flaps during play; it never answers a prompt.

## Quick start

You install a few tools once, then **one command** builds the game, uploads the
Grove firmware, finds the board's port, and starts playing. No Grove kit? Add
`--keyboard` (or answer **Y** when it offers keyboard mode) and play with the
keyboard.

### Windows 10/11

1. Install the tools. Open **PowerShell** and paste this (each line is one
   install; accept the prompts):

   ```powershell
   winget install Python.Python.3.12
   winget install Kitware.CMake
   winget install ArduinoSA.CLI
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"
   ```

   Close and reopen PowerShell afterwards so the new commands are found.
   If you already have Arduino IDE 2, you can skip `ArduinoSA.CLI`; the
   launcher uses the copy bundled with the IDE.
2. Plug in the Grove board. If it doesn't appear under **Ports (COM & LPT)** in
   Device Manager, or it shows as `USB-SERIAL CH340` with a warning icon
   (common for Grove Beginner Kit / Seeeduino boards), install the CH340
   driver from WCH's website and replug the board.
3. **Double-click `run-focusbird.cmd`** in the project folder, or run it from
   PowerShell:

   ```powershell
   .\run-focusbird.cmd
   .\run-focusbird.cmd --keyboard      # no Arduino
   ```

The first run downloads Raylib and the Arduino AVR core, so it takes a few
minutes and needs internet. Later runs take seconds.

### macOS

1. Install the tools ([Homebrew](https://brew.sh) required):

   ```sh
   xcode-select --install
   brew install cmake python arduino-cli
   ```

2. Plug in the board, then **double-click `run-focusbird.command`** in Finder
   or run it from Terminal:

   ```sh
   ./run-focusbird.py
   ./run-focusbird.py --keyboard   # no Arduino
   ```

   If macOS refuses to open the `.command` file, right-click it and choose
   **Open** once.

### Linux

1. Install the compiler, CMake, and Raylib's X11/audio headers:

   ```sh
   # Ubuntu / Debian / Mint
   sudo apt install build-essential cmake git python3 libasound2-dev libx11-dev \
     libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev

   # Fedora
   sudo dnf install gcc-c++ cmake git python3 alsa-lib-devel libX11-devel \
     libXrandr-devel libXi-devel libXcursor-devel libXinerama-devel mesa-libGL-devel

   # Arch
   sudo pacman -S --needed base-devel cmake git python alsa-lib mesa libx11 \
     libxrandr libxi libxcursor libxinerama arduino-cli
   ```

2. Install Arduino CLI if your distro doesn't package it (Arch already did
   above):

   ```sh
   curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
   ```

3. Allow your user to open serial ports, **then log out and back in**:

   ```sh
   sudo usermod -aG dialout "$USER"   # Ubuntu/Debian/Fedora
   sudo usermod -aG uucp "$USER"      # Arch
   ```

4. Run it:

   ```sh
   ./run-focusbird.py
   ./run-focusbird.py --keyboard   # no Arduino
   ```

Don't run the game with `sudo`; fix the group membership instead.

### Launcher options

| Option | What it does |
| --- | --- |
| *(none)* | Build, upload firmware to the detected board, play with the Grove kit |
| `--keyboard` | Build and play with the keyboard only; no Arduino tools needed |
| `--no-upload` | Skip compiling/uploading the sketch (the board already has it) — faster |
| `--port PORT` | Use a specific port, e.g. `COM4`, `/dev/ttyACM0`, `/dev/cu.usbmodem1101` |
| `--fqbn ID` | Board type if it isn't an Uno-compatible board (`arduino-cli board listall`) |

`FOCUSBIRD_PORT`, `FOCUSBIRD_FQBN`, and `FOCUSBIRD_BUILD_DIR` environment
variables set the same things. The launcher ignores ports that can't be the
Grove board (such as Windows' built-in `COM1` and Bluetooth serial ports); if
several USB boards are still plugged in, it asks which one to use.

**Before launching, close Arduino IDE's Serial Monitor and any FocusBird window
that's still open.** Only one program can use the serial port at a time, and a
busy port is the most common reason an upload fails.

## Project layout

```text
.
├── CMakeLists.txt                # C++17 build and Raylib dependency
├── README.md                     # Setup, controls, wiring, protocol, and testing
├── run-focusbird.py              # Build, upload Grove firmware, and launch the game
├── run-focusbird.cmd             # Windows double-click wrapper for the launcher
├── run-focusbird.command         # macOS double-click wrapper for the launcher
├── include/serial_port.h         # Cross-platform non-blocking serial interface
├── src/main.cpp                  # Raylib game, prompt state, physics, and UI
├── src/serial_port.cpp           # Windows/POSIX serial implementation
└── arduino/focusbird/focusbird.ino # Grove sensor and buzzer firmware
```

## Building by hand (optional)

The launcher just runs these commands; use them directly if you prefer an IDE
or want to debug.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The executable is `build/focusbird` (macOS/Linux), `build\Release\focusbird.exe`
(Windows, Visual Studio), or `build\focusbird.exe` (Windows, MinGW). Start it with
`--serial PORT` to use the Grove kit, or with no arguments for keyboard mode.
On Windows you can also open the folder in Visual Studio 2022 (**File > Open >
Folder**), which understands `CMakeLists.txt` directly.

CMake uses an installed Raylib 5.x if it finds one; otherwise it downloads
Raylib 5.5 on the first configure (using Git if installed, otherwise a source
archive). To build offline, install Raylib 5.x with your package manager first.

## Keyboard controls

| Key | Action | Prompt answered |
| --- | --- | --- |
| `Enter` | Start / play again | — |
| `1` | Select Easy on title screen | — |
| `2` | Select Challenge on title screen | — |
| `T` | Cycle background theme on title screen | — |
| `B` | Cycle bird color on title screen | — |
| Grove button | Start/restart; flap during play | — |
| `Space` or `Up` | Flap only | — |
| `Y` | Simulate a sound sensor event | Yell prompt |
| `C` | Simulate covering the light sensor | Cover prompt |
| `Esc` | Quit | — |

An input only answers the **currently active** matching prompt. An early event,
late event, or unrelated sensor event is not counted as the requested response.
The Grove button and `Space`/`Up` are flight controls only. They do not answer or
fail a YELL or COVER prompt; only the sound and light sensors (or `Y`/`C` keyboard
fallbacks) affect those prompts.

## Arduino hardware setup

Connect the Grove components to the pins configured at the top of
`arduino/focusbird/focusbird.ino`:

| Component | Default pin | Role |
| --- | --- | --- |
| Grove button | D2 | Start/restart and flap control; module pull-down, pressed is HIGH |
| Grove buzzer | D3 | Prompt cue and correct/incorrect response feedback |
| Grove sound sensor | A0 | Raw reading above 800 for three samples emits `SOUND` |
| Grove light sensor | A1 | Raw reading at or below 10% of ADC full scale emits `LIGHT` |

Pin numbers vary by Grove base shield, board, and wiring. Confirm the labels on
your board before connecting anything, and change the constants in the sketch
if your setup differs. Use only sensor/output voltage levels supported by the
selected Arduino board.

This sketch uses the Grove button, sound sensor, light sensor, and buzzer. It
does not depend on a particular Arduino Uno revision; select the board that
matches your Grove base shield and hardware.
The standard Grove button module has an onboard pull-down resistor and reads
HIGH when pressed, so the sketch configures D2 as a normal input (not
`INPUT_PULLUP`).

### Uploading the firmware by hand (optional)

The launcher uploads the sketch automatically. To do it yourself, open
`arduino/focusbird/focusbird.ino` in Arduino IDE 2, pick the board and port,
and click **Upload**, or use Arduino CLI:

```sh
arduino-cli core install arduino:avr
arduino-cli board list                   # shows the port, e.g. COM4 or /dev/ttyACM0
arduino-cli compile --fqbn arduino:avr:uno arduino/focusbird
arduino-cli upload -p PORT --fqbn arduino:avr:uno arduino/focusbird
```

### Verify and tune the sensors

The firmware prints raw readings and their cutoffs every half second at **115200 baud**:

```text
SENSORS <soundRaw> <lightRaw> <soundThreshold> <lightThreshold>
```

To inspect it, close FocusBird, open Arduino Serial Monitor (or run
`arduino-cli monitor -p PORT --config 115200`), then reset the board. Use
`COM3` on Windows, `/dev/cu.usbmodem...` on macOS, or `/dev/ttyACM0` on Linux.
When finished, close the monitor before launching FocusBird; only one app can
normally own the serial port at a time.

These are absolute raw ADC thresholds; no sensor baseline or delta is used:

- `SOUND_RAW_THRESHOLD` (default `800`): raw sound readings must be **above 800**
  for three consecutive samples to emit `SOUND`. A burst emits once and rearms
  only after the raw value stays at or below 700.
- `LIGHT_RAW_THRESHOLD` (default `102`): with the Uno's 10-bit ADC maximum of
  `1023`, 10% full scale is `102`. Raw light readings at or below this value for
  three samples emit `LIGHT`.
- `LIGHT_RAW_RELEASE_THRESHOLD` (default `122`): the light sensor must remain
  above 12% full scale to rearm after a cover.
- `ANALOG_MAX_VALUE` (default `1023`): set this to the maximum value returned by
  `analogRead()` if using a board with a different ADC range.
- `BUTTON_DEBOUNCE_MS`: rejects rapid electrical transitions from the button.

The `SENSORS` line shows exactly what the game is comparing. Check that the
analog Grove Light Sensor is plugged into **A1**, not an I2C/digital light
module, and that its cable is seated. Covering should bring `lightRaw` to
`lightThreshold` or lower; if the raw reading never gets that low, check the
sensor, cable, A1 port, and `ANALOG_MAX_VALUE`. The firmware emits one `LIGHT`
event per cover and rearms after the reading rises above the release threshold.

## Connect the game to the Arduino

The launcher passes the detected port automatically. To start the built game
yourself, pass the port shown by `arduino-cli board list`:

```sh
./build/focusbird --serial /dev/ttyACM0          # Linux
./build/focusbird --serial /dev/cu.usbmodem1101  # macOS
.\build\Release\focusbird.exe --serial COM4      # Windows
```

If the port can't be opened, FocusBird prints a warning, keeps keyboard
controls working, and retries every two seconds. The title screen and HUD show
**SEARCHING FOR GROVE KIT** until the port opens, then **GROVE KIT CONNECTED**.
If the board is unplugged mid-game, play continues and it reconnects when the
same port comes back.

## Serial protocol

The connection is **115200 baud, 8 data bits, no parity, 1 stop bit (8-N-1)**.
Each message is a newline-terminated ASCII line:

| Direction | Message | Meaning |
| --- | --- | --- |
| Arduino → game | `BTN` | Debounced button press; starts/restarts on menus and flaps during play |
| Arduino → game | `SOUND` | Confirmed sound burst crossed the configured threshold |
| Arduino → game | `LIGHT` | Light sensor transitioned to covered |
| Arduino → game | `SENSORS ...` | Raw sensor readings and cutoffs, printed twice per second |
| Game → Arduino | `BUZZER 900 70` | Play a 900 Hz prompt cue for 70 ms |
| Game → Arduino | `BUZZER 1350 100` | Short positive feedback tone |
| Game → Arduino | `BUZZER 380 150` | Lower-pitched incorrect-action feedback |

The game timestamps a response when its complete line is received. Unknown
lines, including `READY` and `SENSORS ...`, are ignored.
Incoming reads are non-blocking and tolerate partial USB packets until a newline
arrives.

Reaction time is measured from prompt activation to receipt of a correct input.
The results screen reports average and fastest correct response times; incorrect
and missed prompts are tracked separately.

## Gameplay and scoring

- The game starts with three lives.
- Each gate is assigned one randomly selected prompt.
- A prompt becomes active while its gate is within the configured lead distance
  of the bird, and ends when the gate passes the bird.
- A correct answer records a success and marks that gate green, but does not
  prevent a collision penalty if the bird touches a gate column.
- Correct answers record reaction time from prompt activation to input receipt.
- The gate passing the bird increments the score.
- A wrong action during an active prompt closes that prompt, records an incorrect
  response, and costs one life.
- Button presses during sound/light prompts remain flap controls and are not
  treated as incorrect prompt answers.
- Letting a prompt expire records a miss and costs one life.
- Contact with any gate or the top/bottom playfield boundary costs a life.
- The game ends when no lives remain. The result screen keeps wrong and missed
  prompts separate so the run can be evaluated.

The Easy preset uses slower gates and wider openings. Challenge uses faster gates
and narrower openings. Gates are spaced farther apart than the prompt lead
distance to avoid overlapping response windows. The physics step uses elapsed
frame time, with a safety clamp after long frame stalls.

## Test checklist

### Desktop-only checks

1. Build and launch without `--serial`; verify the HUD says **KEYBOARD MODE**.
2. Start in both difficulties and check the bird flaps with Space/Up.
3. Answer prompts with `Y` or `C`; verify the correct count increases and the
   gate changes to its safe color. `Space`/`Up` should only flap.
4. Use an incorrect key during a prompt; verify incorrect count and lives change.
5. Do not answer a prompt; verify the miss count increases after the gate passes.
6. Hit both green and unanswered gate columns, plus the screen boundaries;
   verify each collision reduces lives and the game-over summary appears at zero.
7. Restart from the results screen; verify statistics reset and difficulty stays
   selected.

### Hardware and integration checks

1. Upload the firmware and confirm the board appears as a serial device.
2. Launch the game with the device path and check **GROVE KIT CONNECTED** (or
   **SEARCHING FOR GROVE KIT** while the port is unavailable).
3. Press the Grove button on the title screen to start. During play it only
   flaps; verify button presses do not answer or penalize either sensor prompt.
4. Use Serial Monitor to verify `soundRaw` rises above 800 when yelling and
   `lightRaw` falls to 10% full scale or lower when covered. Close the monitor,
   then test both prompts in-game.
5. Make a deliberate sound during a YELL prompt and cover the light sensor during
   a COVER prompt; verify each is accepted only during its matching window.
6. Cover the light sensor before a COVER prompt, then uncover it. Confirm the
   early event does not answer the later prompt and a new cover transition does.
7. Hold the button and keep the light sensor covered; verify neither produces a
   stream of repeated input events.
8. Unplug the Arduino during a run; verify the game continues in keyboard mode.
9. Check buzzer cues and tune thresholds in the actual room.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| `cmake` / `py` / `arduino-cli` not recognized (Windows) | Close and reopen PowerShell after `winget install` so PATH updates. |
| Windows: "No CMAKE_CXX_COMPILER could be found" | Install the Visual Studio Build Tools line from the quick start, then delete the `build` folder and rerun. |
| Windows: board not found / no COM port | Try another USB cable (many are charge-only). If Device Manager shows `USB-SERIAL CH340` with a warning icon, install the CH340 driver. |
| Upload fails with "Access is denied" / "port busy" | Close Arduino Serial Monitor/IDE and any FocusBird window, then rerun. |
| Game shows SEARCHING FOR GROVE KIT forever | Same as above: another program holds the port, or the wrong `--port` was chosen. Check `arduino-cli board list`. |
| CMake cannot download Raylib | The first build needs internet access to GitHub, or install Raylib 5.x with your package manager. |
| Linux: "RandR headers not found" or similar | Install the Linux packages from the quick start, delete `build`, and rerun. |
| Linux: permission denied on `/dev/ttyACM0` | Add yourself to `dialout` (or `uucp` on Arch) and log out/in. |
| Sound fires in room noise | Inspect `soundRaw`; it must be above 800 for three samples. Check the sound sensor and A0 connection. |
| Sound prompt does not trigger | Inspect `soundRaw` while yelling. If it does not exceed 800, lower `SOUND_RAW_THRESHOLD` and verify the sensor is on A0. |
| Light prompt does not trigger | Confirm the analog Grove Light Sensor is plugged into A1 and `lightRaw` reaches `lightThreshold` or lower when covered. Check `ANALOG_MAX_VALUE` for the board's ADC range. |
| Light triggers without covering | The raw light value is reaching 10% full scale; raise `LIGHT_RAW_THRESHOLD` if you need a less sensitive cutoff. |
| One physical action triggers more than once | Increase the sound cooldown or button debounce; verify the light hysteresis and release behavior. |
| Buzzer does not sound | Check the configured buzzer pin, module polarity/type, and whether that board supports `tone()` on the selected pin. |
| Window opens but keyboard input seems ignored | Start a run with Enter; Y and C only answer a prompt while its gate is in range. |

## Implementation notes

- Game physics, state, rendering, prompt rules, and keyboard fallback are in
  `src/main.cpp`.
- `SerialPort` in `include/serial_port.h` and `src/serial_port.cpp` isolates
  operating-system serial APIs from game logic. Windows and POSIX paths are
  implemented; POSIX includes Linux and macOS device paths.
- Arduino code is kept separate from the desktop CMake target and can be built
  with Arduino IDE/CLI.
- All game visuals are generated at runtime with Raylib drawing primitives.
- Sensor thresholds are starting points, not calibrated guarantees; tune and
  verify them using the actual modules and room conditions.
