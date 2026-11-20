#!/bin/sh
# Double-click in Finder (macOS) to build and play FocusBird.
cd "$(dirname "$0")" || exit 1
FOCUSBIRD_PAUSE=1 exec python3 ./run-focusbird.py "$@"
