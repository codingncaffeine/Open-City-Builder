# Open City Builder: the native build

This folder is the playable native game: a C++20 decompilation of the original that builds and runs on Linux with
SDL3 and OpenGL 3.3. It started from [DeSCAi4](https://github.com/CadenKalez/DeSCAi4) by CadenKalez and contributors,
a hand-written decompilation of the later Mac release (GPLv3). Their README, with the history of how the
decompilation was made, is kept as [UPSTREAM-README.md](UPSTREAM-README.md). On top of it are the Linux host, the
game loop, and the systems and screens ported since. See the [main README](../README.md) for what plays today and
what is still missing.

No game data is included: the game reads your own copy's data folder and never changes it.

## Building and running

Needs clang with libc++, SDL3, libepoxy, FreeType, libpng and libmpg123; FFmpeg is optional (intro movies).

```bash
scripts/build_linux_game.sh                  # incremental; installs ../bin/open-city-builder
../bin/open-city-builder /path/to/game/folder
scripts/build_linux_game.sh debug            # with AddressSanitizer
```

## Tests

```bash
scripts/run_tests.sh                         # every unit test, about three minutes
scripts/run_tests.sh sc4_zonedeveloper ...   # some of them
```

Each test is built the way the game is (clang, libc++) with AddressSanitizer, UndefinedBehaviorSanitizer and leak
detection, so a leak fails a test like a wrong value. Shared sources compile once and the tests run in parallel
(`JOBS=n` sets how many; a quarter of the cores by default). Logs land in `$TMPDIR/sc4-tests/`.

## Layout

- `src/sc4`, `src/gz`, `src/rz`, `src/s3d`, `src/lua`, `src/audio`, `src/sc4audio`, `src/html`, `src/render`: the
  decompiled game, one class per file, each function with the address it came from in a comment. The original's
  object layouts, quirks and bugs are kept; where the port departs from the original, the comment says why.
- `src/app`: the native host. It stands in where the original reached into its platform layer, wires the decompiled
  systems together and runs the game loop.
- `src/platform`: the small shims the Mac-derived sources need on Linux.
- `scripts/`: the build scripts, the unit tests (`scripts/tests`) and their list (`scripts/run_tests.sh`).
- `notes/`, `docs/`: upstream's decompilation notes.

## License

GNU General Public License v3.0 (`LICENSE.txt`).
