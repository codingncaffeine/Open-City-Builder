#!/bin/bash
# Opens the Linux game build in a window. DATA is the PC game folder (the one holding SimCity_*.dat, Fonts/, Regions/ ...);
# it is only read. The game keeps its regions and saves under /tmp/sc4boot_docs for now (NativeBoot's documents folder).
#   scripts/run_linux_game.sh [DATA]      (default: ../scratch/testbed/game, the test-bed copy)
set -e
cd "$(dirname "$0")/.."
DATA="$(realpath "${1:-../scratch/testbed/game}")"
[ -x build/sc4_game_linux ] || bash scripts/build_linux_game.sh
bash scripts/install_desktop_entry.sh "$DATA" >/dev/null   # the taskbar icon (Wayland matches the app id to the entry)
CONTENTS="$(mktemp -d)"   # the build looks for <contents>/GameData: point it at the PC folder
trap 'rm -rf "$CONTENTS"' EXIT
ln -s "$DATA" "$CONTENTS/GameData"
SC4_GAME_CONTENTS="$CONTENTS" build/sc4_game_linux
