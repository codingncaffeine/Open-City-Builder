#!/bin/bash
# Installs the desktop entry and icons the taskbar needs for the Linux game build. On Wayland a taskbar shows a window's icon
# by matching its app id (the game sets io.github.codingncaffeine.OpenCityBuilder) to a desktop entry of that name; without the entry it shows a
# generic icon. Per user, under $XDG_DATA_HOME (default ~/.local/share); rerunning updates the entry in place.
#   scripts/install_desktop_entry.sh [DATA]   DATA = the PC game folder the entry's launcher passes on (optional)
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
SHARE="${XDG_DATA_HOME:-$HOME/.local/share}"
ID=io.github.codingncaffeine.OpenCityBuilder
mkdir -p "$SHARE/applications" "$SHARE/icons/hicolor/256x256/apps"
cp ../art/logo-256.png "$SHARE/icons/hicolor/256x256/apps/$ID.png"
if command -v magick >/dev/null; then
  for size in 16 24 32 48 64 128; do
    mkdir -p "$SHARE/icons/hicolor/${size}x${size}/apps"
    magick ../art/logo-256.png -resize ${size}x${size} "$SHARE/icons/hicolor/${size}x${size}/apps/$ID.png"
  done
fi
EXEC="\"$ROOT/scripts/run_linux_game.sh\""
[ -n "$1" ] && EXEC="$EXEC \"$(realpath "$1")\""
cat > "$SHARE/applications/$ID.desktop" <<ENTRY
[Desktop Entry]
Type=Application
Name=Open City Builder
Comment=City building on Linux
Exec=$EXEC
Icon=$ID
StartupWMClass=$ID
Terminal=false
Categories=Game;Simulation;
ENTRY
command -v update-desktop-database >/dev/null && update-desktop-database -q "$SHARE/applications" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$SHARE/icons/hicolor" 2>/dev/null || true
echo "desktop entry: $SHARE/applications/$ID.desktop"
