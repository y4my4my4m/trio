#ifndef ARENA_TRIO_H
#define ARENA_TRIO_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"
#include "games.h"

//Interface to one running copy of any of the three games (docs/BATTLE.md).
//Addresses come from the game's TrioGame (games.c), so every function works
//on G1, G2 and G3 machines alike.

//Extra columns per side of the battle view (the port's widescreen patches).
#define TRIO_ARENA_WIDE 96
#define TRIO_OBJ_SIZE 0x40

//G3's Blade is 8 px taller than the hero: his object's Y is 25 px over his feet.
//trio_read_player and trio_place move it by that much, so feet are at y + 17
//for every player.
#define TRIO_BLADE_RISE 8

typedef struct TrioPlayer {
  int x, y;                //world pixels; y = body centre, feet at y + 17
  int vx, vy;              //1/256 px per frame, vy up-positive
  int action, step;
  bool facing_right;
  bool ground;
  int hp;
  int charge;              //frames the blaster has been charging (0 = none)
  int iframes;
  int shots;               //shots on screen (the game's count)
} TrioPlayer;

typedef struct TrioShot {
  int slot;                //WRAM offset of the object
  int x, y;
  bool moving;             //moved since the previous frame (set by the arena)
  int kind;                //+$0A
  bool right;
  int vx, vy;              //pixels it moved in that frame (set by the arena)
} TrioShot;

//Upgrades. parts: helmet, arm, body, legs (bits 0-3, all games); hearts: one
//bit per life upgrade (max HP 16 + 2 each); chips: G3's enhancement chips
//(bits 4-7, all four = gold armor); special: G1's Fireball upgrade, G2's
//Uppercut.
typedef struct TrioUpgrades { uint8_t parts, hearts, chips; bool special; } TrioUpgrades;
#define TRIO_PARTS_ALL 0x0F
#define TRIO_HEARTS_ALL 0xFF
#define TRIO_CHIPS_GOLD 0xF0

//Power-on to a controllable the hero standing in `stage`: START until a game
//starts, the stage number forced while the new game loads it (with the
//upgrades, so the stage loads the armor graphics), dialogues paged with B.
//rom must stay valid while s is used. False if no controllable hero came.
//stage | TRIO_STAGE_SUB(n): also writes n to the byte after the stage number.
#define TRIO_STAGE_SUB(n) (((n) + 1) << 8)
bool trio_boot_stage(Snes *s, const uint8_t *rom, uint32_t rom_size, bool recomp, int stage, const TrioUpgrades *up);
void trio_set_upgrades(Snes *s, const TrioUpgrades *up);

void trio_read_player(const Snes *s, TrioPlayer *p);
//Moves the player (body centre y, feet at y + 17) with no speed.
void trio_place(Snes *s, int x, int y, bool facing_right);
//The game's beam-in at the player's position: he falls from there to the
//ground and materialises (action $22, with its sound).
void trio_warp_in(Snes *s);
//Animation frame ($+17, bit 7 cleared).
int trio_anim(const Snes *s);
//Sets the HP (bit 7 asks the HUD to redraw).
void trio_set_hp(Snes *s, int hp);
int trio_max_hp(const Snes *s);

//G3's Blade: the story flags that hand him the controls, then a pause and a
//hop so the game loads his palette, HUD and tiles (about 200 frames).
void trio_make_blade(Snes *s);
bool trio_is_blade(const Snes *s);

int trio_read_shots(const Snes *s, TrioShot *out, int max);
int trio_read_enemy_shots(const Snes *s, TrioShot *out, int max);
//Removes a player shot the way the game does when it leaves the screen
//(clearing the slot directly would leak the game's shots-on-screen count).
void trio_kill_shot(Snes *s, int slot);

