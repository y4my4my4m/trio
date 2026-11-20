#ifndef ARENA_ARENA_H
#define ARENA_ARENA_H

#include "trio.h"
#include "stage.h"
#include "sound.h"
#include "boss.h"

//A match (docs/BATTLE.md): one running copy of a game per fighter, all with
//the arena's collision, so they share the floors. Each copy moves its own
//player with its game's code; the arena carries shots from one copy into
//hits on the others, routes the sounds and runs the rules.

#define ARENA_MAX 4
#define ARENA_ITEMS 8

enum { FK_X, FK_BLADE, FK_BOSS };

typedef struct ArenaFighter {
  bool on, cpu;
  int level;               //bot 1..3
  int kind;                //FK_*
  Snes *base;              //installed copy this fighter (re)spawns from
  Snes *s;                 //this fighter's copy of the game
  int boss;                //TRIO_BOSS_* for FK_BOSS
  TrioBossCtl ctl;
  int pend_move, pend_t;   //boss: a move pressed early waits this long
  TrioPlayer p, prev;
  TrioShot shots[16];
  int nshots;
  //HERO FULL's specials in every style: G1's Fireball and G2's Uppercut.
  //When the fighter's own game hasn't the move, it is performed by the hero in a
  //helper copy of the game that has it (helper[game]), at the fighter's
  //place; the helper's fireball or flame is this fighter's attack.
  bool specials;
  int sp_move, sp_t, sp_game;   //ARENA_SP_* in progress
  int sp_cd;                    //frames until the next special may start
  //The motion typed into the helper copy (the game starts the move itself
  //and loads its graphics); sp_go once its the hero is in the move.
  uint16_t sp_seq[8];
  int sp_seq_n, sp_seq_i;
  bool sp_go;
  int mot[3], mot_t[3];         //motion read so far per move, frames since its last step
  //The hero's copies of the other games (helper[game], NULL for the one he is in
  //now: s). They do the specials of the other games, and the weapons of
  //other games: the fighter moves into that game's copy (arena.c, hop),
  //the one he leaves runs on while its shots fly.
  Snes *helper[4];
  bool helper_live[4];
  bool helper_init[4];     //cloned from its base this match
  Snes *alt_base[4];       //X of the same tier in each game (ArenaSlot.alt)
  int own_game, cur_game;
  //Weapons in L / R order: his game's blaster, then each game's specials.
  struct { uint8_t game, w; } wl[32];
  int nwl, wi;
  uint16_t sw_inject;
  int drop_t;              //frames of dropping through a one-way platform
  int slide;               //on ice: speed carried, 1/256 px per frame      //a press for the copy next frame (the game's own switch)
  TrioShot hshots[4][16];
  int nhshots[4];
  uint8_t hsnd_seen[4];
  //The weapon each shot slot was fired with (by slot index in the shot
  //table), so only the current weapon's shots count against its limit.
  int8_t shot_weapon[32];
  //The copy's picture chip as it was when he last put a weapon away: the
  //next weapon's tiles and colors replace that one's in the copy, so its
  //shots still out are drawn from this (battle.c, draw_sprites).
  Ppu *old_ppu;
  bool have_old;
  uint8_t snd_seen;        //sound queue write index already forwarded
  //Camera the copy's OAM was built with: the OAM in the PPU after a frame
  //was built during the frame before, so it goes with that frame's camera.
  int cam_x, cam_y, oam_cam_x, oam_cam_y;
  int max_hp;              //the copy's max HP: every life lasts the same hits
  int stocks;
  bool dead;
  int dead_t;              //frames since the death began
  int spawn_t;             //spawn protection left
  bool first_spawn;
  int kos, falls, dealt;
  int last_hitter, last_hit_t;
  int hit_flash;
  uint16_t pad, prev_pad;
  //Blade's sword: the slash runs while his action is the sword's.
  int sword_t;
  uint8_t sword_hit;
} ArenaFighter;

typedef enum { AP_COUNTDOWN, AP_FIGHT, AP_GAME, AP_OVER } ArenaPhase;

typedef struct ArenaFx { int kind; int x, y; } ArenaFx;
enum { FX_HIT = 1, FX_BIG_HIT, FX_KO, FX_BLOCK, FX_ITEM, FX_1UP };

//Items lying in the arena (life and weapon energy, extra lives).
enum { IT_NONE, IT_LIFE_S, IT_LIFE_L, IT_WEAPON, IT_HEART, IT_COUNT };
typedef struct ArenaItem { int kind; int x, y, vy; int t; } ArenaItem;

