# Game select screen

`./build/trio` without a ROM argument opens the game select screen over the
ROMs found in `rom/` (`g1.sfc`, `g2.sfc`, `g3.sfc`, rom/README.md).
A ROM named on the command line starts that game directly, as before.

The screen shows the title of the game under the cursor, its logo and its
opening song, with the port's own menu in place of the game's:

| | |
|---|---|
| Left / Right | change game (logo and song follow; the song starts over) |
| PLAY | that game's own title screen (GAME START, PASS WORD, OPTION MODE) |
| BATTLE MODE | the battle setup page (docs/BATTLE.md) |
| OPTIONS | the port menu (F1) |
| QUIT | |

The port menu's Game tab has a Game select button that comes back here from
a game or a match. Save slots are per game (`saves/` for G3, `saves/g1/`,
`saves/g2/`).

## How it works (`src/arena/launcher.c`)

Each game's machine is booted once from power-on to its title menu (START
skips the intro; G1 shows its logo first and takes a second START) and then
held there: it is not run any more, so the attract timer never runs out. The
game under the cursor is booted first; the others boot in the background,
100 frames per displayed frame.

The song is on a chip of the screen's own (`start_song`, `ArenaSound`): a
copy of the held machine runs the game's music routine with the song the
title had loaded (`$7E:FFFD`: `$1F` in G1, `$25` in G2 and G3), and that
chip is kept as the song starts. It plays from there when the game is
selected and again 1.5 s after it ends (G1's lasts 25 s, G2's 52 s, G3's
loops). The held machines' own chips could not play it: START fades the
opening song out in G2 and G3 (driver command `$F1`) and their title menus
are silent, and G1's title theme is 14 s in when its machine is held. The
menu sounds go to the same chip.

The backdrop is one frame of a copy of the held machine drawn without BG3
(the title menu text), with the bottom band blanked (the menu cursor sprite);
the G2 and G3 numerals are sprites, so OBJ stays on. The text uses the BG3
font and the two menu palettes of the G3 title (else G2's; G1's font tiles
are not ASCII), `src/arena/ui.c`.

PLAY gives the frontend a copy of the held machine, sound chip included, as
the game left it (`launcher_resume` puts the chip's cycle count on the CPU's
clock).
