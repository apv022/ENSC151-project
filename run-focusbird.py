#!/usr/bin/env python3
"""Build the desktop app and Grove firmware, upload it, then launch FocusBird."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
DEFAULT_FQBN = "arduino:avr:uno"


def fail(message):
    print(f"Error: {message}", file=sys.stderr)
    return 1


def run(command, cwd=ROOT):
    print(f"\n==> {' '.join(str(part) for part in command)}", flush=True)
    subprocess.run([str(part) for part in command], cwd=cwd, check=True)


def capture(command):
    result = subprocess.run(
        [str(part) for part in command],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        raise RuntimeError(f"command failed: {' '.join(str(part) for part in command)}")
    return result.stdout


def read_board_list():
    return json.loads(capture(["arduino-cli", "board", "list", "--json"])).get(
        "detected_ports", []
    )


def serial_boards(detected_ports):
    boards = []
    for item in detected_ports:
        port = item.get("port") or {}
        address = port.get("address")
        if not address or port.get("protocol") != "serial":
            continue
        matches = item.get("matching_boards") or []
        match = matches[0] if len(matches) == 1 else {}
        boards.append(
            {
                "port": address,
                "fqbn": match.get("fqbn", ""),
                "name": match.get("name", ""),
            }
        )
    return boards


def choose_board(boards, requested_port):
    if requested_port:
        match = next((board for board in boards if board["port"] == requested_port), {})
        return requested_port, match.get("fqbn", ""), match.get("name", "")

    if not boards:
        raise ValueError(
            "no serial board found. Connect the Grove Arduino, check its USB cable, "
            "or pass --port PORT"
        )
    if len(boards) == 1:
        board = boards[0]
        return board["port"], board["fqbn"], board["name"]

    print("More than one serial board is connected:")
    for index, board in enumerate(boards, start=1):
        label = board["name"] or "board not identified"
        print(f"  {index}) {board['port']} ({label})")
    if not sys.stdin.isatty():
        raise ValueError("select a board with --port PORT")
    selection = input("Select board number: ").strip()
    if not selection.isdigit() or not 1 <= int(selection) <= len(boards):
        raise ValueError("invalid board selection")
    board = boards[int(selection) - 1]
    return board["port"], board["fqbn"], board["name"]


def installed_cores():
    output = capture(["arduino-cli", "core", "list"])
    return {
        line.split()[0]
        for line in output.splitlines()
        if line.split() and ":" in line.split()[0]
    }


def serial_permission_hint(port):
    if not sys.platform.startswith("linux") or not Path(port).exists():
        return ""
    if os.access(port, os.R_OK | os.W_OK):
        return ""

    distro_id = ""
    try:
        for line in Path("/etc/os-release").read_text().splitlines():
            if line.startswith("ID="):
                distro_id = line.partition("=")[2].strip('"').lower()
                break
    except OSError:
        pass
    group = "uucp" if distro_id == "arch" else "dialout"
    return (
        f"No read/write permission for {port}. Add your user to {group} "
        "(`sudo usermod -aG "
        f"{group} \"$USER\"`), then log out and back in."
    )


def app_path(build_dir):
    candidates = (
        build_dir / "focusbird",
        build_dir / "Release" / "focusbird.exe",
        build_dir / "focusbird.exe",
    )
    return next((candidate for candidate in candidates if candidate.is_file()), None)


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Build FocusBird and its Grove firmware, upload the sketch, "
            "detect the serial port, and launch the game."
        )
    )
    parser.add_argument("--port", default=os.environ.get("FOCUSBIRD_PORT"))
    parser.add_argument("--fqbn", default=os.environ.get("FOCUSBIRD_FQBN"))
    args = parser.parse_args()

    for tool in ("cmake", "arduino-cli"):
        if shutil.which(tool) is None:
            if tool == "arduino-cli":
                return fail(
                    "Arduino CLI is missing. On Arch Linux install it with "
                    "`sudo pacman -S arduino-cli`."
                )
            return fail(f"{tool} is missing; install it for your operating system.")

    print("==> Detecting the Grove Arduino")
    detected_ports = read_board_list()
    boards = serial_boards(detected_ports)
    try:
        port, detected_fqbn, board_name = choose_board(boards, args.port)
    except ValueError as error:
        print("arduino-cli board list:")
        subprocess.run(["arduino-cli", "board", "list"], cwd=ROOT, check=False)
        return fail(str(error))

    fqbn = args.fqbn or detected_fqbn or DEFAULT_FQBN
    print(f"Using {port} ({board_name or 'board not identified'}), FQBN {fqbn}")
    permission_error = serial_permission_hint(port)
    if permission_error:
        return fail(permission_error)

    core = ":".join(fqbn.split(":")[:2])
    if core not in installed_cores():
        run(["arduino-cli", "core", "update-index"])
        run(["arduino-cli", "core", "install", core])

    build_dir = Path(os.environ.get("FOCUSBIRD_BUILD_DIR", ROOT / "build")).expanduser()
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir

    run(
        [
            "cmake",
            "-S",
            ROOT,
            "-B",
            build_dir,
            "-DCMAKE_BUILD_TYPE=Release",
        ]
    )
    run(["cmake", "--build", build_dir, "--config", "Release", "--parallel"])
    run(["arduino-cli", "compile", "--fqbn", fqbn, ROOT / "arduino" / "focusbird"])
    run(
        [
            "arduino-cli",
            "upload",
            "--port",
            port,
            "--fqbn",
            fqbn,
            ROOT / "arduino" / "focusbird",
        ]
    )

    game = app_path(build_dir)
    if game is None:
        return fail(f"build finished but the FocusBird executable was not found in {build_dir}")

    print(f"\n==> Starting FocusBird on {port}", flush=True)
    return subprocess.run([str(game), "--serial", port], cwd=ROOT).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode or 1)
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        sys.exit(f"Error: {error}")
