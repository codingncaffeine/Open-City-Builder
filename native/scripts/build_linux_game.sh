#!/bin/bash
# Builds build/sc4_game_linux: the game on Linux (SDL3 window and audio, OpenGL 3.3 core through cGLGDriver).
# Incremental: one object per source of scripts/linux_game_link_list.txt under build/linux_obj (debug: build/linux_obj_debug).
#   scripts/build_linux_game.sh [debug]      debug: -O0 + AddressSanitizer
#   JOBS=N scripts/build_linux_game.sh      N parallel compiles (default: a quarter of the cores, so the desktop stays usable)
#   SC4_GAME_CONTENTS=<folder with GameData/> build/sc4_game_linux [--shot out.png [--frames N] | --script scenario.txt]
# Needs clang, ar, xxd, libc++, SDL3, libepoxy, FreeType, libpng and libmpg123 (the Mac-derived classes assert libc++'s layouts).
set -e
cd "$(dirname "$0")/.."
MODE=${1:-fast}
if [ "$MODE" = debug ]; then OBJ=build/linux_obj_debug; OUT=build/sc4_game_linux_debug; OPT="-O0 -g -fno-omit-frame-pointer -fsanitize=address"; LD="-fsanitize=address"
else OBJ=build/linux_obj; OUT=build/sc4_game_linux; OPT="-O1 -g"; LD=""; fi
mkdir -p $OBJ
PKG="$(pkg-config --cflags sdl3 epoxy freetype2 libpng libmpg123)"
CF="-std=c++20 -stdlib=libc++ -include cstdlib -include cmath -include platform/linux_char_traits.h -Dst_atimespec=st_atim -Dst_mtimespec=st_mtim -Dst_ctimespec=st_ctim -D_CS_DARWIN_USER_TEMP_DIR=_CS_PATH $OPT -Wno-overloaded-virtual -w -Isrc -Iscripts/tests/stubs $PKG"
# The window and taskbar icon (art/logo-256.png) is compiled into the game; a new logo rebuilds the file that includes it.
mkdir -p build/generated
if [ ! -f build/generated/app_icon.inc ] || [ ../art/logo-256.png -nt build/generated/app_icon.inc ]; then
  (cd ../art && xxd -i -n app_icon_png logo-256.png) > build/generated/app_icon.inc
  rm -f $OBJ/src_app_linux_game_main_linux.cpp.o
fi
CF="$CF -Ibuild/generated"
# The scripting language: Lua 5.0.2 as a static library, and its Lua 4 compatibility script compiled into the host.
LUA=../third_party/lua-5.0.2
CF="$CF -I$LUA/include -I$LUA/src"
[ $LUA/etc/compat.lua -nt build/generated/lua_compat.inc ] && rm -f $OBJ/src_lua_SCLuaInstance.cpp.o
LUA_OPT="$OPT" bash scripts/build_lua50.sh $OBJ/lua50
LIST=scripts/linux_game_link_list.txt
OBJS=""
for s in $(grep -v '^#' $LIST); do OBJS="$OBJS $OBJ/$(echo "$s" | tr '/' '_').o"; done
FAILED=$(mktemp)
for s in $(grep -v '^#' $LIST); do
  o=$OBJ/$(echo "$s" | tr '/' '_').o
  if [ -f "$o" ] && [ "$o" -nt "$s" ] && [ -z "$(find src -name '*.h' -newer "$o" | head -1)" ]; then continue; fi
  echo "$s $o"
done | xargs -r -P "${JOBS:-$(( $(nproc) > 4 ? $(nproc) / 4 : 1 ))}" -L 1 sh -c 'clang++ '"$CF"' -c "$0" -o "$1" 2>"$1.err" || echo "$0" >>'"$FAILED"
if [ -s "$FAILED" ]; then
  echo "COMPILE ERRORS:"; for s in $(cat "$FAILED"); do echo "--- $s"; grep -m5 error "$OBJ/$(echo "$s" | tr '/' '_').o.err"; done
  rm -f "$FAILED"; exit 1
fi
rm -f "$FAILED"
clang++ -stdlib=libc++ $LD $OBJS $OBJ/lua50/liblua50.a $(pkg-config --libs sdl3 epoxy freetype2 libpng libmpg123) -o $OUT
echo "built $OUT"
