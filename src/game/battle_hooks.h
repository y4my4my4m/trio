#ifndef GAME_BATTLE_HOOKS_H
#define GAME_BATTLE_HOOKS_H

#include <stdbool.h>

//Battle mode's hooks in the games' code (src/game/battle_hooks.c). Off
//outside a match, so normal play and trio-verify see the original code.

typedef struct BattleBox {
  bool on;
  int x0, y0, x1, y1;      //world pixels: objects inside count as on screen
} BattleBox;

extern BattleBox battle_box;

#endif
