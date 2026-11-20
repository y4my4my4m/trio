# In-game PORT OPTIONS

The title OPTION MODE screen gets a PORT OPTIONS entry between SOUND MODE and
EXIT (`src/game/port_menu.c`, recompiled build only). It opens a page drawn
in the game's own font and box tiles, with the most used port settings. Up and
Down move, Left and Right change a value, B/A/Start confirm, Y goes back.
MORE (F1) opens the full port menu. Changes are saved to `trio.ini` right away
and apply as if made in the F1 menu.

## Option mode loop (00:E9xx-00:EDxx)

| | |
|---|---|
| $7E:FF80 | cursor: 0-5 key config, 6 sound mode, 7 exit |
| $7E:FF81 | cursor at the start of the frame |
| $AC / $AD | new presses, low / high joypad byte |
| 00:EA2F | loop head (copies the cursor, key repeat timer) |
| 00:EA42-00:EA7D | Up/Down with wraparound over 0-7 |
| 00:EA87-00:EAC2 | on a change: old item redrawn plain, new item highlighted, via string ids at 00:B662 (+1 = highlighted) |
| 00:EACB | handler per item, table at 00:EAD4 |
| 00:EB7D | `JSR yield; JMP 00:EA2F` |

Text is BG3 (tilemap VRAM $0800, 32 x 64). Font tiles are ASCII codes; palette
0 is plain text, 2 is the highlight, 5 is values, and 3 is the key config box
(tiles $AA-$AD).

## Overrides

| PC | Function | Change |
|---|---|---|
| 00:EA42 | `ovr_OptCursor` | draws the entry row every frame; moves into and out of item 8 in the order 6, 8, 7; runs the page while it is open (then yields) |
| 00:EA8A | `ovr_OptDrawOld` | skips the plain redraw when the old item is 8 |
| 00:EAA7 | `ovr_OptDrawNew` | skips the highlight redraw when the new item is 8 |
| 00:EACB | `ovr_OptDispatch` | item 8: confirm opens the page |

All other cursor moves, the key config, sound mode and exit stay on the ROM
path. The page saves BG3 rows 0-27 when it opens and puts them back when it
closes.

## G1 and G2

The same loop runs in both games (`games[]` in `src/game/port_menu.c`):

| | G1 | G2 | G3 |
|---|---|---|---|
| items | 0-5 key config, 6 sound mode, 7 BGM test, 8 S.E. test, 9 exit | as G3 | 0-5 key config, 6 sound mode, 7 exit |
| PORT OPTIONS | item 10, between 8 and 9 | item 8, between 6 and 7 | item 8, between 6 and 7 |
| new presses (B Y Sel Start U D L R / A X L R) | $AC / $AB | $AD / $AC | $AD / $AC |
| cursor move (`ovr_G1OptCursor` ...) | 80:EAB9 | 00:EACB | 00:EA42 |
| old item redraw | 80:EAFE | 00:EB13 | 00:EA8A |
| new item redraw | 80:EB1B | 00:EB30 | 00:EAA7 |
| item dispatch | 80:EB3F | 00:EB54 | 00:EACB |
| wait a frame, loop | 80:EB39 | 00:EB4E | 00:EB7D |

G1's option screen has no key config box tiles, so its page has a plain
title instead of the frame.
