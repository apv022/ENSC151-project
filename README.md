# FocusBird

A child-friendly Flappy Bird-style reaction game in **C++17 + Raylib**, controlled
by an **Arduino Grove kit** (button, sound sensor, light sensor, buzzer) or the
keyboard. Fly through gates and answer each gate's prompt: **YELL** or
**COVER THE LIGHT**. The game tracks gates passed, correct/wrong/missed prompts,
and reaction times.

## Quick start

Install the tools once, then one command builds the game, uploads the Arduino
firmware, finds the board, and starts playing. The first run downloads Raylib
and the Arduino board package, so it needs internet and takes a few minutes.

### Windows 10/11

In PowerShell (then close and reopen it):

```powershell
winget install Python.Python.3.12
winget install Kitware.CMake
winget install ArduinoSA.CLI
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"
```

Plug in the board and **double-click `run-focusbird.cmd`**. If the board isn't
listed under *Ports (COM & LPT)* in Device Manager, or shows as `USB-SERIAL CH340`
with a warning icon (common on Grove Beginner Kits), install the CH340 driver
from WCH's website.

### macOS

```sh
xcode-select --install
brew install cmake python arduino-cli
./run-focusbird.py        # or double-click run-focusbird.command in Finder
```

### Linux

```sh
# Ubuntu/Debian
sudo apt install build-essential cmake python3 libasound2-dev libx11-dev libxrandr-dev \
  libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev
# Fedora
sudo dnf install gcc-c++ cmake python3 alsa-lib-devel libX11-devel libXrandr-devel \
  libXi-devel libXcursor-devel libXinerama-devel mesa-libGL-devel
# Arch
sudo pacman -S --needed base-devel cmake python alsa-lib mesa libx11 libxrandr libxi \
  libxcursor libxinerama arduino-cli
```

Get Arduino CLI if your distro doesn't package it (it installs to `~/.local/bin`):
`curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh`

Allow serial access, then **log out and back in**: `sudo usermod -aG dialout "$USER"`
(use `uucp` instead of `dialout` on Arch). Then run `./run-focusbird.py`.

### Launcher options

| Option | Effect |
| --- | --- |
| *(none)* | Build, upload firmware, play with the Grove kit |
| `--keyboard` | Play with the keyboard only (no Arduino tools needed) |
| `--no-upload` | Skip re-flashing the board; faster when the firmware is already on it |
| `--port PORT` | Choose the port, e.g. `COM4`, `/dev/ttyACM0`, `/dev/cu.usbmodem1101` |
| `--fqbn ID` | Board type if not Uno-compatible (see `arduino-cli board listall`) |
| `--build-only` | Just compile the game |

If no board is found, the launcher offers keyboard mode. **Close Arduino Serial
Monitor and any open FocusBird window first:** only one program can use the port.

## Controls

| Input | Action |
| --- | --- |
| `Enter` / Grove button | Start, or play again from the results screen |
| `Space` / `Up` / Grove button | Flap (never answers a prompt) |
| Sound sensor / `Y` | Answer a **YELL** prompt |
| Light sensor covered / `C` | Answer a **COVER** prompt |
| `1` / `2` | Easy / Challenge (title screen) |
| `T` / `B` | Cycle background / bird color (title screen) |
| `Esc` | Quit |

## Rules

- Three lives. A gate's prompt opens when it gets close (its **!** or **C** badge appears) and closes when it passes the bird.
- Correct answer: gate turns green, success beep, reaction time recorded.
- Wrong answer or missed prompt: one life. Sensor events outside a prompt window are ignored.
- Touching any gate (even a green one), the ground, or the ceiling: one life.
- Challenge mode has faster gates and narrower gaps.

## Hardware

| Grove module | Pin | Behavior |
| --- | --- | --- |
| Button | D2 | Sends `BTN` on press (module has its own pull-down; pressed = HIGH) |
| Buzzer | D3 | Prompt cue, success and error tones |
| Sound sensor | A0 | Sends `SOUND` when the loudest reading in two 20 ms windows in a row is above 700 |
| Light sensor (analog) | A1 | Sends `LIGHT` when raw reading drops to 10% of full scale (≤102) |

