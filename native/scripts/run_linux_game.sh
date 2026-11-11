#!/bin/bash
# Kept for the old command: builds if needed, then runs bin/open-city-builder (see scripts/open-city-builder.sh).
#   scripts/run_linux_game.sh [DATA] [game arguments...]
set -e
cd "$(dirname "$0")/.."
[ -x ../bin/open-city-builder ] || bash scripts/build_linux_game.sh
exec ../bin/open-city-builder "$@"
