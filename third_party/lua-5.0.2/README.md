# Lua 5.0.2

The interpreter and standard libraries of the lua-5.0.2.tar.gz release (`src/`, `src/lib/`, `include/`) and its
Lua 4 compatibility script (`etc/compat.lua`); `COPYRIGHT` is its licence (MIT). The original program embeds Lua 5.0
and runs its scripts with this compatibility script loaded, so the game links the same interpreter.

Left out: the stand-alone interpreter and compiler (`src/lua`, `src/luac`), the test hooks (`src/ltests.c`) and the
dynamic loader (`src/lib/loadlib.c`), none of which the game uses. No file is changed.

The game builds it with `LUA_USERSTATE` defined as a pointer, which reserves one pointer just before every Lua state;
the script host keeps each state's thread object there, as the original does.
