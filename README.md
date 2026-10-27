# FocusBird

FocusBird is a small, child-friendly Flappy Bird-inspired reaction game. It is a
desktop **C++17 + Raylib** application with optional **Arduino/Grove** controls.
The player guides a bird through gates while answering prompts with a button,
sound, or covered light sensor. The game records gates passed, correct answers,
wrong responses, missed prompts, and play time.

The game has no external art or sound-asset dependency: its graphics are drawn
with Raylib primitives. It can be played with keyboard controls even when the
Arduino is not connected, which is useful for development and demonstrations.

## Features

- Side-scrolling game with bird physics, obstacle gaps, and collision detection.
- Three prompt types: **PRESS THE BUTTON**, **YELL**, and **COVER THE LIGHT**.
- Correct prompt answers open that gate and play a positive buzzer tone.
- Wrong answers, missed prompts, ground/ceiling contact, and closed-gate
  collisions use a three-life allowance.
- Easy and Challenge presets, with different obstacle speed and gap size.
- HUD shows score, remaining lives, correct responses, current prompt, average
  correct-response time, and whether the Arduino is connected.
- Results screen reports correct, incorrect, and missed prompts separately, as
  well as average and fastest correct reaction time.
- Non-blocking serial communications: loss of the Arduino connection does not
  stop the game.
- Keyboard fallback for all three inputs.

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
- Raylib's normal platform dependencies. On Linux, a desktop/X11 development
  environment is required; Raylib's build output will identify any missing
  packages.
- Optional hardware: an Arduino compatible with the Arduino IDE/CLI and Grove
  button, sound sensor, light sensor, and buzzer modules.

## Build the desktop game

From the project root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The executable is usually `build/focusbird` on Linux/macOS and
`build/Release/focusbird.exe` with a multi-configuration Windows generator.

Run without hardware:

```sh
./build/focusbird
```

On Windows, run the executable from the build output directory or use its full
path. For a debug build, replace `Release` with `Debug` or omit the build type.

### Raylib dependency behavior

`CMakeLists.txt` first looks for a system Raylib package (version 5 or newer).
When none is found, it downloads the pinned Raylib 5.5 source via CMake
`FetchContent`. The initial configure therefore needs network access. To build
fully offline, install Raylib 5.x using the platform package manager and make
its CMake package discoverable before configuring.

## Keyboard controls

| Key | Action | Prompt answered |
| --- | --- | --- |
| `Enter` | Start / play again | — |
| `1` | Select Easy on title screen | — |
| `2` | Select Challenge on title screen | — |
| `Space` or `Up` | Flap | Button prompt |
| `Y` | Simulate a sound sensor event | Yell prompt |
| `C` | Simulate covering the light sensor | Cover prompt |
| `Esc` | Quit | — |

An input only answers the **currently active** matching prompt. An early event,
late event, or unrelated sensor event is not counted as the requested response.
The button still flaps whenever it is pressed, including when no button prompt is
active. Since it is also flight control, pressing it during a YELL or COVER
prompt does not count as an incorrect answer.

## Arduino hardware setup

Connect the Grove components to the pins configured at the top of
`arduino/focusbird/focusbird.ino`:

| Component | Default pin | Role |
| --- | --- | --- |
| Grove button | D2 | Flap and button-prompt response; uses `INPUT_PULLUP` (pressed is LOW) |
| Grove buzzer | D3 | Prompt cue and correct/incorrect response feedback |
| Grove sound sensor | A0 | A sound above the learned room baseline emits `SOUND` |
| Grove light sensor | A1 | Covering the sensor below its threshold emits `LIGHT` |

Pin numbers vary by Grove base shield, board, and wiring. Confirm the labels on
your board before connecting anything, and change the constants in the sketch
if your setup differs. Use only sensor/output voltage levels supported by the
selected Arduino board.

### Upload firmware

1. Open `arduino/focusbird/focusbird.ino` in the Arduino IDE, or select the
   `arduino/focusbird` sketch directory in Arduino CLI.
2. Select the matching board and USB port.
3. Compile and upload the sketch.
4. Close Arduino Serial Monitor before starting the game; only one application
   can normally own the serial port at a time.

With Arduino CLI installed, a typical Uno-compatible upload is:

```sh
arduino-cli compile --fqbn arduino:avr:uno arduino/focusbird
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:avr:uno arduino/focusbird
```

Replace the FQBN and port with values for your board. On Windows, a port may be
named `COM3`; on macOS it is commonly `/dev/cu.usbmodem...`.

### Tune the sensors

The sketch exposes these starting values near its top:

- `SOUND_DELTA_THRESHOLD`: analog rise above the automatically learned quiet
  baseline required to emit `SOUND`. Raise it if room noise triggers events;
  lower it if a deliberate yell is not detected.
