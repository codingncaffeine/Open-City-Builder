#!/usr/bin/env bash
# The session tools/run-original.sh runs inside tools/nested.sh: starts the original program in the test bed's
# prefix, watches it, takes the probes and writes how long it lived to $bed/logs/lifetime.txt.
# Everything it needs arrives in the environment (bed, OB_ARGS, seconds, out, shot_after, OB_PROBE_DIR).
set -uo pipefail
eval "args=($OB_ARGS)"
export HOME="$bed/home" WINEPREFIX="$bed/prefix"
mkdir -p "$bed/logs"

# Refuses to run while any user folder of the prefix points outside the test bed.
while IFS= read -r -d '' link; do
  target="$(readlink -f "$link")"
  [[ "$target" == "$bed"/* ]] || { echo "prefix link escapes the test bed: $link -> $target" >"$bed/logs/lifetime.txt"; exit 1; }
done < <(find "$WINEPREFIX/drive_c/users" -mindepth 2 -maxdepth 2 -type l -print0 2>/dev/null)

# The game counts as running while ANY process of that exact name runs in this prefix: the launcher is not the
# game, and the user may run their own copy at the same time in another prefix.
game_running() {
  local p
  for p in $(pgrep -x "SimCity 4.exe" 2>/dev/null || true); do
    tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$WINEPREFIX" && return 0
  done
  return 1
}
# Xwayland has no readable root window: capture through the nested KWin's screenshot interface instead.
shoot() { spectacle -b -n -f -o "$1" >/dev/null 2>&1 || echo "capture failed: $1" >>"$bed/logs/captures.txt"; }
# The X clients the window manager knows, with their titles and sizes.
windows() {
  local id
  for id in $(xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -oE '0x[0-9a-f]+'); do
    printf '%s %s %s\n' "$id" "$(xprop -id "$id" WM_NAME 2>/dev/null | cut -d= -f2-)" \
      "$(xprop -id "$id" _NET_FRAME_EXTENTS WM_NORMAL_HINTS 2>/dev/null | grep -oE 'size: [0-9]+ by [0-9]+' | head -1)"
  done
}
rm -f "$bed/logs/captures.txt"

cd "$bed/game/Apps"
if [[ -n "${OB_WINEDBG_SCRIPT:-}" ]]; then
  # Under Wine's debugger, running the given command file (breakpoints, backtraces).
  wine winedbg --file "$OB_WINEDBG_SCRIPT" "Z:${PWD//\//\\}\\SimCity 4.exe" "${args[@]}" >"$bed/logs/game.out" 2>&1 &
else
  wine "SimCity 4.exe" "${args[@]}" >"$bed/logs/game.out" 2>&1 &
fi
launcher=$!
for (( t = 0; t < 120; t++ )); do game_running && break; sleep 0.5; done
if ! game_running; then
  echo "game process never appeared" >"$bed/logs/lifetime.txt"
else
  [[ -n "${OB_PROBE_DIR:-}" ]] && mkdir -p "$OB_PROBE_DIR"
  # OB_ACTIONS=file drives the game: lines "wait SECONDS", "shot PNG", or "input ARGS" (OB_INPUT_EXE, a Win32
  # helper run in the same prefix, tools/obinput.c: click/rclick X Y, drag/rdrag X0 Y0 X1 Y1, move X Y, wheel N, key VK).
  if [[ -n "${OB_ACTIONS:-}" ]]; then
    (
      while read -r verb rest; do
        case "$verb" in
          wait) sleep "$rest" ;;
          shot) shoot "$rest"; echo "$(date +%s.%N) shot $rest" >>"$bed/logs/actions.txt" ;;
          input) eval "wine \"\$OB_INPUT_EXE\" $rest" </dev/null >>"$bed/logs/actions.txt" 2>&1 ;;
          ''|'#'*) ;;
        esac
      done <"$OB_ACTIONS"
    ) &
  fi
  gone=0
  for (( t = 1; t <= seconds; t++ )); do
    sleep 1
    if game_running; then gone=0; else gone=$(( gone + 1 )); (( gone >= 3 )) && break; fi
    if [[ -n "${OB_PROBE_DIR:-}" ]] && (( t % 15 == 0 )); then
      windows >"$OB_PROBE_DIR/windows-$t.txt" 2>&1
      shoot "$OB_PROBE_DIR/shot-$t.png"
    fi
    [[ -n "$out" ]] && (( t == shot_after )) && shoot "$out"
  done
  if (( t > seconds )); then
    echo "game alive after $seconds s" >"$bed/logs/lifetime.txt"
  else
    echo "game exited about $(( t - gone )) s after it appeared" >"$bed/logs/lifetime.txt"
  fi
fi
wineserver -k 2>/dev/null || true
wait "$launcher" 2>/dev/null || true
