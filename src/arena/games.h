#ifndef ARENA_GAMES_H
#define ARENA_GAMES_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"

//What battle mode needs to know about each game's RAM and routines
//(docs/BATTLE.md). The three games share one engine: object layouts, field
//offsets, actions and collision attributes are the same, the addresses
//around them move. WRAM addresses are 17-bit offsets ($7E:0000 = 0,
//$7F:0000 = 0x10000); routines are 24-bit SNES addresses.

typedef struct TrioCall {
  uint32_t pc;                 //JSL target, 0 = none
  uint8_t db;                  //data bank it expects
  bool x16;                    //16-bit index registers on entry
} TrioCall;

typedef struct TrioGame {
  int id;                      //SNES_GAME_*
  const char *name;            //short: G1, G2, G3

  //Player object. +02 action, +03 step, +05 X, +08 Y (body centre, feet at
  //+17), +0E bit 0 drawn, +11 bit 6 facing right, +1A / +1C speed (1/256 px,
  //Y up positive), +22 / +24 previous position, +27 HP (bit 7: HUD refresh),
  //+33 equipped weapon * 2, +35 shots on screen, +6B invincibility in all three.
  uint32_t player;
  uint8_t ground_off, ground_mask;  //standing: (+off & mask) != 0
  uint8_t charge_off;          //blaster charge counter (G1: counts down)
  bool charge_down;
  //Object tables, [first, end) with the slot size.
  uint32_t enemy_first, enemy_end;
  uint32_t shot_first, shot_end;
  uint32_t eshot_first, eshot_end;
  uint32_t item_first, item_end, item_size;   //pickups (life, weapon energy, 1UP)
  uint32_t event_first, event_end, event_size;   //effects (G2 / G3), events (G1)
  uint32_t area_first, area_end, area_size;
  //Camera: position, previous position (the streamers and the spawner
  //compare against it), the BG1 scroll shadows, and the scroll limits the
  //game clamps the camera and the player to (x min, x max, y; each written
  //to all its copies).
  uint32_t cam_x, cam_y, cam_px, cam_py, scroll_h, scroll_v;
  uint32_t lim_xmin[2], lim_xmax[2], lim_y[5];
  int nlim_y;
  uint32_t bg2_cam_x, bg2_cam_y;
  //Level: BG1 screen map (32 x 32 screens, one byte each), block map (0x200
  //per screen), long pointers to the block attribute table and the BG1
  //block tiles (4 tilemap words per block), and the BG2 maps. The BG1
  //block map ends where BG2's begins.
  uint32_t screens, blocks, attr_ptr, tiles_ptr;
  uint32_t bg2_screens, bg2_blocks, bg2_tiles_ptr;
  //Game state.
  uint32_t stage;              //stage number read by the stage load
  uint32_t lives, max_hp, parts, hearts;
  uint32_t weapon_energy;      //weapon w (1..) at + 2 * (w - 1): bit 6 owned, bits 0-5 energy
  int nweapons;                //blaster + specials
  uint8_t weapon_full;
  //Routines.
  TrioCall damage;              //player hurt / death, D = attacker (+26 damage, +05 X)
  TrioCall music;               //A = track: uploads and starts a song (blocking)
  uint32_t song;               //the song loaded ($7E:FFFD)
  //Sound queue: entries of (id, parameter), write and read index (bytes);
  //the NMI sends one entry a frame. Ids $F0 and up are commands.
  uint32_t snd_queue, snd_w, snd_r;
  uint8_t sfx_warp, sfx_death, sfx_hurt, snd_stop;
  //Boot: START every 60 frames from `start` until the top mode byte is 2
  //(a game); the stage load reads the stage number while the game mode is
  //2 and the sub mode is load_sub.
  uint8_t mode_top, mode_game, mode_sub, load_sub;
  int start;
  int home;                    //stage the fighter copies are booted in
} TrioGame;

extern const TrioGame trio_games[4];
static inline const TrioGame *trio_game(const Snes *s) { return &trio_games[s->game & 3]; }

//Player actions (+02), the same in the three games.
enum {
  TRIO_ACT_IDLE = 0x00, TRIO_ACT_LAND = 0x02, TRIO_ACT_RUN = 0x04, TRIO_ACT_JUMP = 0x06, TRIO_ACT_FALL = 0x08,
  TRIO_ACT_DYING = 0x0C, TRIO_ACT_HURT = 0x0E, TRIO_ACT_WALLKICK = 0x10, TRIO_ACT_WALLSLIDE = 0x12,
  TRIO_ACT_DASH = 0x14, TRIO_ACT_WARP = 0x22, TRIO_ACT_CHARGED = 0x4A, TRIO_ACT_SWORD = 0x4C, TRIO_ACT_SWORD_AIR = 0x4E,
};

//Collision attributes (low 6 bits), the same in the three games.
enum {
  TRIO_AT_EMPTY = 0x00, TRIO_AT_WATER = 0x0D, TRIO_AT_WATER_TOP = 0x0E, TRIO_AT_LADDER = 0x12, TRIO_AT_LADDER_TOP = 0x13,
  TRIO_AT_SOLID_MIN = 0x33, TRIO_AT_SOLID = 0x3B, TRIO_AT_HURT = 0x3E, TRIO_AT_SPIKES = 0x3F,
};
static inline bool trio_at_solid(int a) { return a >= TRIO_AT_SOLID_MIN && a < TRIO_AT_HURT; }

#endif