//Holds the camera at (camx, camy): scroll limits closed on it, so the game
//keeps the player inside [camx + 8, camx + 248] and kills him below it.
void trio_camera(Snes *s, int camx, int camy);
int trio_cam_x(const Snes *s);
int trio_cam_y(const Snes *s);
//The games' own pickups as objects in a copy (the item table): slot n of
//the table set to kind at world (x, y) (feet); true when it was just made
//(the game sets it up on its next frame). They are the kind placed in a
//level: no pop-up, no timeout.
enum { TRIO_ITEM_NONE, TRIO_ITEM_LIFE_S, TRIO_ITEM_LIFE_L, TRIO_ITEM_WEAPON_S, TRIO_ITEM_WEAPON_L, TRIO_ITEM_1UP, TRIO_ITEM_COUNT };
bool trio_put_item(Snes *s, int slot, int kind, int x, int y);
void trio_clear_item(Snes *s, int slot);
//Every frame of a fighter or display copy: enemies, their shots and the
//stage's events and items switched off.
void trio_quiet(Snes *s);
//trio_quiet without the items (a display copy showing the arena's items).
void trio_quiet_enemies(Snes *s);

//Runs a routine of the game to its RTL (direct page d, the call's data bank
//and index size, 8-bit A) and puts the CPU back. Returns false if it did
//not return within max_insns.
bool trio_call(Snes *s, TrioCall c, uint16_t d, uint16_t a, uint16_t x, uint16_t y, int max_insns);
//The game's player damage routine as if an enemy at (from_x, from_y)
//touched him for dmg HP: hurt, knockback, invincibility, death, sound.
void trio_damage(Snes *s, int dmg, int from_x, int from_y);

//Collision attribute (low 6 bits, TRIO_AT_*) at world (x, y); -1 outside the
//level.
int trio_attr(const Snes *s, int x, int y);
//BG1 / BG2 tilemaps of the whole level from the level data, column-major
//BG1 tile entries (top-left, top-right, bottom-left, bottom-right) of
//level block blk.
void trio_block_tiles(const Snes *s, uint16_t blk, uint16_t out[4]);
//Writes block blk into the copy's block map at world (x, y). In a copy with
//an arena's collision installed (trio_geo_install) a block's number is its
//attribute, so this sets the collision there.
void trio_put_block(Snes *s, int x, int y, uint16_t blk);
//(w x h entries of 8x8 tiles), the layout PpuEnh.vmap takes.
void trio_build_bg1_map(const Snes *s, uint8_t *map, int w, int h);
void trio_build_bg2_map(const Snes *s, uint8_t *map, int w, int h);
//The BG2 camera the game's streamer draws from.
void trio_bg2_camera(const Snes *s, int *x, int *y);

//Arena geometry: collision attributes of a rectangle of the world (whole
//256 px screens), one byte per 16 px block, row-major.
typedef struct TrioGeo {
  int x0, y0, sw, sh;      //first screen (pixels) and size in screens
  uint8_t *attr;           //(sw * 16) x (sh * 16)
} TrioGeo;
void trio_geo_read(const Snes *s, TrioGeo *g, int x0, int y0, int x1, int y1);
void trio_geo_free(TrioGeo *g);
uint8_t trio_geo_at(const TrioGeo *g, int x, int y);
void trio_geo_set(TrioGeo *g, int x, int y, uint8_t a);
//Replaces a copy's level collision with g, whatever game and stage it
//runs: the screens outside g become empty, the attribute table becomes an
//identity table in WRAM, so block number = attribute (docs/BATTLE.md).
bool trio_geo_install(Snes *s, const TrioGeo *g);

//Weapons: w = 0 blaster, 1.. specials (nweapons in the game).
int trio_weapon(const Snes *s);
void trio_set_weapon(Snes *s, int w);
int trio_weapon_energy(const Snes *s, int w);        //0..weapon_full
void trio_set_weapon_energy(Snes *s, int w, int e);
void trio_unlock_weapons(Snes *s);
void trio_refill_weapons(Snes *s, int amount);

//Sound queue (games.h): the entries a copy queued since *seen (its write
//index then), and a sound pushed into a copy's queue.
typedef struct TrioSnd { uint8_t id, param; } TrioSnd;
int trio_sound_take(const Snes *s, uint8_t *seen, TrioSnd *out, int max);
void trio_sound_push(Snes *s, uint8_t id, uint8_t param);

#endif
