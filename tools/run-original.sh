#!/usr/bin/env bash
# Runs the original program from the local test bed, headless but GPU-accelerated: a private nested KWin with
# Xwayland (tools/nested.sh), the test bed's own Wine prefix and HOME, sound made impossible (the audio wall is
# proven before every start), nothing on the user's desktop. Reports how long the game lived; captures the screen.
#
#   tools/run-original.sh [--hook] [--plain | --driver OpenGL|DirectX|Software] [--seconds N] [--shot-after N] [--out PNG]
#                         [--size WxH] [-- extra game args]
#
# --plain passes exactly the store launcher's windowed arguments. OB_PROBE_DIR=dir adds the window list and a
# screenshot every 15 s. OB_WINEDBG_SCRIPT=file runs the game under winedbg with that command file (a Windows
# path, e.g. Z:\...\cmds.txt). The test bed lives in scratch/testbed/ (gitignored): game/ is a copy of the install,
# prefix/ the Wine prefix, home/ the private HOME, logs/ the output.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bed="$root/scratch/testbed"
seconds=60 shot_after=0 out="" size=1280x720 driver=OpenGL plain="" hook=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --hook) hook=1; shift ;;
    --seconds) seconds="$2"; shift 2 ;;
    --shot-after) shot_after="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --driver) driver="$2"; shift 2 ;;
    --plain) plain=1; shift ;;
    --) shift; break ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -f "$bed/game/Apps/SimCity 4.exe" ]] || { echo "no test bed at $bed/game" >&2; exit 1; }

w="${size%x*}" h="${size#*x}"
if [[ -n "$plain" ]]; then
  args=(-CPUCount:1 -w "$@")   # exactly the store launcher's windowed task
else
  args=(-w "-d:$driver" -Intro:off -CPUCount:1 -CustomResolution:enabled "-r${w}x${h}x32" "$@")
fi

# A private HOME: Wine links the prefix's Documents/Desktop/... to $HOME's folders, and the game writes its user
# folder into Documents. With the real HOME that lands in the user's own Documents (it did, 2026-10-28).
mkdir -p "$bed/home" "$bed/logs"
export HOME="$bed/home" WINEPREFIX="$bed/prefix" WINEDEBUG="${WINEDEBUG:--all}"
# No desktop entries, no Mono/Gecko prompts. Audio: silence is guaranteed by tools/nested.sh (no sound devices or
# audio servers exist inside it); the game still gets a sound device, ALSA's null PCM (tools/silent-alsa.conf),
# because it quits without one. Only Wine's ALSA driver is left enabled, so that is the device it finds.
export WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree=d;mshtml=d;winepulse.drv=d;wineoss.drv=d${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
# ALSA splits ALSA_CONFIG_PATH on spaces, and this folder's name has one: hand it a copy at a space-free path.
ALSA_CONFIG_PATH="/tmp/openbuilder-$(id -u)-silent-alsa.conf"
cp "$root/tools/silent-alsa.conf" "$ALSA_CONFIG_PATH"
export ALSA_CONFIG_PATH
[[ -d "$WINEPREFIX/drive_c" ]] || DISPLAY= wineboot -i >/dev/null 2>&1
# On a machine with an NVIDIA card and an integrated GPU, the nested session's GL otherwise lands on the
# integrated one, whose driver rejects the game's command streams; PRIME offload sends the game to the NVIDIA card.
if [[ -e /dev/nvidia0 && -z "${OB_NO_PRIME:-}" ]]; then
  export __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia __VK_LAYER_NV_optimus=NVIDIA_only
fi

# Never start the game until the audio wall is proven, this run, with its control.
"$root/tools/check-silence.sh" >"$bed/logs/silence-check.txt" 2>&1 || {
  echo "audio wall check FAILED, not starting the game:" >&2; cat "$bed/logs/silence-check.txt" >&2; exit 1; }

# --hook: our build/vc71/version.dll sits beside the game for this run only, ahead of Wine's own VERSION.dll; without
# it the folder must hold no version.dll, so a plain run is the untouched original.
apps="$bed/game/Apps"
rm -f "$apps/version.dll" "$apps/ob-hooks.txt"
if [[ -n "$hook" ]]; then
  [[ -f "$root/build/vc71/version.dll" ]] || { echo "no build/vc71/version.dll: run tools/build.sh" >&2; exit 1; }
  cp "$root/build/vc71/version.dll" "$apps/version.dll"
  WINEDLLOVERRIDES="$WINEDLLOVERRIDES;version=n,b"
fi

export bed seconds out shot_after OB_PROBE_DIR="${OB_PROBE_DIR:-}" OB_WINEDBG_SCRIPT="${OB_WINEDBG_SCRIPT:-}"
export OB_ACTIONS="${OB_ACTIONS:-}" OB_INPUT_EXE="${OB_INPUT_EXE:-}"
export OB_ARGS="$(printf '%q ' "${args[@]}")" OB_WIDTH="$w" OB_HEIGHT="$h"
rm -f "$bed/logs/lifetime.txt"
"$root/tools/nested.sh" "$root/scratch/nested" $(( seconds + 120 )) "$root/tools/original-session.sh" | grep -v '^nested: compositor exit 0$' || true
cat "$bed/logs/lifetime.txt" 2>/dev/null || echo "no lifetime recorded (see scratch/nested/kwin/kwin.txt)"
if [[ -n "$hook" ]]; then
  if [[ -f "$apps/ob-hooks.txt" ]]; then
    tr -d '\r' <"$apps/ob-hooks.txt" | tee "$bed/logs/ob-hooks.txt" | sed 's/^/hook: /'
  else
    echo "hook: no ob-hooks.txt written (the hook DLL did not load or did not report)"
  fi
  rm -f "$apps/version.dll" "$apps/ob-hooks.txt"
fi
echo "log: $bed/logs/game.out"
