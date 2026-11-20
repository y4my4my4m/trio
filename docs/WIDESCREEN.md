# Widescreen

Stages render up to 96 extra columns per side (16:10, 16:9 or 2:1, with
or without 8:7 pixel aspect; `settings_wide_columns`). The title, stage
select, pause and password screens stay 4:3 and are centered on the same
canvas, so the window does not change size.

It needs the recompiled build (three of the changes are overrides) and the USA
ROM. `ws_check_rom` compares every patched operand with its expected value,
and the menu disables the option otherwise.

## What the game does

The level engine draws BG1 and BG2 with the same routines. Each layer first
copies its camera into a scratch block (00:BC9E for BG1 from $1E5D/$1E60,
00:BCD5 for BG2 from $1E9D/$1EA0):

| WRAM | |
|---|---|
| $1FE2 | tilemap base ($5000 BG1, $5800 BG2) |
| $1FE4 / $1FE6 | camera X / Y |
| $1FE8 / $1FEA | previous camera X / Y |

and then, on an 8 px step of the camera:

- X changed (00:B8CB): one 16 px column at `cam + $100` (moving right) or
  `cam` (moving left), 16 blocks tall (00:B91B).
- Y changed (00:B8EB): one row of `$12` blocks starting at `cam`, at
  `cam + $E8` (down) or `cam` (up) (00:BA32). The full redraw after a room
  change or respawn is the same row routine in a loop (00:B840).
- Single blocks (destroyed walls, 00:BB70) are drawn when inside
  `[cam - 16, cam + $110)`.

Tilemaps are 64 x 32 (512 px), a ring over the level. Only the visible 256 px
(plus the 16 px being entered) are ever current.

Objects spawn from a list of 32 px columns (00:DE3A), in two passes:

- horizontal, when the camera enters a new 32 px column: the column at
  `cam + $100` (moving right) or `cam` (left), for Y in
  `[camY - $20, camY + $100)` (00:DE79, scan 00:DED3);
- vertical, when the camera's Y changes: 10 columns from `cam - $20`, for a
  32 px band of Y above or below the screen (00:DE8A). A stage start and a
  respawn sweep the camera down a screen through this pass (00:9B5C).

An entry already spawned is skipped. Objects check "near the camera" with
three helpers, used for activity and despawn:

| Helper | X range |
|---|---|
| 02:D58A | `[cam - $40, cam + $140)` |
| 02:D611 | `[cam - $60, cam + $160)` |
| 02:D636 | `[cam - $20, cam + $120)` (on-screen test, 28 callers) |

The Cx4 OAM builder drops sprite pieces outside `[-16, 272]` of screen X.

## Changes for width w

| Where | Change |
|---|---|
| 00:BCBC, 00:BCF3 (`ovr_WideCamBG1/2`) | after the camera copy: `$1FE4 = cam - w`, `$1FE8 = prev - w` |
| 00:B8DF | right column `$0100` + 2w |
| 00:BA49 | row length `$12` + 2w/16 blocks |
| 00:BB8C | single-block bound `$0120` + 2w |
| 00:DE87 (`ovr_WideSpawnColumns`) | horizontal pass: every column from the game's own out to the widened edge (`ws_spawn_columns`) |
| 02:D594/D597, D61B/D61E, D640/D643 | near-camera margins + w, widths + 2w |
| Cx4 (`cx4_wide`) | pieces kept in `[-16 - w, 272 + w]` |
| PPU (`PpuEnh.wide`) | BG1, BG2 and OBJ drawn into the extra columns; BG3 (HUD) is not |

The horizontal spawn pass scans the columns from the game's (`cam + $100`
or `cam`) out to `cam + $100 + w` or `cam - w`, the outermost first and the
game's own last. The vertical pass is the game's: 10 columns. A single
column at the widened edge would leave the ones between it and the
vertical pass unscanned for good (after a ladder, at a stage start); and a
wider vertical pass would spawn objects the game places just outside its
own, which then appear on the first step sideways: G3 stage 0 starts a
scripted scene that way (object $62 at X 4400, in the column right of the
pass when the hero comes down the ladder), and the scene needs the room's
camera and background settled.

With the scratch camera shifted, every streamer covers `[cam - w, cam + 256 + w)`.
The ring has room for it up to w = 120. The game's real camera variables are
not touched, so scrolling, camera locks and collision behave as before. The
immediates are rewritten in the loaded ROM image; both the interpreter and
the recompiled code fetch operands from ROM at run time. With w = 0 the ROM
is original and the overrides are no-ops, and `trio-verify` still reports
identical runs.

Extended columns take the window state of the nearest classic edge, so
the dialog box and other window effects stay inside the 256 px area.

## HUD

The life, weapon and boss meters are sprites the game writes directly into
OAM slots 0-15, below the Cx4 builder's first free slot ($6626). With "HUD at
the screen edges" on, the PPU moves those slots outward by the width: meters
on the left half go left, meters on the right half go right
(`PpuEnh.hud_objs`, `PpuEnh.hud_dx`). Off, they stay in the centered 4:3 area.

## When it is on

`ovr_WideCamBG1` records the frame every time the BG1 streamer runs. That
happens every frame of stage gameplay (and cutscenes that use the level
engine), never on menu screens. The frontend switches the PPU and Cx4 width
after 10 consecutive stage frames and back after 4 without, so single-frame
blips during fades do not flicker (`src/port/enhance.c`).

## Limits

- Changing the width inside a stage takes effect as the camera moves. The
  sides are fully drawn after the next room change or respawn.
- Where a room's camera is locked (boss doors, vertical shafts), the margins
  show whatever level data lies beyond, as in any widescreen hack.
- An object in the extra columns of a room entered by a ladder (or at a
  stage start) appears on the first step sideways, not at once.

## G1 and G2

Both games have the G3 routines byte for byte at other addresses; only the
scratch addresses and G1's camera variables differ. The same changes apply
(`patches_g1`, `patches_g2` in `src/game/ws_rom.c`, hooks in
`src/game/g1_ws.c` and `g2_ws.c`):

| | G1 (code at $80+) | G2 |
|---|---|---|
| BG1 camera copy, hook after it | 80:BA87: $1E4D, $1E6A -> $1FA3, $1FA7 | 00:B7EA: $1E5D, $1E7A -> $1FE0, $1FE4 |
| BG2 camera copy, hook after it | 80:BABE: $1E8D, $1EAA -> $1FA3, $1FA7 | 00:B821: $1E9D, $1EBA -> $1FE0, $1FE4 |
| right column `$0100` + 2w | 80:B649 | 00:B40C |
| row length `$12` + 2w/16 | 80:B7B3 | 00:B576 |
| single-block bound `$0120` + 2w | 80:B8F6 | 00:B6B9 |
| horizontal spawn pass, hook at its `JSR` | 80:DCA5 (scan 80:DCF1) | 00:DC9D (scan 00:DCE9) |
| near-camera helpers | 82:806E, 82:808F, 82:80B4 | 00:D813, 00:D834, 00:D859 |
| sprite pieces | CPU OAM builder 80:D780: keep while x + $10 < $010F, both + w / + 2w | Cx4 (`cx4_wide`) |

G1 builds its OAM on the CPU from slot 16; its HUD meters are in slots 0-15,
as in G3.
