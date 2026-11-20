# Architecture

Targets (USA, rom/README.md): game 1 (LoROM FastROM, 1.5 MiB), game 2
(LoROM + Cx4, 1.5 MiB) and game 3 (LoROM + Cx4, 2 MiB), no battery RAM.
`Snes.game` (from the ROM header's checksum word at $7FDE: 55FB, 09B7,
4055) says which game a machine runs, and `Snes.has_cx4` maps the Cx4
window.

## Layers

```
src/main.c          SDL2 frontend, headless scripted runner, coverage dump
src/script.c        input script parser (main.c, tools/verify.c)
src/snes/           platform layer
  snes.c            bus, LoROM map, Cx4 window, $21xx/$42xx/$43xx I/O, DMA/HDMA, scheduler
  cx4.c             Cx4 coprocessor (HLE), see CX4.md
  cpu.c cpu_ops.h   65C816 interpreter; cpu_ops.h is shared with the recompiled code
  ppu.c             scanline PPU renderer
  apu.c dsp.c       SPC700 + S-DSP (the chip's boot ROM is not included: its
                    transfer protocol is run in C, apu.c ipl_step)
src/game/           recompiled code runtime (recomp_rt.c, one dispatch table per game),
                    widescreen overrides (widescreen.c for G3, g1_ws.c, g2_ws.c)
                    and ROM constants (ws_rom.c), in-game PORT OPTIONS (port_menu.c);
                    gen/gN/ is generated at build time
src/arena/          game select screen (launcher.c, LAUNCHER.md), its text (ui.c),
                    battle mode (BATTLE.md): game RAM interface (trio.c), match
                    rules (arena.c), CPU players (abot.c), screens and picture
                    (battle.c)
src/port/           frontend features: settings, widescreen/input glue (enhance.c),
                    rewind, save slots, ImGui menu (overlay.cpp)
third_party/imgui/  Dear ImGui (MIT)
tools/disasm.py     tracing disassembler -> build/disasm/gN/{bank_XX.asm,map.json}
tools/recomp.py     65816 -> C generator (reads map.json; --prefix per game)
tools/gN/           per game: coverage.txt (executed PCs, seeds for disasm.py),
                    hints.txt, overrides.txt (ROM PCs replaced by C, docs/WIDESCREEN.md)
tools/cover.py      records coverage seeds (attract mode, random play) through sp.py
tools/snespy.c, sp.py  research harness: the emulator as a Python (ctypes) library,
                    with breakpoints and WRAM / I/O write watches
tools/verify.c      trio-verify: interpreter vs recompiled lockstep checker
tools/probe.c       trio-probe: WRAM/VRAM/OAM dumps for RAM research
tools/battle_shot.c trio-battle-shot: headless battle mode frames
tests/              CPU/PPU/APU/Cx4/rewind unit tests, battle mode tests (ROM)
```

## Memory map

| Banks | Address | |
|---|---|---|
| $00-$3F, $80-$BF | $0000-$1FFF | WRAM mirror |
| | $2100-$5FFF | I/O |
| | $6000-$7FFF | Cx4 RAM and registers (G2, G3) |
| | $8000-$FFFF | ROM (LoROM, 32 KiB per bank) |
| $7E-$7F | all | WRAM |

## Execution model

The interpreter runs the unmodified ROM; before each instruction the exec
hook looks up PB:PC and (M,X) and runs the recompiled C from there when it
exists. Recompiled code uses the interpreter's instruction semantics and bus
timing, so both paths produce the same machine state (checked by
`trio-verify`).

Each game's code is generated separately with its own symbol prefix (g1_,
g2_, g3_) and dispatch table (`rt_dispatch_g1` ...); `rt_install` picks the
table of `Snes.game`. A game whose ROM was missing at build time gets an empty
table and runs in the interpreter.

G3's NMI vector points into WRAM ($00:1FF3, a jump the game writes at boot), and
its Cx4 command helper lives in WRAM at $7E:26A0. Code in RAM is always
interpreted. Across the lockstep runs about 97% of instructions execute as
recompiled C in G2 and G3, 99.9% in G1.

## Recompilation seeds

The games dispatch most of their object code through pointer tables that the
static tracer cannot size on its own, so `tools/gN/coverage.txt` supplies the
entry points: the union of `--coverage` runs over the attract mode, the menus
and random play. Code not reached by the seeds runs in the interpreter, which
is always correct, only slower. To extend it:

```bash
./build/trio rom/g3.sfc --headless --coverage tools/g3/coverage.txt --frames 20000 --input my_run.txt
python3 tools/cover.py 1 --frames 30000      # G1: attract mode + random play
cmake -S . -B build && cmake --build build
```
