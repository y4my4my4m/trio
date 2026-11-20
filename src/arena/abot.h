#ifndef ARENA_ABOT_H
#define ARENA_ABOT_H

#include "arena.h"

//CPU player: reads the arena state and returns SNES pad bits for its copy
//of the game, so it plays through the same controls as a human. It finds
//its way between the arena's floors with a platform graph (abot.c).

typedef struct ABot {
  uint32_t rng;
  int target;
  int retarget_t;
  int charge_want;         //frames to hold Y for this shot, 0 = tap
  int holding_y;
  int jump_hold;           //frames left of holding B
  int air_dir;             //steering while airborne (-1, 0, 1)
  int dash_cd;
  int threat_t;
  int tap_cd;
  int stuck_t;
  int last_x;
  int idle_fire_t;         //frames since this bot last landed a hit (raises aggression)
  int last_dealt;
  int charge_cd;
  int weapon_cd;
  int goal_item;           //item it is after (-1 none)
  int wander_x, wander_y, wander_t;  //a place to go when the target is out of reach
  //A wall climb in progress: the wall's side, the floor it leads to.
  int climb_side, climb_from, climb_to, climb_t;
  //A ladder climb in progress: -1 up, 1 down, the ladder's column.
  int ladder_dir, ladder_x, ladder_t;
  //A special's motion being input: the pads left to send, 3 frames each.
  uint16_t seq[4];
  int seq_n, seq_i, seq_t;
  uint16_t last;
} ABot;

void abot_init(ABot *b, int index, uint32_t seed);
uint16_t abot_think(ABot *b, const Arena *a, int me);

#endif