//specials: HERO FULL's Fireball and Uppercut in every style (arena_set_helper).
//alt[g]: the hero of game g in the same tier (for the specials, and the weapons
//of the games in pool: bit g - 1; 0 = his own game's only).
typedef struct ArenaSlot { bool on, cpu; int level; Snes *base; bool specials; Snes *alt[4]; uint8_t pool; } ArenaSlot;

enum { ARENA_SP_NONE, ARENA_SP_FIREBALL, ARENA_SP_UPPERCUT };

typedef struct ArenaRules {
  int stocks;
  bool weapons;            //special weapons (else the blaster only)
  bool items;
  int time_limit;          //frames of fighting before TIME (0: none)
} ArenaRules;

//A shot the arena flies itself (arena.c, ghosts): it has left its copy and
//goes on at the speed it had, drawn from a picture of it taken then.
#define GHOST_MAX 48
#define GHOST_SIZE 64
#define GHOST_CLEAR 0xFF000000u   //a pixel of the picture with nothing in it
typedef struct ArenaGhost {
  bool on;
  int owner, game, kind;
  bool right;
  int x, y, vx, vy;        //world pixels, pixels per frame
  int t;
  uint32_t pic[GHOST_SIZE][GHOST_SIZE];   //centred on (x, y)
} ArenaGhost;

//A moving platform on one of the stage's lanes (ArenaStage.lane). Its
//collision is a row of ladder-top blocks (stood on, passed from below and
//the sides) at the block column nearest it, written into every fighter's
//copy; fighters standing on it are carried along.
typedef struct ArenaMover {
  ArenaLane lane;
  int x;                   //left edge, world pixels * 256
  int dir;
  int px;                  //left edge drawn this frame (pixels)
  int col;                 //first block column of its collision (from lane.x0)
} ArenaMover;

typedef struct Arena {
  ArenaFighter f[ARENA_MAX];
  Snes *helper_base[4];    //X of G1 / G2 standing in the arena (arena_set_helper)
  ArenaMover mover[2];
  int nmover;
  int fight_t;             //frames of fighting so far
  int last_any_hit;        //frame of the last hit anyone took
  int force_spawn;         //spawn(): this spawn point instead (-1)
  bool time_up;            //the match ended on the time limit
  const ArenaStage *st;
  ArenaSound *snd;
  ArenaPhase phase;
  int phase_t;
  int frame;
  int winner;
  ArenaRules rules;
  ArenaFx fx[16];
  int nfx;
  ArenaItem items[ARENA_ITEMS];
  int item_t;
  ArenaGhost ghost[GHOST_MAX];
  uint32_t rng;
  int wind_dir;            //the gust's way (-1 / 1)
} Arena;

//Fire jets (HZ_FIRE): the phase of the cycle at frame t (0 idle, 1 warning,
//2 burning) and how high the flame is (0..1 << 8).
int arena_fire_phase(const Arena *a, int k, int *height);

//slots[i].base: a copy of the fighter's character standing in the arena
//(stage_install done). Allocates one copy per fighter.
void arena_init(Arena *a, const ArenaStage *st, ArenaSound *snd, const ArenaSlot slots[ARENA_MAX], ArenaRules rules);
void arena_free(Arena *a);
//X of game (G1 for the Fireball, G2 for the Uppercut) standing in the
//arena, for the specials of fighters of the other games. Kept by pointer.
void arena_set_helper(Arena *a, int game, Snes *base);
//pads: SNES buttons per fighter (bots are filled in by the caller). the hero is
//Blade's sword button; L / R switch weapons at any time.
void arena_step(Arena *a, const uint16_t pads[ARENA_MAX]);
int arena_alive(const Arena *a);

//Boxes (world pixels).
void arena_hurtbox(const ArenaFighter *f, int *x0, int *y0, int *x1, int *y1);
void arena_shotbox(int game, const TrioShot *s, int *x0, int *y0, int *x1, int *y1);
void arena_swordbox(const TrioPlayer *p, int *x0, int *y0, int *x1, int *y1);

//What a shot of a game does (damage in sixteenths of a life, 0 = none).
//known: a kind listed in arena_shot_info (others share the shot tables:
//glows, sparks, weapons not looked at yet; they count as 2 damage).
typedef struct ShotInfo { int dmg; bool pierce, still, shield, big, known; } ShotInfo;
ShotInfo arena_shot_info(int game, int kind);

#endif
