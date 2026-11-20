# Trio: a native port of three 16-bit action platformers

Three SNES action platformers (USA versions), running
natively on Linux, macOS and Windows. The games are identified by the SHA-1
of their ROM images (section 2) and are called game 1, game 2 and game 3
(G1, G2, G3).

The game code is recompiled from your own ROMs to C at build time and runs
on a built-in SNES platform layer (65C816, PPU, SPC700 / DSP, Cx4). No ROM
and no game data is included: you need your own dumps of the cartridges.

| | |
|---|---|
| Widescreen | 16:10, 16:9 or 2:1 in stages, in all three games |
| Save states | quick slot, 10 slots and rotating autosaves, with thumbnails |
| Rewind, fast forward | hold a key |
| Controls | keyboard and gamepad rebinding, auto-charge, turbo fire |
| Game select screen | one program for the three games |
| Battle mode | 2-4 players (people or CPU) fighting as the hero, Blade and others, in arenas from the three games |

## 1. Requirements

- CMake 3.16 or newer
- a C11 and C++17 compiler (GCC, Clang or MSVC)
- SDL2 (development package)
- Python 3 (the build disassembles and recompiles the ROMs with it)
- your own ROM of at least one of the games (USA)

| System | Install |
|---|---|
| Debian, Ubuntu | `sudo apt install build-essential cmake libsdl2-dev python3` |
| Fedora | `sudo dnf install gcc gcc-c++ cmake SDL2-devel python3` |
| Arch, Manjaro | `sudo pacman -S base-devel cmake sdl2-compat python` |
| macOS | `xcode-select --install`, then `brew install cmake sdl2` |
| Windows | Visual Studio 2022 (Desktop development with C++), CMake, Python 3, and SDL2 from vcpkg: `vcpkg install sdl2:x64-windows` |

## 2. Get the code and add your ROMs

```bash
git clone https://github.com/y4my4my4m/trio.git
```

```bash
cd trio
```

Copy your ROMs into the `rom/` folder before building. Any file name works
(`.sfc` or `.smc`, with or without a 512-byte copier header): each game is
recognized by its header. The table lists the default names. Any of the
three can be left out.

| File | Size | SHA-1 (no copier header) |
|---|---|---|
| `g1.sfc` | 1.5 MiB | `449a00631208fbcc8d58209e66d0d488674b7fb1` |
| `g2.sfc` | 1.5 MiB | `637079014421563283cded6aeaa0604597b2e33c` |
| `g3.sfc` | 2 MiB | `b226f7ec59283b05c1e276e2f433893f45027cac` |

Other versions (Japan, Europe, revisions) are not supported. ROMs and the C
code generated from them are never committed (`.gitignore`).

## 3. Build

