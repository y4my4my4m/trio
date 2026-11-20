#ifndef ARENA_BATTLE_H
#define ARENA_BATTLE_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"
#include "port/port_api.h"
#include "arena.h"

//Battle mode as the frontend sees it (docs/BATTLE.md): a match, a pause
//menu and a results screen, all drawn with the game's own graphics. The
//stage is a display copy of the game; every fighter is its own copy, whose
//sprites (the hero, shots, effects) and HUD meter are composed over the stage.

int battle_rooms(void);
const char *battle_room_name(int room);
int battle_characters(void);
//"HERO ARMOR G3": the character and its style.
const char *battle_character_name(int c);
const char *battle_character_group(int c);   //"HERO"
const char *battle_character_style(int c);   //"G3 ARMOR"
//Setup page steps: the next character (in its first style), or the next
//style of the same character.
int battle_next_character(int c, int d);
int battle_next_style(int c, int d);
//Raider and Warlord: bosses moved by the battle layer (docs/BATTLE.md), rougher
//than the playable characters. The setup page says so.
bool battle_character_experimental(int c);
//Music choices for the match (0 = the stage's own song).
int battle_songs(void);
const char *battle_song_name(int i);

//Starts a match. rom[g]: the ROM image of game g (SNES_GAME_*, NULL if
//absent), kept valid during the match. Takes a few seconds the first time
//an arena or a character is used. wide: extra columns per side of the
//picture (0 = 256 px, up to TRIO_ARENA_WIDE). The text uses the font of
//src/arena/ui.c.
bool battle_begin(const BattleSetup *setup, const uint8_t *const rom[4], const uint32_t size[4], bool recomp, int wide);
//One frame. pads: SNES buttons of each human slot; any: every device
//together (menus). Returns false once the player leaves battle mode.
bool battle_frame(const uint16_t pads[BATTLE_SLOTS], uint16_t any);
//Largest picture (BV_HUGE).
#define BATTLE_MAXW 640
#define BATTLE_MAXH 360
//The last picture: XRGB8888, BATTLE_MAXW pixels per row.
const uint32_t *battle_picture(int *w, int *h);
//Stereo samples of the match; returns frames written.
int battle_audio(int16_t *out, int max_frames);
//The running match (tools and tests).
const Arena *battle_arena(void);
//Ends a match.
void battle_end(void);

#endif
