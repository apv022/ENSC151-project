#!/usr/bin/env python3
"""Build FocusBird, flash the Grove firmware, and launch the game.

Works the same on Windows, macOS, and Linux:

    python run-focusbird.py              # build, upload firmware, play with the Grove kit
    python run-focusbird.py --keyboard   # build and play with the keyboard only
    python run-focusbird.py --no-upload  # reuse the firmware already on the board
    python run-focusbird.py --build-only # just compile the game

If no board (or no Arduino CLI) is found, it offers to start in keyboard mode
instead of failing.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent
SKETCH = ROOT / "arduino" / "focusbird"
DEFAULT_FQBN = "arduino:avr:uno"
IS_WINDOWS = sys.platform.startswith("win")
IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")


class LaunchError(Exception):
    """A problem with a clear, user-facing explanation."""


def info(message):
    print(message, flush=True)


def run(command, cwd=ROOT):
    info(f"\n==> {' '.join(str(part) for part in command)}")
    subprocess.run([str(part) for part in command], cwd=cwd, check=True)


def capture(command):
    """Run a command and return stdout, or None if it failed."""
    try:
        result = subprocess.run(
            [str(part) for part in command],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    return result.stdout if result.returncode == 0 else None


def ask_yes_no(question, default=True):
    if not sys.stdin.isatty():
        return default
    suffix = "[Y/n]" if default else "[y/N]"
    answer = input(f"{question} {suffix} ").strip().lower()
    return default if not answer else answer.startswith("y")


def pause_if_double_clicked():
    """Keep a double-clicked console window open long enough to read errors."""
    if os.environ.get("FOCUSBIRD_PAUSE") == "1" and sys.stdin.isatty():
        input("\nPress Enter to close this window...")


# ------------------------------------------------------------------ tools ----


def find_arduino_cli():
    """Find arduino-cli on PATH, in ~/.local/bin, or bundled inside Arduino IDE 2."""
    found = shutil.which("arduino-cli")
    if found:
        return found
    bundled = Path("resources") / "app" / "lib" / "backend" / "resources"
    if IS_WINDOWS:
        candidates = [
            Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Arduino IDE" / bundled / "arduino-cli.exe",
            Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Arduino IDE" / bundled / "arduino-cli.exe",
        ]
    elif IS_MAC:
        candidates = [Path("/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli")]
    else:
        candidates = [Path.home() / ".local" / "bin" / "arduino-cli"]
    return next((str(c) for c in candidates if c.is_file()), None)


def arduino_cli_install_hint():
    if IS_WINDOWS:
        return "winget install ArduinoSA.CLI   (or install Arduino IDE 2)"
    if IS_MAC:
        return "brew install arduino-cli   (or install Arduino IDE 2)"
    return (
        "sudo pacman -S arduino-cli   (Arch)\n"
        "  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh"
        " | BINDIR=~/.local/bin sh   (any distro)"
    )


def cmake_install_hint():
    if IS_WINDOWS:
        return (
            "winget install Kitware.CMake\n"
            "  winget install Microsoft.VisualStudio.2022.BuildTools --override "
            "\"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive\""
        )
    if IS_MAC:
        return "xcode-select --install && brew install cmake"
    return "see the Linux section of README.md for your distribution's packages"


def require_cmake():
    cmake = shutil.which("cmake")
    if cmake is None and IS_WINDOWS:
        default = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "CMake" / "bin" / "cmake.exe"
        if default.is_file():
            cmake = str(default)
    if cmake is None:
        raise LaunchError(
            "CMake is not installed (or not on PATH). Install it with:\n  " + cmake_install_hint()
        )
    return cmake


# ------------------------------------------------------------ build game ----


def build_dir():
    root_tag = hashlib.sha256(str(ROOT).encode("utf-8")).hexdigest()[:10]
    default = ROOT / "build" / f"run-focusbird-{root_tag}"
    path = Path(os.environ.get("FOCUSBIRD_BUILD_DIR", default)).expanduser()
    return path if path.is_absolute() else ROOT / path


def build_game(cmake):
    out = build_dir()
    configure = [cmake, "-S", ROOT, "-B", out, "-DCMAKE_BUILD_TYPE=Release"]
    if IS_WINDOWS and not (out / "CMakeCache.txt").exists() and not os.environ.get("CMAKE_GENERATOR"):
        configure += ["-A", "x64"]  # 64-bit Visual Studio build; works from a normal PowerShell window.
    try:
        run(configure)
        run([cmake, "--build", out, "--config", "Release", "--parallel"])
    except subprocess.CalledProcessError:
        hint = "The C++ build failed. Make sure a C++ compiler is installed:\n  " + cmake_install_hint()
        if IS_WINDOWS:
            hint += (
                "\nIf you changed compilers, delete the build folder and try again:\n  "
                f"rmdir /s /q \"{out}\""
            )
        raise LaunchError(hint) from None

    for candidate in (
        out / "Release" / "focusbird.exe",
        out / "focusbird.exe",
        out / "focusbird",
    ):
        if candidate.is_file():
            return candidate
    raise LaunchError(f"build finished but the FocusBird executable was not found in {out}")


# ------------------------------------------------------------- the board ----


def list_serial_ports(cli):
    output = capture([cli, "board", "list", "--format", "json"])
    if output is None:
        return []
    try:
        data = json.loads(output or "[]")
    except json.JSONDecodeError:
        return []
    # arduino-cli >= 1.0 wraps the list in {"detected_ports": [...]}; 0.x returns a bare list.
    detected = data.get("detected_ports", []) if isinstance(data, dict) else data

    ports = []
    for item in detected or []:
        port = item.get("port") or {}
        address = port.get("address")
        if not address or port.get("protocol") != "serial":
            continue
        matches = item.get("matching_boards") or item.get("boards") or []
        match = matches[0] if len(matches) == 1 else {}
        props = port.get("properties") or {}
        ports.append(
            {
                "port": address,
                "fqbn": match.get("fqbn", ""),
                "name": match.get("name", ""),
                # Built-in COM1 and Bluetooth serial ports have no USB vendor id.
                "usb": bool(props.get("vid")) or "usb" in address.lower(),
                "label": port.get("protocol_label") or port.get("label") or "",
            }
        )
    return ports


def choose_port(ports, requested):
    if requested:
        match = next((p for p in ports if p["port"].lower() == requested.lower()), {})
        return requested, match.get("fqbn", ""), match.get("name", "")

    # Narrow down: recognised Arduino boards first, then any USB serial device.
    for subset in (
        [p for p in ports if p["fqbn"]],
        [p for p in ports if p["usb"]],
        ports,
    ):
        if len(subset) == 1:
            p = subset[0]
            return p["port"], p["fqbn"], p["name"]
        if subset:
            ports = subset
            break

    if not ports:
        return None, "", ""

    info("More than one serial port is connected:")
    for index, p in enumerate(ports, start=1):
        info(f"  {index}) {p['port']}  ({p['name'] or p['label'] or 'board not identified'})")
    if not sys.stdin.isatty():
        raise LaunchError("several serial ports found; choose one with --port PORT")
    selection = input("Select the Grove board number: ").strip()
    if not selection.isdigit() or not 1 <= int(selection) <= len(ports):
        raise LaunchError("invalid board selection")
    p = ports[int(selection) - 1]
    return p["port"], p["fqbn"], p["name"]


def serial_permission_problem(port):
    if not IS_LINUX or not Path(port).exists() or os.access(port, os.R_OK | os.W_OK):
        return ""
    distro_id = ""
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            if line.startswith("ID="):
                distro_id = line.partition("=")[2].strip('"').lower()
    except OSError:
        pass
    group = "uucp" if distro_id in ("arch", "manjaro", "endeavouros") else "dialout"
    return (
        f"No read/write permission for {port}. Run:\n"
        f"  sudo usermod -aG {group} \"$USER\"\n"
        "then log out and back in (or reboot). Do not run the game with sudo."
    )


def ensure_core(cli, fqbn):
    core = ":".join(fqbn.split(":")[:2])
    listing = capture([cli, "core", "list"]) or ""
    installed = {line.split()[0] for line in listing.splitlines() if line.split()}
    if core not in installed:
        info(f"\nInstalling the Arduino board support package {core} (first run only)...")
        run([cli, "core", "update-index"])
        run([cli, "core", "install", core])


def upload_firmware(cli, port, fqbn):
    ensure_core(cli, fqbn)
    run([cli, "compile", "--fqbn", fqbn, SKETCH])
    try:
        run([cli, "upload", "--port", port, "--fqbn", fqbn, SKETCH])
    except subprocess.CalledProcessError:
        hint = (
            f"Uploading to {port} failed. Usually this means:\n"
            "  - another program has the port open: close Arduino Serial Monitor/IDE\n"
            "    and any FocusBird window that is still running, then try again;\n"
            "  - the wrong board type: pass --fqbn (see `arduino-cli board listall`);\n"
            "  - a charge-only USB cable or a loose connection."
        )
        if IS_WINDOWS:
            hint += (
                "\n  - Grove Beginner Kits / Seeeduino boards use a CH340 USB chip. If the\n"
                "    board shows up as 'USB-SERIAL CH340' with a warning in Device Manager,\n"
                "    install the CH340 driver from the WCH website, then replug the board."
            )
        raise LaunchError(hint) from None
    # The board resets after upload; on Windows the COM port briefly disappears.
    time.sleep(2.0)


# ------------------------------------------------------------------ main ----


def main():
    parser = argparse.ArgumentParser(
        description="Build FocusBird, upload the Grove firmware, and launch the game."
    )
    parser.add_argument("--port", default=os.environ.get("FOCUSBIRD_PORT"),
                        help="serial port of the Grove board, e.g. COM3 or /dev/ttyACM0")
    parser.add_argument("--fqbn", default=os.environ.get("FOCUSBIRD_FQBN"),
                        help=f"Arduino board id (default: detected, else {DEFAULT_FQBN})")
    parser.add_argument("--keyboard", action="store_true",
                        help="skip the Arduino entirely and play with the keyboard")
    parser.add_argument("--no-upload", action="store_true",
                        help="connect to the board without re-uploading the firmware")
    parser.add_argument("--build-only", action="store_true",
                        help="build the game and stop (no Arduino, no launch)")
    args = parser.parse_args()

    info(f"FocusBird launcher on {platform.system()} ({platform.machine()})")
    cmake = require_cmake()
    game = build_game(cmake)
    if args.build_only:
        info(f"\nBuilt {game}")
        return 0

    port = None
    if not args.keyboard:
        port = prepare_board(args)

    command = [str(game)]
    if port:
        command += ["--serial", port]
        info(f"\n==> Starting FocusBird connected to {port}")
    else:
        info("\n==> Starting FocusBird in keyboard mode (Space = flap, Y = sound, C = light)")
    return subprocess.run(command, cwd=ROOT).returncode


def prepare_board(args):
    """Return the port to connect to, or None to play with the keyboard."""
    cli = find_arduino_cli()
    if cli is None:
        if args.port and args.no_upload:
            return args.port
        info("\nArduino CLI was not found, so the Grove firmware cannot be uploaded.")
        info("Install it with:\n  " + arduino_cli_install_hint())
        if ask_yes_no("Start in keyboard mode instead?"):
            return None
        raise LaunchError("Arduino CLI is required for Grove mode")

    info("\n==> Looking for the Grove Arduino")
    port, detected_fqbn, name = choose_port(list_serial_ports(cli), args.port)
    if port is None:
        info("No Arduino was found. Check the USB cable (some cables are charge-only).")
        if IS_WINDOWS:
            info("On Windows, look under 'Ports (COM & LPT)' in Device Manager; a CH340 board\n"
                 "with a warning icon needs the CH340 driver.")
        if ask_yes_no("Start in keyboard mode instead?"):
            return None
        raise LaunchError("no Arduino found; connect the board or pass --port PORT")

    fqbn = args.fqbn or detected_fqbn or DEFAULT_FQBN
    info(f"Using {port} ({name or 'board not identified'}), board type {fqbn}")

    problem = serial_permission_problem(port)
    if problem:
        raise LaunchError(problem)

    if not args.no_upload:
        upload_firmware(cli, port, fqbn)
    return port


if __name__ == "__main__":
    code = 1
    try:
        code = main()
    except LaunchError as error:
        print(f"\nError: {error}", file=sys.stderr)
    except subprocess.CalledProcessError as error:
        print(f"\nError: command failed with exit code {error.returncode}", file=sys.stderr)
        code = error.returncode or 1
    except KeyboardInterrupt:
        code = 130
    if code:
        pause_if_double_clicked()
    sys.exit(code)
