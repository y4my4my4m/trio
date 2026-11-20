# Roadmap

## Known rough edges

- Battle mode: RAIDER and WARLORD are experimental. They are bosses moved by the
  battle layer between their own attacks: they stand a block high on
  slopes, walk with standing frames, and their sprites break up at times.
- Battle mode: the rebuilt arenas are made of one floor block and one wall
  block per stage, over the stage's background layer (plain where it has
  nothing). A special weapon's shot flies straight once the arena takes it
  over (far from its owner, when his shot table is full).
- Battle mode: a moving platform's collision moves in 16 px steps while it
  is drawn every pixel; CPU players do not plan routes over them.
- Widescreen: an object placed in the extra columns of a room entered from
  above or below (or at a stage start) appears on the first step sideways,
  not at once. Rooms with a locked camera show level data beyond the room.
- The recompiled build is tested on Linux. CI builds the interpreter-only
  configuration on Linux, macOS and Windows; the prebuilt Windows download
  is that configuration.
- Only the USA ROMs are supported.

## Next

1. Releases
   - Overrides (widescreen, PORT OPTIONS, battle mode) in the interpreter
     build, so a prebuilt binary has every feature without recompiling.
   - Prebuilt binaries for Linux and macOS from CI, as for Windows.
   - The recompiled build in CI, with a ROM supplied to the runner.
2. Single player
   - Full playthroughs of the three games in widescreen, every scripted
     scene checked against the classic width.
   - Coverage seeds from those playthroughs (more code recompiled, less
     interpreted).
   - Per-game settings; XDG / AppData paths for `trio.ini` and `saves/`.
3. Battle mode
   - Bosses: slopes, walking frames, hit boxes from their sprites.
   - Arenas drawn by hand per stage instead of one layout rule.
   - Player colors for fighters of the same character.
   - CPU players that use moving platforms and one-way drops.
   - Online play (machine states can be cloned and restored, which
     rollback needs).
4. Later
   - Japan and Europe ROM versions.
   - CRT shader, integer-scaled widescreen presets.