Linux and macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j
```

Windows (a "x64 Native Tools" or PowerShell prompt, with vcpkg at `C:\vcpkg`):

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

```bash
cmake --build build --config Release -j
```

The first build takes a few minutes: each ROM found in `rom/` is
disassembled and recompiled to 2-4 MB of C.

- A ROM added to `rom/` later: run both commands again.
- A ROM somewhere else: `-DG1_ROM=/path/to/rom.sfc` (also `G2_ROM`,
  `G3_ROM`) on the first command.
- `-DTRIO_RECOMP=OFF` builds without any ROM. The games then run in the
  interpreter (correct, slower), without widescreen, the in-game PORT
  OPTIONS page and battle mode.

## 4. Run

From the project folder:

```bash
./build/trio
```

On Windows: `build\Release\trio.exe`.

The game select screen opens: Left / Right pick the game, then PLAY, BATTLE
MODE, OPTIONS or QUIT. To start one game directly, name its ROM:

```bash
./build/trio rom/g3.sfc
```

Settings are saved to `trio.ini` and save states to `saves/`, both in the
project folder (the folder that holds `rom/`).

## Controls

Default keys. Everything can be rebound in the menu (F1), and gamepads are
picked up automatically (hot-plug, up to four).

| Key | SNES button | In the games |
|---|---|---|
| Arrows | D-pad | move |
| Z | B | jump |
| X | Y | shoot (hold to charge) |
| C | A | dash |
| V | X | |
| A / S | L / R | previous / next weapon |
| Enter | Start | pause, confirm |
| Right Shift, Backspace | Select | |

| Key | Gamepad | |
|---|---|---|
| F1, Esc | Guide, Back + Start | port menu |
| Tab (hold) | Back + RB | fast forward |
| R (hold) | Back + LB | rewind |
| P / O | | pause / advance one frame |
| F5 / F9 | Back + Y / X | save / load the selected slot |
| F6 / F7 | | previous / next slot |
| Shift + F9 | | load the newest autosave |
| F11, Alt + Enter | | fullscreen |
| F12 | | screenshot (PNG) |
| Ctrl + R | | reset |

The port menu (F1) has the video options (scale, widescreen, filter,
scanlines), audio, key and gamepad bindings, save slots, rewind length and
the assists (auto-charge, turbo fire, infinite health and lives). The most
used ones are also in each game: OPTION MODE > PORT OPTIONS on its title
screen. The games themselves save with passwords, as on the console.

## Battle mode

BATTLE MODE on the game select screen: 2-4 fighters, people or CPU, with
lives, an optional time limit, items and special weapons. Every fighter is
a running copy of its own game, so the hero moves, dashes, wall-kicks and
charges exactly as in that game. Details and the setup page:
`docs/BATTLE.md`.

- HERO, HERO ARMOR, HERO FULL (each as G1, G2 or G3) and BLADE are the
  playable characters.
- RAIDER and WARLORD are experimental: they are the games' bosses, moved by the
  battle layer between their own attacks. Expect rough movement, graphics
  glitches and uneven balance. The setup page says so when one is picked.
- Second keyboard player (KEYS 2): W A S D, F jump, G shoot, H dash, Q / E
  weapon, J sword, Tab pause.

## Troubleshooting

| | |
|---|---|
| `no ROM: put a supported ROM in rom/` | No supported ROM was found in `rom/`. Check the SHA-1 above. |
| Widescreen is greyed out, battle mode does not start | The game was not recompiled: its ROM was missing when CMake ran, or the build used `-DTRIO_RECOMP=OFF`. Put the ROM in `rom/` and run both build commands again. |
| `Could NOT find SDL2` | Install the SDL2 development package (table above). On Windows pass the vcpkg toolchain file. |
| A save state is listed as incompatible | It was made by another build or ROM. |
| Wrong buttons on a gamepad | Port menu > Controls: layout by position or by label, or bind each button. |

## Development

```bash
ctest --test-dir build --output-on-failure
```

runs the CPU, PPU, APU, Cx4 and rewind unit tests (no ROM needed) and the
battle mode tests (skipped without ROMs).

```bash
./build/trio-verify rom/g3.sfc --frames 6000 --srm none
```

runs the interpreter and the recompiled code in lockstep and compares the
whole machine state after every frame.

```bash
./build/trio rom/g3.sfc --headless --frames 3000 --input script.txt --shot out.ppm --wav out.wav
```

runs without a window from an input script (`src/script.h`). `--interp`
(or `TRIO_RECOMP=0`) uses the interpreter only, `--widescreen N` (0-3)
overrides the setting, `--coverage FILE` records executed code addresses.

| | |
|---|---|
| `docs/ARCHITECTURE.md` | layers, memory map, how the recompiled code runs |
| `docs/CX4.md` | the Cx4 coprocessor (G2, G3) |
| `docs/WIDESCREEN.md` | what the widescreen changes in each game |
| `docs/ENHANCEMENTS.md` | the port menu's features |
| `docs/MENUS.md` | the in-game PORT OPTIONS page |
| `docs/LAUNCHER.md` | the game select screen |
| `docs/BATTLE.md` | battle mode |
| `ROADMAP.md` | what is planned and what is known to be rough |

## License

The port's own code is under the MIT license (`LICENSE`), except
`src/snes/dsp.c`: a C port of the S-DSP from blargg's snes_spc 0.9.0, under
the GNU LGPL 2.1 or later (`LICENSES/LGPL-2.1-or-later.txt`). Dear ImGui
(`third_party/imgui`) is under the MIT license
(`third_party/imgui/LICENSE.txt`).

The license covers this repository only. It contains no code or data from
the games: it needs ROM images that you dump from cartridges you own, and
the C code the build generates from them is yours to keep, not to
distribute. The games, their names and their characters belong to the
games' publisher; this project is not affiliated with or endorsed by the
games' publisher.
