#ifndef ARENA_STAGE_H
#define ARENA_STAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "trio.h"

//Battle arenas (docs/BATTLE.md): a box of a stage of any of the three games.
//The stage is shown by a display copy of its game; every fighter copy, of
//whatever game, gets the box's collision (trio_geo_install) so all of them
//stand on the same floors and platforms.

typedef struct ArenaPlat { int x, y, w; } ArenaPlat;
//A lane of a moving platform: w blocks with their top at y, the left edge
//going back and forth between x0 and x1.
typedef struct ArenaLane { int y, x0, x1, w; } ArenaLane;   //world px of the left block; w blocks, one block thick

//Layouts: the box as it is in the level, or rebuilt as a platform-fighter stage with the
//stage's own blocks over its background (stage.c, rebuild).
enum { LAYOUT_LEVEL, LAYOUT_BATTLEFIELD, LAYOUT_FINAL, LAYOUT_TIERS, LAYOUT_TOWERS, LAYOUT_COUNT };

typedef struct ArenaDef {
  const char *name;
  int game, stage;
  int x0, y0, x1, y1;      //the box, world pixels (whole 16 px blocks)
  ArenaPlat plat[8];       //added platforms (stage graphics and collision)
  int nplat;
  int layout;              //LAYOUT_*
  int hazard;              //HZ_*: a small section of the rebuilt layout
} ArenaDef;

//Hazards: an icy stretch of the main floor (slides), a gust zone (pushes,
//turning about now and then), fire jets (erupt from the floor every few
//seconds). Run by the arena, drawn by battle.c.
enum { HZ_NONE, HZ_ICE, HZ_WIND, HZ_FIRE };
typedef struct ArenaHazard { int kind, x0, y0, x1, y1; } ArenaHazard;

extern const ArenaDef arena_defs[];
extern const int arena_ndefs;

#define STAGE_ITEMS 8
#define STAGE_FLOORS 48
#define VIEW_MAP_W 1024    //whole-level tilemaps, 8x8 tiles
#define VIEW_MAP_H 1024

typedef struct ArenaStage {
  const ArenaDef *def;
  bool ok;
  Snes *view;              //display copy: in the stage, camera on the box
  PpuEnh enh;
  uint8_t *map1, *map2;    //BG1 / BG2 tilemaps of the whole level (PpuEnh.vmap)
  TrioGeo geo;              //collision of the box
  int spawn_x[4], spawn_y[4];   //feet
  int floor_x[STAGE_FLOORS], floor_y[STAGE_FLOORS];   //open floors of the main space (item drops)
  int nfloor;
  ArenaLane lane[2];       //moving platforms (picked from the collision)
  int nlane;
  int plat_blk;            //level block drawn for platforms (the box's floor; -1 none)
  ArenaHazard haz[3];
  int nhaz;
  //Rebuilt layouts: the background (stage_draw). The window of the BG2 map
  //the game itself shows over the box, [bg_x0, bg_x1) x [bg_y0, bg_y1), from
  //its BG2 camera with the game's camera at the box's left / right
  //(bg_xl, bg_xr) and top / bottom (bg_yt, bg_yb); bg: that window drawn.
  int bg_x0, bg_x1, bg_y0, bg_y1, bg_xl, bg_xr, bg_yt, bg_yb;
  uint32_t *bg;
  //The display copy's camera as pinned this frame and the frame before: its
  //picture (scroll written in vblank) goes with the one before.
  int cx, cy, pcx, pcy;
  //stage_view renders the display copy's own picture (tools); battle mode
  //draws the stage with stage_draw instead.
  bool render;
  //Pickups shown as the game's own item objects in the display copy
  //(TRIO_ITEM_*, world x and feet y), set before each stage_view.
  int item_kind[STAGE_ITEMS], item_x[STAGE_ITEMS], item_y[STAGE_ITEMS];
} ArenaStage;

//Boots the stage, takes the camera to the box (the game loads each area's
//graphics on the way), adds the platforms and reads the collision. rom must
//stay valid while the stage is used. Takes a few seconds.
bool stage_boot(ArenaStage *st, const ArenaDef *def, const uint8_t *rom, uint32_t rom_size, bool recomp);
void stage_free(ArenaStage *st);
//Display copy: camera at world (camx, camy) clamped to the box for a view
//`w` pixels wide (256 + 2 * wide), picture rendered with BG1 and BG2 (and
//OBJ when objs is set). Moved a few pixels per frame.
void stage_view(ArenaStage *st, int camx, int camy, int w, bool objs);
//The stage at any picture size from the whole-level tilemaps, with the
//display copy's characters, palettes and background scroll: picture pixel
//(0, 0) is world (px, py). No windows or color math.
void stage_draw(const ArenaStage *st, uint32_t *img, int stride, int w, int h, int px, int py);
//Clamps a picture's top-left (w x h) to the box with a margin; a picture
//bigger than that is centered on it.
void stage_clamp_pic(const ArenaStage *st, int *px, int *py, int w, int h);
//Clamps a camera (top-left of a w x 224 view) to the box.
void stage_clamp(const ArenaStage *st, int *camx, int *camy, int w);
//A fighter copy in the box: collision installed, camera on the player.
bool stage_install(const ArenaStage *st, Snes *s);
//Every frame of a fighter copy: camera on its player, clamped to the box.
void stage_follow(const ArenaStage *st, Snes *s);

#endif