Pins and thresholds are constants at the top of `arduino/focusbird/focusbird.ino`.

**Tuning:** the board prints `SENSORS <soundPeak> <light> <soundCutoff> <lightCutoff>`
twice a second, where `soundPeak` is the loudest sound reading of the last half second.
That's the same value the yell detector uses. Close the game and open Arduino Serial Monitor (or
`arduino-cli monitor -p PORT --config 115200`), then yell and cover the sensor to
see the values. Adjust `SOUND_RAW_THRESHOLD` / `LIGHT_RAW_THRESHOLD` if your room
or sensor needs it, and `ANALOG_MAX_VALUE` for boards whose ADC isn't 10-bit.

## Serial protocol

115200 baud, 8-N-1, newline-terminated ASCII lines.

| Direction | Message | Meaning |
| --- | --- | --- |
| Arduino → game | `BTN`, `SOUND`, `LIGHT` | Input events |
| Arduino → game | `READY`, `SENSORS ...` | Diagnostics (ignored by the game) |
| Game → Arduino | `BUZZER <hz> <ms>` | Play a tone: `900 70` prompt, `1350 100` correct, `380 150` wrong |

If the port can't be opened or the board is unplugged, the game keeps running on
the keyboard and reconnects every two seconds (status: **SEARCHING FOR GROVE KIT**
→ **GROVE KIT CONNECTED**).

## Manual build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/focusbird --serial PORT        # Windows: build\Release\focusbird.exe --serial COM4
```

Omit `--serial` for keyboard mode. CMake uses an installed Raylib 5.x if found,
otherwise it downloads Raylib 5.5. Upload the firmware by hand with Arduino IDE
(open `arduino/focusbird/focusbird.ino`, pick board and port, Upload) or:

```sh
arduino-cli core install arduino:avr
arduino-cli compile --upload -p PORT --fqbn arduino:avr:uno arduino/focusbird
```

## Troubleshooting

| Problem | Fix |
| --- | --- |
| `cmake`/`py`/`arduino-cli` not recognized (Windows) | Reopen PowerShell after `winget install`. |
| "No CMAKE_CXX_COMPILER could be found" | Install the compiler from Quick start, delete `build/`, rerun. |
| Linux: "RandR headers not found" or similar | Install your distro's packages from Quick start, delete `build/`, rerun. |
| Board not found | Try another USB cable (many are charge-only); Windows: install the CH340 driver. |
| Upload fails / "Access is denied" / stuck on SEARCHING | Another program has the port: close Serial Monitor/IDE and other FocusBird windows. |
| Linux: permission denied on `/dev/tty*` | Join `dialout` (`uucp` on Arch), log out and in. Don't use `sudo`. |
| Sound/light don't trigger or trigger by themselves | Watch the `SENSORS` values (see Tuning) and check the modules are on A0/A1. |
| Buzzer silent | Check it's on D3 and the board supports `tone()` on that pin. |

## Test checklist

**Keyboard:** start without `--serial` (HUD shows KEYBOARD MODE). In both difficulties,
check flapping, correct `Y`/`C` answers (gate turns green), a wrong key, a missed
prompt, gate/ground/ceiling hits (each costs a life), and that replay resets the stats.

**Hardware:** confirm GROVE KIT CONNECTED; the button starts and flaps but never answers;
yelling and covering are accepted only during the matching prompt; a cover before a
COVER prompt doesn't count until you uncover and cover again; holding the button or
keeping the sensor covered produces no repeated events; unplugging mid-run keeps the
game going and replugging reconnects; buzzer tones play.

## Project layout

```text
run-focusbird.py / .cmd / .command  One-command build, upload, and launch (+ Windows/macOS wrappers)
CMakeLists.txt                      Game build; fetches Raylib if not installed
src/main.cpp                        Game rules, input, and drawing
src/serial_port.cpp, include/       Non-blocking serial port for Windows and macOS/Linux
arduino/focusbird/focusbird.ino     Grove sensor/button/buzzer firmware
.github/workflows/build.yml         CI: builds on Windows, macOS, Linux and compiles the firmware
```
