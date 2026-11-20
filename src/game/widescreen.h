#ifndef GAME_WIDESCREEN_H
#define GAME_WIDESCREEN_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"

//Extra columns per side the stage streamers draw for (multiple of 16,
//0 = off). Set by the frontend together with the PPU and Cx4 widths.
extern int ws_active_wide;
//Snes.frame_count of the last stage-streamer pass: the level engine is running.
extern uint64_t ws_level_frame;

//True when every patched operand holds its original value (right ROM).
bool ws_check_rom(const uint8_t *rom, uint32_t size);
//Rewrites the ROM constants the streamers use for width `w` (0 restores
//the originals). rom must be the buffer the Snes reads.
void ws_apply_rom(uint8_t *rom, uint32_t size, int w);

//Override body for the JSR of the object spawner's horizontal pass (G3
//00:DE87, G2 00:DC9D, G1 80:DCA5; D = 0, $00 = column X, $02 / $04 = Y
//band). The game scans one 32 px column of the spawn list when the camera
//enters a new one: cam + $100 moving right, cam moving left. Here every
//column from that one out to the widened edge is scanned, outermost first,
//each scan returning to the JSR; the game's own column goes last and
//returns past it. Entries already spawned are skipped by the scan itself.
//
//The vertical pass (stage start, respawn, a room entered by a ladder) keeps
//the game's 10 columns, so an object placed just outside it still appears
//on the first horizontal step, as in the game: G3's intro stage starts
//a scripted scene that way (object $62 at 4400, one column right of the pass).
//cam: WRAM address of the camera X. scan: the column scan's entry PC.
bool ws_spawn_columns(Snes *s, uint32_t cam, uint16_t jsr, uint16_t scan);

#endif
