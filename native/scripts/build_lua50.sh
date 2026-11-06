#!/bin/bash
# Builds the scripting language for the game and its tests: Lua 5.0.2 (third_party/lua-5.0.2) as a static library, built as C
# with one pointer reserved before every Lua state (LUA_USERSTATE; the script host keeps the state's thread object there), and
# build/generated/lua_compat.inc, the Lua 4 compatibility script as the bytes the host compiles in.
#   scripts/build_lua50.sh [out dir, default build/lua50]      LUA_OPT="-O0 -g ..." overrides the compile flags
# Needs clang, ar and xxd.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-build/lua50}
LUA=../third_party/lua-5.0.2
mkdir -p "$OUT" build/generated
if [ ! -f build/generated/lua_compat.inc ] || [ $LUA/etc/compat.lua -nt build/generated/lua_compat.inc ]; then
  xxd -i < $LUA/etc/compat.lua > build/generated/lua_compat.inc
fi
OBJS=""; CHANGED=""
for c in $LUA/src/*.c $LUA/src/lib/*.c; do
  o=$OUT/$(basename "$c").o
  OBJS="$OBJS $o"
  if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
    clang ${LUA_OPT:--O1 -g} -w "-DLUA_USERSTATE=void*" -I$LUA/include -I$LUA/src -c "$c" -o "$o"
    CHANGED=1
  fi
done
if [ -n "$CHANGED" ] || [ ! -f "$OUT/liblua50.a" ]; then rm -f "$OUT/liblua50.a"; ar rcs "$OUT/liblua50.a" $OBJS; fi