- `SOUND_COOLDOWN_MS`: suppresses multiple detections from one yell/noise.
- `LIGHT_COVERED_THRESHOLD`: analog value at or below which the sensor counts as
  covered. Check actual uncovered/covered readings in your lighting before
  changing it.
- `LIGHT_RELEASE_HYSTERESIS`: requires the sensor to become brighter than the
  cover threshold by this amount before another cover event can be generated.
- `BUTTON_DEBOUNCE_MS`: rejects rapid electrical transitions from the button.

Sensor modules and rooms differ. Test readings in the actual demonstration
environment and adjust these values instead of assuming the defaults fit every
board. The sketch deliberately emits a light event on the transition into the
covered state, not continuously while it remains covered.

## Connect the game to the Arduino

Find the device name, then pass it as `--serial`:

```sh
./build/focusbird --serial /dev/ttyACM0
```

Other common names include `/dev/ttyUSB0`, `/dev/cu.usbmodem...`, and `COM3`.
On Linux, access may require membership in the system's serial-device group
(often `dialout`) and a new login session. Do not run the game as root just to
work around normal device permissions. Close Arduino Serial Monitor first.

If the requested port cannot be opened, FocusBird prints a warning and starts
in keyboard mode. The status in the HUD changes to **KEYBOARD MODE** if the
serial device later disconnects.

## Serial protocol

The connection is **115200 baud, 8 data bits, no parity, 1 stop bit (8-N-1)**.
Each message is a newline-terminated ASCII line:

| Direction | Message | Meaning |
| --- | --- | --- |
| Arduino → game | `BTN` | Debounced button press; also flaps the bird |
| Arduino → game | `SOUND` | Sound crossed the configured threshold |
| Arduino → game | `LIGHT` | Light sensor transitioned to covered |
| Game → Arduino | `BUZZER 900 70` | Play a 900 Hz prompt cue for 70 ms |
| Game → Arduino | `BUZZER 1350 100` | Short positive feedback tone |
| Game → Arduino | `BUZZER 380 150` | Lower-pitched incorrect-action feedback |

The game timestamps a response when its complete line is received. Unknown
lines, including the firmware's optional `READY` diagnostic, are ignored.
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
- A correct answer records a success and marks that gate safe to pass.
- Correct answers record reaction time from prompt activation to input receipt.
- The gate passing the bird increments the score.
- A wrong action during an active prompt closes that prompt, records an incorrect
  response, and costs one life.
- Button presses during sound/light prompts remain flap controls and are not
  treated as incorrect prompt answers.
- Letting a prompt expire records a miss and costs one life.
- Contact with a closed gate or the top/bottom playfield boundary costs a life.
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
3. Answer each prompt with the matching key (`Space`, `Y`, or `C`); verify the
   correct count increases and the gate changes to its safe color.
4. Use an incorrect key during a prompt; verify incorrect count and lives change.
5. Do not answer a prompt; verify the miss count increases after the gate passes.
6. Hit an obstacle and the screen boundaries; verify collisions reduce lives and
   the game-over summary appears at zero.
7. Restart from the results screen; verify statistics reset and difficulty stays
   selected.

### Hardware and integration checks

1. Upload the firmware and confirm the board appears as a serial device.
2. Launch the game with the device path and check **ARDUINO CONNECTED**.
3. Press the button and verify the bird flaps; during a button prompt, verify it
   also records a correct answer.
4. Make a deliberate sound during a YELL prompt and cover the light sensor during
   a COVER prompt; verify each is accepted only during its matching window.
5. Cover the light sensor before a COVER prompt, then uncover it. Confirm the
   early event does not answer the later prompt and a new cover transition does.
6. Hold the button and keep the light sensor covered; verify neither produces a
   stream of repeated input events.
7. Unplug the Arduino during a run; verify the game continues in keyboard mode.
8. Check buzzer cues and tune thresholds in the actual room.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| CMake cannot find Raylib | Ensure the first configure can access GitHub, or install Raylib 5.x and expose its CMake package. |
| Linux build reports missing X11/audio dependencies | Install the Raylib development dependencies for your Linux distribution, then rerun CMake configure. |
| Serial port cannot open | Confirm the selected path, close Serial Monitor, check device permissions, and ensure the board finished uploading. |
| Inputs are too sensitive | Increase `SOUND_DELTA_THRESHOLD`; adjust the light threshold using observed raw values. |
| Sound prompt does not trigger | Lower `SOUND_DELTA_THRESHOLD`, check the sensor analog output, and test closer to the microphone. |
| Light prompt triggers while uncovered | Compare uncovered/covered readings and lower `LIGHT_COVERED_THRESHOLD` as appropriate. |
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
