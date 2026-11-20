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

## Project layout

```text
.
├── CMakeLists.txt                # C++17 build and Raylib dependency
├── README.md                     # Setup, controls, wiring, protocol, and testing
├── include/serial_port.h         # Cross-platform non-blocking serial interface
├── src/main.cpp                  # Raylib game, prompt state, physics, and UI
├── src/serial_port.cpp           # Windows/POSIX serial implementation
└── arduino/focusbird/focusbird.ino # Grove sensor and buzzer firmware
```

## Requirements

- CMake **3.16 or newer**.
- A C++17 compiler (GCC, Clang, or MSVC).
- Git and an internet connection for the first CMake configure if Raylib is not
  installed on the system. CMake fetches the pinned Raylib 5.5 source then.
- Raylib's normal platform dependencies and a desktop environment.
- Optional hardware: an Arduino compatible with the Arduino IDE/CLI and Grove
  button, sound sensor, light sensor, and buzzer modules.

## Build the desktop game

### Windows

1. Install Visual Studio 2022 or Build Tools with **Desktop development with
   C++**, plus CMake and Git.
2. Open a Developer PowerShell in the project directory and run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\Release\focusbird.exe
```

Use `--serial COM3` when starting the game with the Grove board connected.

### macOS

1. Install the Xcode command-line tools and CMake:

```sh
xcode-select --install
brew install cmake
```

2. Build and run from the project directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/focusbird
```

Use a `/dev/cu.usbmodem...` port with `--serial` when connecting the Grove board.

### Linux

On Ubuntu or Debian, install the compiler and Raylib's common X11/audio
development dependencies:

```sh
sudo apt update
sudo apt install build-essential cmake git pkg-config libasound2-dev \
  libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
  libgl1-mesa-dev
```

Then build and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/focusbird
```

On Fedora, install the equivalent packages with `dnf` (`gcc-c++`, `cmake`,
`git`, `alsa-lib-devel`, `libX11-devel`, `libXrandr-devel`, `libXi-devel`,
`libXcursor-devel`, `libXinerama-devel`, and `mesa-libGL-devel`).
Use `--serial /dev/ttyACM0` or `--serial /dev/ttyUSB0` for the Grove board.
If access is denied, add your login to the device group (commonly `dialout`),
then log out and back in; do not run the game as root.

### Raylib dependency behavior

`CMakeLists.txt` first looks for an installed Raylib package (version 5 or
newer). Otherwise it downloads the pinned Raylib 5.5 source during the first
configure, so that configure needs internet access. To build offline, install
Raylib 5.x using the platform package manager and make its CMake package
discoverable before configuring.


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
| Grove sound sensor | A0 | A sound above the learned room baseline emits `SOUND` |
| Grove light sensor | A1 | Covering lowers the raw reading from its ambient baseline and emits `LIGHT` |

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

### Install the board core and upload firmware

Install Arduino IDE 2 or Arduino CLI for your operating system. The
[Arduino CLI installation guide](https://arduino.github.io/arduino-cli/latest/installation/)
covers Windows, macOS, and Linux; on macOS, Homebrew also provides it:

```sh
brew install arduino-cli
```

With Arduino CLI, install the AVR board core once, then list attached boards:

```sh
arduino-cli core install arduino:avr
arduino-cli board list
```

Compile the sketch for an Uno-compatible board:

```sh
arduino-cli compile --fqbn arduino:avr:uno arduino/focusbird
```

Upload using the port name for your OS:

Linux:

```sh
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno arduino/focusbird
```

macOS (use the `/dev/cu.*` device):

```sh
arduino-cli upload -p /dev/cu.usbmodem1101 --fqbn arduino:avr:uno arduino/focusbird
```

Windows PowerShell:

```powershell
arduino-cli upload -p COM3 --fqbn arduino:avr:uno arduino/focusbird
```

Replace the FQBN and example port with the values reported by `arduino-cli board
list`. The same compile/upload workflow is available in Arduino IDE: open
`arduino/focusbird/focusbird.ino`, choose the matching board and port, then
select **Upload**.

### Verify and tune the sensors

The firmware prints a diagnostic line every half second at **115200 baud**:

```text
SENSORS <soundRaw> <soundBaseline> <soundDelta> <lightRaw> <lightBaseline>
```

To inspect it, close FocusBird, open Arduino Serial Monitor (or run
`arduino-cli monitor -p PORT --config 115200`), then reset the board. Use
`COM3` on Windows, `/dev/cu.usbmodem...` on macOS, or `/dev/ttyACM0` on Linux.
Keep the room quiet for the first second while the sound baseline calibrates.
The `soundDelta` value is the current sound reading minus that learned baseline.
When finished, close the monitor before launching FocusBird; only one app can
normally own the serial port at a time.

Defaults and tuning guidance:

- `SOUND_DELTA_THRESHOLD` (default `450`): a sound must exceed the learned
  baseline by this many ADC counts for three consecutive samples. Increase it
  in steps (for example, to `500`) if the room still triggers; lower it if a
  deliberate yell does not trigger. A confirmed burst emits once, then must
  return near baseline before it can emit again.
- `SOUND_CALIBRATION_MS` (default `1000`): keep the room quiet during this
  startup interval so steady ambient noise becomes the baseline.
- `LIGHT_COVER_DELTA` (default `100`): the raw light reading must fall at least
  this far below its learned uncovered level for three samples. Standard Grove
  analog light sensors read higher when brighter. If the diagnostic `lightRaw`
  value clearly drops when covered but by less than 100 counts, lower this value
  (for example, to `60`).
- `LIGHT_RELEASE_DELTA` (default `45`): requires the reading to return close to
  the ambient level before another cover event is armed.
- `BUTTON_DEBOUNCE_MS`: rejects rapid electrical transitions from the button.

The `SENSORS` line also shows whether the light input changes at all. Check that
the analog Grove Light Sensor is plugged into **A1**, not an I2C/digital light
module, and that its Grove cable is seated. Covering should make `lightRaw`
drop; uncovering should bring it back toward `lightBaseline`. If the values do
not change, check the sensor, cable, and A1 port before tuning thresholds. The
firmware emits one `LIGHT` event when the cover is detected, not continuously
while the sensor remains covered.

## Connect the game to the Arduino

Find the device name with `arduino-cli board list`, then pass it as `--serial`:

```sh
# Linux
./build/focusbird --serial /dev/ttyACM0

