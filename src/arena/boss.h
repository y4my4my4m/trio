#ifndef ARENA_BOSS_H
#define ARENA_BOSS_H

#include "trio.h"
#include "stage.h"

//Bosses played as fighters (docs/BATTLE.md). A boss exists only as an
//enemy object with its own AI, so a boss fighter is a copy of the arena
//where that object runs with its graphics brought over from its own room:
//
//  - the source: a copy booted into the boss's room the way the game gets
//    there (through the boss door), fought until the boss is in its normal
//    fight loop; its object, OBJ tiles and OBJ palettes are kept;
//  - the fighter: an arena copy with those installed. The boss's own code
//    moves, animates and attacks. the hero stays in the copy, invisible and
//    invincible, standing where the boss's target is: the AI turns toward
//    him and aims at him, so it aims at the real opponent.
//
//Control: the AI's idle pattern is held until the player picks a move, then
//the move's pattern is started and the ROM plays it.

enum { TRIO_BOSS_NONE, TRIO_BOSS_RAIDER, TRIO_BOSS_WARLORD, TRIO_BOSS_G1_RAIDER, TRIO_BOSS_G1_WARLORD, TRIO_BOSS_G2_WARLORD, TRIO_BOSS_G1_RAIDER_ARMOR, TRIO_BOSS_COUNT };

//G3's boss meter: $7E:1F1F points at the boss object; its HUD sprites
//follow the hero's (OAM slots from TRIO_BOSS_METER_OBJ up to the HUD count).
#define TRIO_BOSS_METER 0x1F1F
#define TRIO_BOSS_METER_OBJ 16

//Moves a boss can be told to make.
enum { TRIO_MOVE_NONE, TRIO_MOVE_JUMP, TRIO_MOVE_SHOT, TRIO_MOVE_SPECIAL };

typedef struct TrioBossSrc {
  int kind;
  bool ok;
  uint8_t obj[TRIO_OBJ_SIZE];
  uint16_t name, sel;              //OBJ name tables (word addresses)
  uint8_t tiles[2][0x2000];        //both OBJ name tables
  uint16_t cgram[128];             //OBJ palettes
  uint8_t mirror[0x100];           //the game's copy of them ($7E:0400)
  //The stage's WRAM table the boss reads (boss.c, stage_tab).
  uint8_t gfx_tab[0x200];
} TrioBossSrc;

typedef struct TrioBossCtl {
  uint16_t atk_pad;                //the pad the frame before, during an attack
  bool face_right;
  int walk;                        //-1 / 0 / 1: walk while waiting (Left / Right held)
  int walk_t;
  int sub_x, sub_y;
  uint16_t pad, prev_pad;          //the fighter's buttons (B jumps, Down + B drops)
  bool ground;                     //standing (the controller's own idea)
  int vy;                          //1/256 px per frame, up positive
  int drop_t;                      //frames of falling through ladder tops
  int top_t;                       //frames an attack has kept it at the box's top
  int last_x;                      //its x the frame before (attacks: which way it moves)
  int atk_t;                       //frames of the attack in progress
  bool has_target;                 //an opponent on the facing side, at (tx, ty)
  int tx, ty;
  bool holding;                    //idle pattern held
  bool faced_right;                //facing the idle pose was last turned to
  bool have_x;                     //The hero's object as it was when the fighter began
  uint8_t x_obj[0xB0];
  uint8_t idle[TRIO_OBJ_SIZE];
  int idle_t;
} TrioBossCtl;

const char *trio_boss_name(int kind);
//The game (SNES_GAME_*) a boss kind comes from: its fighter is a copy of it.
int trio_boss_game(int kind);
//Boots the boss's room and fights to its fight loop (a few seconds).
bool trio_boss_source(TrioBossSrc *src, int kind, const uint8_t *rom, uint32_t rom_size, bool recomp);
//Installs the boss into s (a copy of its game standing in an arena) at x, feet at
//floor_y.
void trio_boss_install(Snes *s, const TrioBossSrc *src, int x, int floor_y);
//Moves the installed boss to x, feet at floor_y, facing right or left.
void trio_boss_place(Snes *s, int x, int floor_y, bool right);
//Boss kind of the object in the boss slot (NONE if there is none).
int trio_boss_kind(const Snes *s);
//The boss as a fighter: position (y = body center, feet at +17 like the hero),
//HP, facing; action TRIO_ACT_HURT while it is invincible after a hit.
void trio_read_boss(const Snes *s, TrioPlayer *p);
bool trio_boss_vulnerable(const Snes *s);
//True when a shot from x would hit the boss's shield (Warlord, facing it,
//while he waits or walks). Warlord's sprite does not follow bit 6 of +$11
//the way Raider's does, so the facing is the one the controls chose.
bool trio_boss_blocks(const Snes *s, int x, bool face_right);
//HP as a fighter's (16 a life).
void trio_boss_set_hp(Snes *s, int hp);
//Takes dmg HP and starts the game's hit flash and invincibility.
void trio_boss_damage(Snes *s, int dmg);
//Every frame before the copy runs: camera on the boss, other enemies off,
//X parked where the boss should face, the idle pattern held, and `move`
//started when the boss is idle. The boss is kept inside the arena's box.
void trio_boss_frame(Snes *s, TrioBossCtl *c, int move, const ArenaStage *st);
bool trio_boss_idle(const Snes *s);
//Active enemy shots (the boss's projectiles).
int trio_read_enemy_shots(const Snes *s, TrioShot *out, int max);

#endif
