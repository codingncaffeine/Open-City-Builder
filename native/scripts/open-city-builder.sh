#!/bin/bash
# Starts the Linux dev build in a window. The build copies this launcher and the executable into the project's bin/ folder:
#   bin/open-city-builder [DATA] [game arguments...]
# DATA is the PC game folder (SimCity_*.dat, Fonts/, Regions/ ...), only read; default: scratch/testbed/game. Game arguments
# such as -intro:off pass through.
set -e
BIN="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$BIN")"
DATA="$ROOT/scratch/testbed/game"
if [ $# -gt 0 ] && [ -d "$1" ]; then DATA="$1"; shift; fi
DATA="$(realpath "$DATA")"
[ -x "$BIN/sc4_game_linux" ] || { echo "no build yet: run native/scripts/build_linux_game.sh" >&2; exit 1; }
bash "$ROOT/native/scripts/install_desktop_entry.sh" "$DATA" >/dev/null 2>&1 || true   # the taskbar icon (Wayland matches the app id)
CONTENTS="$(mktemp -d)"   # the game looks for <contents>/GameData: point it at the PC folder
trap 'rm -rf "$CONTENTS"' EXIT
ln -s "$DATA" "$CONTENTS/GameData"
SC4_GAME_CONTENTS="$CONTENTS" "$BIN/sc4_game_linux" "$@"