# macOS
./build/focusbird --serial /dev/cu.usbmodem1101
```

Windows PowerShell:

```powershell
.\build\Release\focusbird.exe --serial COM3
```

Other common names include `/dev/ttyUSB0`, `/dev/cu.usbmodem...`, and `COM3`.
On Linux, access may require membership in the serial-device group (often
`dialout`). On Ubuntu/Debian, add yourself with
`sudo usermod -aG dialout "$USER"`, then log out and back in. Do not run the
game as root to work around permissions. Close Arduino Serial Monitor first.

If the requested port cannot be opened, FocusBird prints a warning and keeps
keyboard controls available while retrying every two seconds. The title screen
and HUD show **SEARCHING FOR GROVE KIT** until the port opens, then
**GROVE KIT CONNECTED**. If the device disconnects, the game continues and
reconnects automatically when the same port is available again.

## Serial protocol

The connection is **115200 baud, 8 data bits, no parity, 1 stop bit (8-N-1)**.
Each message is a newline-terminated ASCII line:

| Direction | Message | Meaning |
| --- | --- | --- |
| Arduino → game | `BTN` | Debounced button press; starts/restarts on menus and flaps during play |
| Arduino → game | `SOUND` | Confirmed sound burst crossed the configured threshold |
| Arduino → game | `LIGHT` | Light sensor transitioned to covered |
| Arduino → game | `SENSORS ...` | Raw sensor/baseline diagnostics, printed twice per second |
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
4. Use Serial Monitor to confirm that covering A1 lowers `lightRaw` by at least
   `LIGHT_COVER_DELTA`; check that a deliberate yell raises `soundDelta` above
   `SOUND_DELTA_THRESHOLD`. Close the monitor, then test both prompts in-game.
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
| CMake cannot find Raylib | Ensure the first configure can access GitHub, or install Raylib 5.x and expose its CMake package. |
| Linux build reports missing X11/audio dependencies | Install the Raylib development dependencies for your Linux distribution, then rerun CMake configure. |
| Serial port cannot open | Confirm the selected path, close Serial Monitor, check device permissions, and ensure the board finished uploading; FocusBird keeps retrying the selected port. |
| Sound fires in room noise | Let the room stay quiet during the first second after reset, then inspect `soundDelta`; raise `SOUND_DELTA_THRESHOLD` in 20–40 count steps. Each burst now needs three high samples and cannot repeat until it quiets. Since the ADC tops out at 1023, a baseline above 573 leaves less than 450 counts of headroom; lower the threshold if yells cannot reach it. |
| Sound prompt does not trigger | Inspect `soundDelta` while yelling; lower `SOUND_DELTA_THRESHOLD` gradually and check that the analog sound sensor is plugged into A0. |
| Light prompt does not trigger | Confirm the analog Grove Light Sensor is plugged into A1. Compare `lightRaw` with `lightBaseline` uncovered and covered; lower `LIGHT_COVER_DELTA` if the drop is smaller than its setting. |
| Light triggers without covering | Increase `LIGHT_COVER_DELTA`; the sensor must stay below its ambient baseline for three samples. |
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
