# Open City Builder

![Open City Builder](art/banner.jpg)

A decompilation of a classic 2003 city-building game, rebuilt in C++ so it runs natively on Linux at any resolution
and becomes easy to mod.

**Status:** playable, not finished. The native Linux build plays the intro, the region view and its cities. It loads
the original's saved cities, plays them and saves them back. Running natively:

- **The city:** growth, traffic with cars, trains, buses and working stations, power and water, the budget and
  taxes, advisors, the news ticker, data views, query dialogs and neighbor deals.
- **Events:** fires, crime and riots, a mayor rating that follows the city, night lights, and every disaster on the
  menu, including pipe bursts.

Still missing:

- **Simulation:** subway stations, arrests, lots refitted to moved ground, and the landfill, port and toxic waste
  services.
- **Screens:** graphs, cheats, tutorials, the personal-sim and driving modes, the snapshot album, the subway view,
  laying new pipes, and pipe junctions in the underground view (the other pipe pieces show).

There is no release yet.

## You need your own copy

Open City Builder contains no game data, art, sound, text or executable code from the original. To play it you need
your own legally purchased copy of the game (the PC release, version 1.1.641, the current digital edition). The build
reads that copy's data folder and never changes it. Nothing from that copy may ever be added to this repository.

## Building and running (Linux)

Needs clang with libc++, SDL3, libepoxy, FreeType, libpng and libmpg123. FFmpeg is optional (the intro movies play
when it is installed).

```bash
native/scripts/build_linux_game.sh          # incremental; installs bin/open-city-builder
bin/open-city-builder /path/to/game/folder   # the install folder: its .dat files, Fonts/, Regions/ ...
```

`native/scripts/build_linux_game.sh debug` builds with AddressSanitizer. The unit tests run with
`native/scripts/run_tests.sh`.

## How it works

The native game is a C++20 decompilation: each function of the original is transcribed into readable C++ with the
address it came from in a comment. The original's object layouts, quirks and bugs are kept. Where the original
reached into its Windows or Mac platform layer, a small native host stands in: SDL3 for the window, input and audio,
and OpenGL 3.3 for drawing.

Every ported piece has a unit test, built with AddressSanitizer and UndefinedBehaviorSanitizer. Scripted runs of the
real game check it against the original's own saved cities. Where behavior is in doubt, the original is the
reference.

## Layout

- `native/`: the playable native build. It is based on [DeSCAi4](https://github.com/CadenKalez/DeSCAi4) by
  CadenKalez and contributors, a C++20 decompilation of the later Mac release (GPLv3). On top of it are our Linux
  host, the game loop, and the systems ported since. Its own README and notes are kept as they were upstream.
- `src/`, `hook/`, `tools/`: the first route, a byte-exact rewrite of the PC release that a loader swapped into the
  original program. It was retired in favor of playability; the compare and inventory tools are still used.
- `third_party/`: the open-source libraries the original was built with (STLport 4.6, Lua 5.0.2), each under its own
  license.

## License

GNU General Public License v3.0 (see `LICENSE`), as `native/` is GPLv3.

## Roadmap

Finish the simulation → the remaining screens → a Linux release (tarball and AUR) and a Windows build → modding
(your own buildings and props from open formats, game rules as text, a mod manager) → our own content set, so the
game runs without the original data → a web build.
