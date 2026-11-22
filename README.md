# Open City Builder

![Open City Builder](art/banner.jpg)

A decompilation of a classic 2003 city-building game, rebuilt in C++ so it runs natively on Linux at any resolution
and becomes easy to mod.

**Status:** playable, not finished. The native Linux build plays the intro, the region view and its cities. It loads
the original's saved cities, plays them and saves them back. Running natively:

- **The city:** growth, traffic with cars, trains, buses and working stations, subway stations, power, water with
  pipes you can lay and see underground, landfills, airports and seaports, street props and sidewalks on new roads,
  signs and labels, the budget and taxes down to a single building, advisors, the news ticker, data views, query
  dialogs, the city graphs and neighbor deals.
- **Events:** fires, crime with arrests and riots, toxic spills and potholes, a mayor rating that follows the city,
  night lights, and every disaster on the menu, including pipe bursts.
- **Sound:** the game's own sound system: the radio stations and their playlists, effects placed in the city and
  heard left and right, sirens, the city's ambience, and the audio options.
- **Growth:** buildings decline, are abandoned and are taken over again; farms grow and plant their fields, small lots
  merge into larger ones and large lots split, and the ground is levelled for new buildings; power lines are laid with
  the original's pole tool, and helicopters, planes and the blimp fly.
- **Building:** roads shape the ground under them; bridges (with the bridge chooser, suspension wires and supports),
  tunnels, highways with their raised decks, overpasses and on-ramps, one-way roads with their arrows, and the subway
  tool; construction crews build new roads piece by piece and lots get their workers.
- **People:** personal sims move in, form their opinions and commute; you can take over a vehicle, drive it
  and accept the training missions.
- **Around the city:** the tutorials, the cheat box, snapshots and their album, starting a city over, the loading
  screen, the compass and the alert border, the game's own fonts and cursors, and every data view.

Still missing:

- **Driving and personal sims:** the mission dialog's text and status panel, chase and coverage missions, the
  personal-sim balloons and panels.
- **Screens:** making and importing regions, and a few cheats.
- **Display:** text is soft at fractional interface scales.

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

`native/scripts/build_linux_game.sh debug` builds with AddressSanitizer. `native/scripts/run_tests.sh` builds and
runs every unit test (about three minutes; `run_tests.sh NAME...` runs some).

## How it works

The native game is a C++20 decompilation: each function of the original is transcribed into readable C++ with the
address it came from in a comment. The original's object layouts, quirks and bugs are kept. Where the original
reached into its Windows or Mac platform layer, a small native host stands in: SDL3 for the window, input and audio,
and OpenGL 3.3 for drawing.

Every ported piece has a unit test, built with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection: a
leak fails its test like a wrong value. Scripted runs of the real game check it against the original's own saved
cities. Where behavior is in doubt, the original is the
reference.

## Layout

- `native/`: the playable native build. It is based on [DeSCAi4](https://github.com/CadenKalez/DeSCAi4) by
  CadenKalez and contributors, a C++20 decompilation of the later Mac release (GPLv3). On top of it are our Linux
  host, the game loop, and the systems ported since. Upstream's README is kept as `native/UPSTREAM-README.md`.
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
