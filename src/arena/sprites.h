#ifndef ARENA_SPRITES_H
#define ARENA_SPRITES_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"

//World sprites of a copy of a game, collected after its frame from what its
//sprite builder reads (the Cx4 object list in G2 / G3, the objects' own
//sprite fields in G1), so they can be drawn at any distance from the copy's
//camera (src/arena/sprites.c).

#define SPR_MAX 256

typedef struct ArenaSpr {
  int x, y;                //world pixels, top-left
  uint8_t tile, attr;
  bool big;
  int ox, oy;              //its object's position
} ArenaSpr;
typedef struct ArenaSprites { ArenaSpr s[SPR_MAX]; int n; } ArenaSprites;

void spr_collect(const Snes *s, ArenaSprites *o);
//Only the item objects' sprites (the arena's pickups in the display copy).
void spr_collect_items(const Snes *s, ArenaSprites *o);
//Draws them with the copy's tiles and palettes, offset by (dx, dy).
void spr_draw(const Snes *s, const ArenaSprites *o, uint32_t *img, int stride, int w, int h, int dx, int dy);

#endif
