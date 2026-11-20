#include "stage.h"
#include <stdlib.h>
#include <string.h>

static inline uint16_t rd16(const Snes *s, uint32_t a) { return (uint16_t)(s->wram[a & 0x1FFFF] | s->wram[(a + 1) & 0x1FFFF] << 8); }
static inline void wr16(Snes *s, uint32_t a, uint16_t v) { s->wram[a & 0x1FFFF] = (uint8_t)v; s->wram[(a + 1) & 0x1FFFF] = (uint8_t)(v >> 8); }

#define BOX(sx, sy, w, h) .x0 = (sx) * 256, .y0 = (sy) * 256, .x1 = ((sx) + (w)) * 256, .y1 = ((sy) + (h)) * 256

//Boxes picked by scoring every window of 2-4 x 1-3 screens of every stage
//for floors at several heights, room to move and no water, spikes or
//damaging blocks, keeping those whose floors can all be reached from each
//other with jumps, drops, ladders and wall kicks (tools/arenas.py).
const ArenaDef arena_defs[] = {
{"G1 STAGE 0", SNES_GAME_G1, 0, BOX(9, 1, 4, 2), .layout = LAYOUT_BATTLEFIELD},
  {"G1 STAGE 2", SNES_GAME_G1, 2, BOX(10, 2, 4, 1), .layout = LAYOUT_TIERS},
  {"G1 STAGE 3", SNES_GAME_G1, 3, BOX(13, 9, 4, 1), .layout = LAYOUT_FINAL},
  {"G1 STAGE 4", SNES_GAME_G1, 4, BOX(24, 2, 4, 1), .layout = LAYOUT_TOWERS, .hazard = HZ_FIRE},
  {"G1 STAGE 5", SNES_GAME_G1, 5, BOX(13, 1, 3, 2), .layout = LAYOUT_BATTLEFIELD, .hazard = HZ_WIND},
  {"G1 STAGE 6", SNES_GAME_G1, 6, BOX(2, 2, 4, 2), .layout = LAYOUT_TIERS, .hazard = HZ_FIRE},
  {"G1 STAGE 7", SNES_GAME_G1, 7, BOX(5, 3, 2, 2), .layout = LAYOUT_FINAL},
  {"G1 STAGE 8", SNES_GAME_G1, 8, BOX(19, 2, 4, 1), .layout = LAYOUT_TOWERS, .hazard = HZ_ICE},
  {"G1 STAGE 9", SNES_GAME_G1, 9, BOX(2, 9, 4, 1), .layout = LAYOUT_BATTLEFIELD},
  {"G1 STAGE 10", SNES_GAME_G1, 10, BOX(6, 5, 4, 1), .layout = LAYOUT_TIERS},
  {"G1 STAGE 11", SNES_GAME_G1, 11, BOX(13, 2, 3, 1), .layout = LAYOUT_FINAL},
  {"G2 STAGE 0", SNES_GAME_G2, 0, BOX(17, 2, 4, 1), .layout = LAYOUT_TOWERS},
  {"G2 STAGE 1", SNES_GAME_G2, 1, BOX(4, 3, 4, 1), .layout = LAYOUT_BATTLEFIELD},
  {"G2 STAGE 2", SNES_GAME_G2, 2, BOX(1, 2, 3, 2), .layout = LAYOUT_TIERS, .hazard = HZ_WIND},
  {"G2 STAGE 4", SNES_GAME_G2, 4, BOX(8, 4, 2, 2), .layout = LAYOUT_FINAL, .hazard = HZ_FIRE},
  {"G2 STAGE 5", SNES_GAME_G2, 5, BOX(6, 0, 4, 2), .layout = LAYOUT_TOWERS},
  {"G2 STAGE 6", SNES_GAME_G2, 6, BOX(6, 2, 2, 2), .layout = LAYOUT_BATTLEFIELD, .hazard = HZ_ICE},
  {"G2 STAGE 8", SNES_GAME_G2, 8, BOX(1, 5, 3, 2), .layout = LAYOUT_TIERS},
  {"G2 STAGE 9", SNES_GAME_G2, 9, BOX(3, 5, 3, 2), .layout = LAYOUT_FINAL},
  {"G3 STAGE 0", SNES_GAME_G3, 0, BOX(18, 6, 4, 1), .layout = LAYOUT_TOWERS},
  {"G3 STAGE 1", SNES_GAME_G3, 1, BOX(14, 4, 4, 2), .layout = LAYOUT_BATTLEFIELD, .hazard = HZ_WIND},
  {"G3 STAGE 2", SNES_GAME_G3, 2, BOX(8, 2, 3, 2), .layout = LAYOUT_TIERS, .hazard = HZ_ICE},
  {"G3 STAGE 3", SNES_GAME_G3, 3, BOX(21, 4, 4, 1), .layout = LAYOUT_FINAL},
  {"G3 STAGE 4", SNES_GAME_G3, 4, BOX(16, 2, 2, 2), .layout = LAYOUT_TOWERS},
  {"G3 STAGE 5", SNES_GAME_G3, 5, BOX(9, 8, 3, 2), .layout = LAYOUT_BATTLEFIELD},
  {"G3 STAGE 6", SNES_GAME_G3, 6, BOX(2, 3, 3, 2), .layout = LAYOUT_TIERS},
  {"G3 STAGE 7", SNES_GAME_G3, 7, BOX(9, 6, 4, 1), .layout = LAYOUT_FINAL},
  {"G3 STAGE 8", SNES_GAME_G3, 8, BOX(19, 2, 3, 1), .layout = LAYOUT_TOWERS},
  {"G3 STAGE 9", SNES_GAME_G3, 9, BOX(17, 1, 2, 2), .layout = LAYOUT_BATTLEFIELD},
  {"G3 STAGE 10", SNES_GAME_G3, 10, BOX(14, 2, 2, 2), .layout = LAYOUT_TIERS},
  {"G3 STAGE 11", SNES_GAME_G3, 11, BOX(3, 4, 4, 1), .layout = LAYOUT_FINAL},
  {"G3 STAGE 12", SNES_GAME_G3, 12, BOX(12, 4, 4, 1), .layout = LAYOUT_TOWERS},
  //The bosses' rooms as they are (found by a scan of each level for
  //closed one-screen rooms with a flat floor: the one at the level's end).
  {"G1 ROOM 2", SNES_GAME_G1, 2, .x0 = 7936, .y0 = 208, .x1 = 8192, .y1 = 464},
  {"G1 ROOM 3", SNES_GAME_G1, 3, .x0 = 5120, .y0 = 2512, .x1 = 5376, .y1 = 2768},
  {"G1 ROOM 5", SNES_GAME_G1, 5, .x0 = 3840, .y0 = 272, .x1 = 4096, .y1 = 528},
  {"G1 ROOM 6", SNES_GAME_G1, 6, .x0 = 7424, .y0 = 736, .x1 = 7680, .y1 = 992},
  {"G1 ROOM 8", SNES_GAME_G1, 8, .x0 = 7680, .y0 = 224, .x1 = 7936, .y1 = 480},
  {"G2 ROOM 1", SNES_GAME_G2, 1, .x0 = 5120, .y0 = 1760, .x1 = 5376, .y1 = 2016},
  {"G2 ROOM 2", SNES_GAME_G2, 2, .x0 = 6144, .y0 = -32, .x1 = 6400, .y1 = 224},
  {"G2 ROOM 4", SNES_GAME_G2, 4, .x0 = 3328, .y0 = 464, .x1 = 3584, .y1 = 720},
  {"G2 ROOM 5", SNES_GAME_G2, 5, .x0 = 7424, .y0 = 1264, .x1 = 7680, .y1 = 1520},
  {"G2 ROOM 6", SNES_GAME_G2, 6, .x0 = 5376, .y0 = 480, .x1 = 5632, .y1 = 736},
  {"G2 ROOM 8", SNES_GAME_G2, 8, .x0 = 2816, .y0 = 480, .x1 = 3072, .y1 = 736},
  {"G3 ROOM 1", SNES_GAME_G3, 1, .x0 = 7936, .y0 = 2000, .x1 = 8192, .y1 = 2256},
  {"G3 ROOM 2", SNES_GAME_G3, 2, .x0 = 2816, .y0 = 1248, .x1 = 3072, .y1 = 1504},
  {"G3 ROOM 3", SNES_GAME_G3, 3, .x0 = 6656, .y0 = 224, .x1 = 6912, .y1 = 480},
  {"G3 ROOM 4", SNES_GAME_G3, 4, .x0 = 6144, .y0 = 992, .x1 = 6400, .y1 = 1248},
  {"G3 ROOM 5", SNES_GAME_G3, 5, .x0 = 3328, .y0 = 2768, .x1 = 3584, .y1 = 3024},
  {"G3 ROOM 6", SNES_GAME_G3, 6, .x0 = 6144, .y0 = 1504, .x1 = 6400, .y1 = 1760},
  {"G3 ROOM 7", SNES_GAME_G3, 7, .x0 = 5888, .y0 = 224, .x1 = 6144, .y1 = 480},
  {"G3 ROOM 8", SNES_GAME_G3, 8, .x0 = 7424, .y0 = 1248, .x1 = 7680, .y1 = 1504},
};
const int arena_ndefs = (int)(sizeof(arena_defs) / sizeof(arena_defs[0]));

//---------------------------------------------------------------- boot

static bool standable(int a) { return trio_at_solid(a) || (a >= 0x01 && a <= 0x0C) || a == TRIO_AT_LADDER_TOP; }

//A block the hero can stand on: solid with three empty blocks above.
static bool floor_at(const TrioGeo *g, int x, int y) {
  return standable(trio_geo_at(g, x, y)) && trio_geo_at(g, x, y - 16) == 0 && trio_geo_at(g, x, y - 32) == 0 && trio_geo_at(g, x, y - 48) == 0;
}

//A spawn point: flat floor three blocks wide, solid or a one-way platform
//(all three the same), room above.
static bool spawn_at(const TrioGeo *g, int x, int y) {
  bool soft = trio_geo_at(g, x, y) == TRIO_AT_LADDER_TOP;
  for (int k = -16; k <= 16; k += 16) {
    int a = trio_geo_at(g, x + k, y);
    if ((soft ? a != TRIO_AT_LADDER_TOP : !trio_at_solid(a)) || !floor_at(g, x + k, y)) return false;
  }
  return true;
}

//The box's open space split into regions of blocks connected through
//non-solid blocks; *main gets the biggest. A floor in a sealed room is in
//another region, so nobody spawns where the others can't reach.
#define REG_MAX 8192
static int air_regions(const ArenaStage *st, int16_t *reg, int *main) {
  const ArenaDef *d = st->def;
  int w = (d->x1 - d->x0) / 16, h = (d->y1 - d->y0) / 16;
  static int q[REG_MAX], size[REG_MAX];
  int nr = 0;
  *main = -1;
  if (w * h > REG_MAX) return 0;
  for (int k = 0; k < w * h; k++) reg[k] = -1;
  for (int k0 = 0; k0 < w * h; k0++) {
    if (reg[k0] >= 0 || trio_at_solid(trio_geo_at(&st->geo, d->x0 + k0 % w * 16, d->y0 + k0 / w * 16))) continue;
    int qh = 0, qt = 0;
    reg[k0] = (int16_t)nr;
    q[qt++] = k0;
    while (qh < qt) {
      int k = q[qh++], cx = k % w, cy = k / w;
      const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
      for (int e = 0; e < 4; e++) {
        int nx = cx + nb[e][0], ny = cy + nb[e][1];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        int nk = ny * w + nx;
        if (reg[nk] >= 0 || trio_at_solid(trio_geo_at(&st->geo, d->x0 + nx * 16, d->y0 + ny * 16))) continue;
        reg[nk] = (int16_t)nr;
        q[qt++] = nk;
      }
    }
    size[nr] = qt;
    if (*main < 0 || qt > size[*main]) *main = nr;
    nr++;
  }
  return w;
}

//Four spawn points spread over the box: the floor blocks inside it (two
//blocks from its sides) open to its main space, each the farthest from
//those already taken.
static void pick_spawns(ArenaStage *st) {
  const ArenaDef *d = st->def;
  static int16_t reg[REG_MAX];
  int main_reg, w = air_regions(st, reg, &main_reg);
  int cand[2048][2], n = 0;
  for (int y = d->y0 + 48; y < d->y1; y += 16)
    for (int x = d->x0 + 40; x < d->x1 - 40; x += 16) {
      if (w && reg[(y - 16 - d->y0) / 16 * w + (x - d->x0) / 16] != main_reg) continue;
      if (n < 2048 && spawn_at(&st->geo, x, y)) { cand[n][0] = x + 8; cand[n][1] = y; n++; }
    }
  //A spread of them for item drops.
  st->nfloor = 0;
  for (int k = 0; k < n && st->nfloor < STAGE_FLOORS; k += n / STAGE_FLOORS + 1) {
    st->floor_x[st->nfloor] = cand[k][0];
    st->floor_y[st->nfloor++] = cand[k][1];
  }
  if (!n) {
    for (int i = 0; i < 4; i++) { st->spawn_x[i] = (d->x0 + d->x1) / 2; st->spawn_y[i] = d->y1 - 16; }
    return;
  }
  //Start from the floor nearest the box's left third, low down.
  int best = 0, bs = 1 << 30;
  for (int k = 0; k < n; k++) {
    int sc = abs(cand[k][0] - (d->x0 + (d->x1 - d->x0) / 4)) + (d->y1 - cand[k][1]);
    if (sc < bs) { bs = sc; best = k; }
  }
  for (int i = 0; i < 4; i++) {
    st->spawn_x[i] = cand[best][0];
    st->spawn_y[i] = cand[best][1];
    int far = -1, fb = -1;
    for (int k = 0; k < n; k++) {
      int md = 1 << 30;
      for (int j = 0; j <= i; j++) {
        int dx = cand[k][0] - st->spawn_x[j], dy = (cand[k][1] - st->spawn_y[j]) * 2;
        int dd = dx * dx + dy * dy;
        if (dd < md) md = dd;
      }
      if (md > fb) { fb = md; far = k; }
    }
    best = far;
  }
}

//The level's block id that looks like the box's floor: the most common
//block on top of a floor inside it (used for added platforms).
static int floor_block(const Snes *s, const ArenaDef *d) {
  const TrioGame *g = trio_game(s);
  static int count[0x1000];
  memset(count, 0, sizeof(count));
  int best = -1;
  for (int y = d->y0 + 16; y < d->y1; y += 16)
    for (int x = d->x0; x < d->x1; x += 16) {
      if (trio_attr(s, x, y) != TRIO_AT_SOLID || trio_attr(s, x, y - 16) != 0) continue;
      uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
      uint16_t blk = rd16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3));
      if (blk >= 0x1000) continue;
      if (++count[blk] > (best < 0 ? 0 : count[best])) best = blk;
    }
  return best;
}

static void add_platforms(ArenaStage *st) {
  const ArenaDef *d = st->def;
  if (!d->nplat) return;
  Snes *s = st->view;
  const TrioGame *g = trio_game(s);
  int blk = floor_block(s, d);
  for (int k = 0; k < d->nplat; k++) {
    const ArenaPlat *p = &d->plat[k];
    for (int i = 0; i < p->w; i++) {
      int x = p->x + i * 16, y = p->y;
      trio_geo_set(&st->geo, x, y, TRIO_AT_SOLID);
      if (blk < 0) continue;
      uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
      if (scr) wr16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3), (uint16_t)blk);
    }
  }
}

//---------------------------------------------------------------- layouts

static int fill_any(const Snes *s, const ArenaDef *d);

//The level block that fills the box's walls: the most common solid block
//right under the floor block (else with solid blocks above and below).
static int block_at(const Snes *s, int x, int y) {
  const TrioGame *g = trio_game(s);
  uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
  return rd16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3));
}

static int fill_block(const Snes *s, const ArenaDef *d, int top) {
  static int under[0x1000];
  memset(under, 0, sizeof(under));
  int best = -1;
  for (int y = d->y0; y < d->y1 - 16; y += 16)
    for (int x = d->x0; x < d->x1; x += 16) {
      if (block_at(s, x, y) != top || !trio_at_solid(trio_attr(s, x, y + 16))) continue;
      int blk = block_at(s, x, y + 16);
      if (blk < 0x1000 && blk != top && ++under[blk] > (best < 0 ? 0 : under[best])) best = blk;
    }
  if (best >= 0) return best;
  return fill_any(s, d);
}

static int fill_any(const Snes *s, const ArenaDef *d) {
  const TrioGame *g = trio_game(s);
  static int count[0x1000];
  memset(count, 0, sizeof(count));
  int best = -1;
  for (int y = d->y0 + 16; y < d->y1 - 16; y += 16)
    for (int x = d->x0; x < d->x1; x += 16) {
      if (!trio_at_solid(trio_attr(s, x, y)) || !trio_at_solid(trio_attr(s, x, y - 16)) || !trio_at_solid(trio_attr(s, x, y + 16))) continue;
      uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
      uint16_t blk = rd16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3));
      if (blk >= 0x1000) continue;
      if (++count[blk] > (best < 0 ? 0 : count[best])) best = blk;
    }
  return best;
}

//A BG1 tile entry that draws nothing: a character of all zeroes.
static uint16_t blank_tile(const Snes *s) {
  PpuBgInfo bi;
  ppu_bg_info(&s->ppu, 0, &bi);
  for (int t = 0; t < 1024; t++) {
    const uint8_t *c = s->ppu.vram + (((uint32_t)bi.chr_base * 2 + (uint32_t)t * 32) & 0xFFFF);
    bool zero = true;
    for (int i = 0; i < 32 && zero; i++) zero = !c[i];
    if (zero) return (uint16_t)t;
  }
  return 0;
}

static void map_tiles(ArenaStage *st, int x, int y, const uint16_t t[4]) {
  int bx = x >> 4, by = y >> 4;
  for (int q = 0; q < 4; q++) {
    int tx = bx * 2 + (q & 1), ty = by * 2 + (q >> 1);
    if (tx < 0 || ty < 0 || tx >= VIEW_MAP_W || ty >= VIEW_MAP_H) continue;
    uint8_t *e = st->map1 + ((size_t)tx * VIEW_MAP_H + (size_t)ty) * 2;
    e[0] = (uint8_t)t[q];
    e[1] = (uint8_t)(t[q] >> 8);
  }
}

typedef struct Builder { ArenaStage *st; int top, fill; } Builder;

static void put(Builder *b, int x, int y, int blk, int attr) {
  const ArenaDef *d = b->st->def;
  if (x < d->x0 || x >= d->x1 || y < d->y0 || y >= d->y1) return;
  trio_geo_set(&b->st->geo, x, y, (uint8_t)attr);
  uint16_t t[4];
  trio_block_tiles(b->st->view, (uint16_t)blk, t);
  map_tiles(b->st, x, y, t);
}

//A solid slab: the floor block on top, the wall block under it down to the
//box's bottom. Block columns c0..c1-1, top row r (from the box's top).
static void slab(Builder *b, int c0, int c1, int r) {
  const ArenaDef *d = b->st->def;
  for (int c = c0; c < c1; c++)
    for (int y = d->y0 + r * 16; y < d->y1; y += 16)
      put(b, d->x0 + c * 16, y, y == d->y0 + r * 16 || b->fill < 0 ? b->top : b->fill, TRIO_AT_SOLID);
}

//A platform stood on from above and passed through from below (a ladder's
//top, as the moving platforms).
static void soft(Builder *b, int c0, int c1, int r) {
  const ArenaDef *d = b->st->def;
  for (int c = c0; c < c1; c++) put(b, d->x0 + c * 16, d->y0 + r * 16, b->top, TRIO_AT_LADDER_TOP);
}

//The hero's jump rises 55 px in all three games: a platform three blocks over a
//floor is reached from it, one four blocks over it is not.
#define STEP 3

//Rebuilt boxes: the level's collision in the box is taken out and every
//BG1 tile of the level with it (inside the box and around it: only the
//layout stands over the background), then a platform-fighter layout is built
//from the stage's own floor and wall blocks. Every platform is a jump
//(STEP blocks) over the floor or platform under it. The sides of the main
//platform are open: off them is a fall.
static void rebuild(ArenaStage *st) {
  const ArenaDef *d = st->def;
  Snes *s = st->view;
  int top = floor_block(s, d);
  Builder b = {st, top, top < 0 ? -1 : fill_block(s, d, top)};
  if (b.top < 0) return;
  uint16_t blank[4];
  for (int q = 0; q < 4; q++) blank[q] = blank_tile(s);
  for (size_t k = 0; k < (size_t)VIEW_MAP_W * VIEW_MAP_H; k++) {
    st->map1[k * 2] = (uint8_t)blank[0];
    st->map1[k * 2 + 1] = (uint8_t)(blank[0] >> 8);
  }
  for (int y = d->y0; y < d->y1; y += 16)
    for (int x = d->x0; x < d->x1; x += 16) trio_geo_set(&st->geo, x, y, 0);
  int W = (d->x1 - d->x0) / 16, H = (d->y1 - d->y0) / 16;
  int base = H - 6;                       //the main platform's top row
  if (base > 22) base = 22;
  int mid = W / 2;
  switch (d->layout) {
  case LAYOUT_BATTLEFIELD: {
    int m0 = W * 18 / 100, m1 = W - m0;
    slab(&b, m0, m1, base);
    int tw = W >= 48 ? 8 : 6, sw = W >= 48 ? 7 : 5;
    soft(&b, mid - tw / 2 - 1 - sw, mid - tw / 2 - 1, base - STEP);
    soft(&b, mid + tw / 2 + 1, mid + tw / 2 + 1 + sw, base - STEP);
    soft(&b, mid - tw / 2, mid + tw / 2, base - 2 * STEP);
    break;
  }
  case LAYOUT_FINAL:
    slab(&b, W * 12 / 100, W - W * 12 / 100, base);
    break;
  case LAYOUT_TIERS: {
    int a = W * 8 / 100, c = W * 32 / 100;
    slab(&b, a, c, base - 2);
    slab(&b, c, W - c, base);
    slab(&b, W - c, W - a, base - 2);
    soft(&b, mid - 4, mid + 4, base - STEP);
    soft(&b, a + 2, c - 1, base - 2 - STEP);
    soft(&b, W - c + 1, W - a - 2, base - 2 - STEP);
    break;
  }
  case LAYOUT_TOWERS: {
    //The pillars are walls to kick up (5 blocks); the bridge joins their tops.
    int m0 = W * 14 / 100, m1 = W - m0, p0 = W * 32 / 100, p1 = W - p0 - 3;
    slab(&b, m0, m1, base);
    slab(&b, p0, p0 + 3, base - 5);
    slab(&b, p1, p1 + 3, base - 5);
    soft(&b, p0 + 3, p1, base - 5);
    soft(&b, m0, p0 - 1, base - STEP);
    soft(&b, p1 + 4, m1, base - STEP);
    break;
  }
  }
  //The hazard, on or over the main platform (its top row is `base`; it
  //spans the middle of the box).
  int fy = d->y0 + base * 16, c0 = -1, c1 = -1;
  for (int c = 0; c < W; c++)
    if (trio_at_solid(trio_geo_at(&st->geo, d->x0 + c * 16, fy)) && !trio_at_solid(trio_geo_at(&st->geo, d->x0 + c * 16, fy - 16))) { if (c0 < 0) c0 = c; c1 = c + 1; }
  if (c0 < 0) return;
  int fx0 = d->x0 + c0 * 16, fx1 = d->x0 + c1 * 16, third = (fx1 - fx0) / 3;
  switch (d->hazard) {
  case HZ_ICE:
    st->haz[st->nhaz++] = (ArenaHazard){HZ_ICE, fx0, fy - 16, fx0 + third, fy + 16};
    st->haz[st->nhaz++] = (ArenaHazard){HZ_ICE, fx1 - third, fy - 16, fx1, fy + 16};
    break;
  case HZ_WIND:
    st->haz[st->nhaz++] = (ArenaHazard){HZ_WIND, fx0 + third, d->y0, fx1 - third, fy};
    break;
  case HZ_FIRE:
    st->haz[st->nhaz++] = (ArenaHazard){HZ_FIRE, fx0 + third / 2 - 8, fy - 72, fx0 + third / 2 + 8, fy};
    st->haz[st->nhaz++] = (ArenaHazard){HZ_FIRE, fx1 - third / 2 - 8, fy - 72, fx1 - third / 2 + 8, fy};
    break;
  }
}

//---------------------------------------------------------------- lanes

//Lanes for moving platforms: long runs of open air a jump (48 px) above a
//floor, and a second jump (96 px) above one, with room to stand on the
//platform. The longest of each kind, apart from each other.
static bool lane_cell(const TrioGeo *g, int x, int y, int up) {
  //The platform's row and three blocks above it clear (room to stand).
  for (int k = 0; k <= 3; k++)
    if (trio_geo_at(g, x, y - k * 16) != 0) return false;
  //Open air down to a floor `up` px below.
  for (int k = 1; k < up / 16; k++)
    if (trio_geo_at(g, x, y + k * 16) != 0) return false;
  int f = trio_geo_at(g, x, y + up);
  return trio_at_solid(f) || (f >= 1 && f <= 0x0C) || f == TRIO_AT_LADDER_TOP;
}

static void pick_lanes(ArenaStage *st) {
  const ArenaDef *d = st->def;
  st->nlane = 0;
  static const int ups[2] = {48, 96};
  for (int kind = 0; kind < 2; kind++) {
    ArenaLane best = {0};
    int best_len = 0;
    for (int y = d->y0 + 64; y < d->y1 - ups[kind]; y += 16) {
      int run0 = -1;
      for (int x = d->x0 + 32; x <= d->x1 - 32; x += 16) {
        bool ok = x < d->x1 - 32 && lane_cell(&st->geo, x, y, ups[kind]);
        if (ok && run0 < 0) run0 = x;
        if (!ok && run0 >= 0) {
          int len = x - run0;
          //Away from the other lane (or on the other side of it).
          bool apart = true;
          for (int k = 0; k < st->nlane; k++) {
            const ArenaLane *o = &st->lane[k];
            if (abs(o->y - y) < 80 && run0 < o->x1 + o->w * 16 && x > o->x0) apart = false;
          }
          if (len >= 8 * 16 && len > best_len && apart) {
            best_len = len;
            best = (ArenaLane){y, run0, x - 3 * 16, 3};
          }
          run0 = -1;
        }
      }
    }
    if (best_len) st->lane[st->nlane++] = best;
  }
}

//The BG2 camera with the game's camera settled at (camx, camy).
static void bg2_at(ArenaStage *st, int camx, int camy, int *bx, int *by) {
  stage_clamp(st, &camx, &camy, 256);
  for (int k = 0; k < 200; k++) {
    stage_view(st, camx, camy, 256, false);
    if (st->cx == camx && st->cy == camy && st->pcx == camx && st->pcy == camy) break;
  }
  for (int k = 0; k < 4; k++) stage_view(st, camx, camy, 256, false);
  trio_bg2_camera(st->view, bx, by);
  //16-bit scroll values: a camera left of or above the map's origin is negative.
  *bx = (int16_t)*bx;
  *by = (int16_t)*by;
}

//The whole-level BG2 map has holes and level structure where the game's
//camera never looks. The background of a rebuilt box is the part of it the
//game shows with its camera inside the box: BG2's camera is read with the
//game's at the box's four sides.
static void find_background(ArenaStage *st) {
  const ArenaDef *d = st->def;
  int mx = (d->x0 + d->x1) / 2 - 128, my = (d->y0 + d->y1) / 2 - 112, t;
  bg2_at(st, d->x0, my, &st->bg_xl, &t);
  bg2_at(st, d->x1 - 256, my, &st->bg_xr, &t);
  bg2_at(st, mx, d->y0, &t, &st->bg_yt);
  bg2_at(st, mx, d->y1 - 224, &t, &st->bg_yb);
  bg2_at(st, mx, my, &t, &t);
  st->bg_x0 = st->bg_xl < st->bg_xr ? st->bg_xl : st->bg_xr;
  st->bg_x1 = (st->bg_xl > st->bg_xr ? st->bg_xl : st->bg_xr) + 256;
  st->bg_y0 = st->bg_yt < st->bg_yb ? st->bg_yt : st->bg_yb;
  st->bg_y1 = (st->bg_yt > st->bg_yb ? st->bg_yt : st->bg_yb) + 224;
  st->bg = malloc((size_t)(st->bg_x1 - st->bg_x0) * (size_t)(st->bg_y1 - st->bg_y0) * sizeof(uint32_t));
}

bool stage_boot(ArenaStage *st, const ArenaDef *def, const uint8_t *rom, uint32_t rom_size, bool recomp) {
  memset(st, 0, sizeof(*st));
  st->def = def;
  st->view = malloc(sizeof(Snes));
  Snes *s = st->view;
  bool skip = snes_skip_render;
  snes_skip_render = true;
  if (!trio_boot_stage(s, rom, rom_size, recomp, def->stage, NULL)) { snes_skip_render = skip; return false; }
  const TrioGame *g = trio_game(s);
  //Glide the camera from the stage start to the box, 8 px a frame, so the
  //game's area loader brings in each area's graphics on the way. The player
  //is carried along at the top of the level, where there are no blocks to
  //hurt him.
  int cx = trio_cam_x(s), cy = trio_cam_y(s);
  int tx = (def->x0 + def->x1) / 2 - 128, ty = (def->y0 + def->y1) / 2 - 112;
  if (tx < def->x0) tx = def->x0;
  if (ty < def->y0) ty = def->y0;
  for (int k = 0; k < 4000 && (cx != tx || cy != ty); k++) {
    cx += cx < tx ? (tx - cx > 8 ? 8 : tx - cx) : -(cx - tx > 8 ? 8 : cx - tx);
    cy += cy < ty ? (ty - cy > 8 ? 8 : ty - cy) : -(cy - ty > 8 ? 8 : cy - ty);
    trio_place(s, cx + 128, 16, true);
    trio_camera(s, cx, cy);
    trio_set_hp(s, 16);
    trio_quiet(s);
    s->pad[0] = s->pad[1] = 0;
    snes_run_frame(s);
  }
  for (int i = 0; i < 30; i++) {
    trio_place(s, cx + 128, 16, true);
    trio_camera(s, cx, cy);
    trio_quiet(s);
    snes_run_frame(s);
  }
  (void)g;
  trio_geo_read(s, &st->geo, def->x0, def->y0, def->x1, def->y1);
  //Whole-level tilemaps for the picture (no streaming involved).
  st->map1 = malloc((size_t)VIEW_MAP_W * VIEW_MAP_H * 2);
  st->map2 = malloc((size_t)VIEW_MAP_W * VIEW_MAP_H * 2);
  trio_build_bg1_map(s, st->map1, VIEW_MAP_W, VIEW_MAP_H);
  trio_build_bg2_map(s, st->map2, VIEW_MAP_W, VIEW_MAP_H);
  st->plat_blk = floor_block(st->view, def);
  if (def->layout != LAYOUT_LEVEL) rebuild(st);
  add_platforms(st);
  pick_spawns(st);
  //Moving platforms where there is nothing else to stand on: the level's
  //own boxes and the flat layout.
  if (def->layout == LAYOUT_LEVEL || def->layout == LAYOUT_FINAL) pick_lanes(st);
  memset(&st->enh, 0, sizeof(st->enh));
  st->enh.wide = TRIO_ARENA_WIDE;
  st->enh.wide_layers = 0x13;
  st->enh.layer_mask = 0x03;
  st->enh.vmap[0] = st->map1;
  st->enh.vmap[1] = st->map2;
  st->enh.vmap_w[0] = st->enh.vmap_w[1] = VIEW_MAP_W;
  st->enh.vmap_h[0] = st->enh.vmap_h[1] = VIEW_MAP_H;
  s->ppu.enh = &st->enh;
  st->cx = st->pcx = cx;
  st->cy = st->pcy = cy;
  if (def->layout != LAYOUT_LEVEL) find_background(st);
  snes_skip_render = skip;
  st->ok = true;
  return true;
}

void stage_free(ArenaStage *st) {
  free(st->view);
  free(st->map1);
  free(st->map2);
  free(st->bg);
  trio_geo_free(&st->geo);
  memset(st, 0, sizeof(*st));
}

void stage_clamp(const ArenaStage *st, int *camx, int *camy, int w) {
  const ArenaDef *d = st->def;
  int side = (w - 256) / 2;
  //The classic 256 px screen's left edge: the view reaches `side` beyond it.
  int lo = d->x0 + side, hi = d->x1 - 256 - side;
  if (hi < lo) lo = hi = (d->x0 + d->x1) / 2 - 128;
  if (*camx < lo) *camx = lo;
  if (*camx > hi) *camx = hi;
  int tlo = d->y0, thi = d->y1 - 224;
  if (thi < tlo) thi = tlo;
  if (*camy < tlo) *camy = tlo;
  if (*camy > thi) *camy = thi;
}

void stage_clamp_pic(const ArenaStage *st, int *px, int *py, int w, int h) {
  const ArenaDef *d = st->def;
  //A little beyond the box's sides and top, more under it (falls show).
  int x0 = d->x0 - 32, x1 = d->x1 + 32, y0 = d->y0 - 48, y1 = d->y1 + 48;
  if (x1 - x0 <= w) *px = (x0 + x1) / 2 - w / 2;
  else { if (*px < x0) *px = x0; if (*px > x1 - w) *px = x1 - w; }
  if (y1 - y0 <= h) *py = (y0 + y1) / 2 - h / 2;
  else { if (*py < y0) *py = y0; if (*py > y1 - h) *py = y1 - h; }
}

//Rebuilt layouts: a slab is its floor block over rows of one wall block.
//Under the first row the wall darkens with depth (to 3/8 at 70 px), so
//the floor's edge reads and the repeated block does not.
static void shade_slabs(const ArenaStage *st, uint32_t *img, int stride, int w, int h, int px, int py) {
  const ArenaDef *d = st->def;
  int cx0 = (px < d->x0 ? d->x0 : px) >> 4, cx1 = ((px + w > d->x1 ? d->x1 : px + w) + 15) >> 4;
  for (int cx = cx0; cx < cx1; cx++) {
    int run = -1;
    for (int wy = d->y0; wy < d->y1 && wy < py + h; wy += 16) {
      if (!trio_at_solid(trio_geo_at(&st->geo, cx * 16, wy))) { run = -1; continue; }
      if (run < 0) run = wy;
      if (wy == run || wy + 16 <= py) continue;
      for (int y = wy < py ? py : wy; y < wy + 16 && y < py + h; y++) {
        int f = 256 - (y - run - 16) * 3;
        if (f < 96) f = 96;
        uint32_t *row = img + (size_t)(y - py) * (size_t)stride;
        for (int x = cx * 16 < px ? px : cx * 16; x < cx * 16 + 16 && x < px + w; x++) {
          uint32_t c = row[x - px];
          row[x - px] = ((c >> 16 & 255) * (uint32_t)f >> 8) << 16 | ((c >> 8 & 255) * (uint32_t)f >> 8) << 8 | ((c & 255) * (uint32_t)f >> 8);
        }
      }
    }
  }
}

static uint32_t scale(uint32_t c, unsigned f) {
  return ((c >> 16 & 255) * f >> 8) << 16 | ((c >> 8 & 255) * f >> 8) << 8 | ((c & 255) * f >> 8);
}

static uint32_t lighten(uint32_t c, unsigned f) {
  unsigned r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
  return (r + ((255 - r) * f >> 8)) << 16 | (g + ((255 - g) * f >> 8)) << 8 | (b + ((255 - b) * f >> 8));
}

static uint32_t row_average(const uint32_t *row, int n) {
  unsigned long r = 0, g = 0, b = 0;
  for (int x = 0; x < n; x++) { r += row[x] >> 16 & 255; g += row[x] >> 8 & 255; b += row[x] & 255; }
  return (uint32_t)(r / (unsigned)n) << 16 | (uint32_t)(g / (unsigned)n) << 8 | (uint32_t)(b / (unsigned)n);
}

#define BG_DIM 184               //the background's brightness, of 256

//A rebuilt box's background: its BG2 window (find_background), drawn fresh
//(the game animates tiles and colors), scrolled at the game's own rate
//(the game's 256 x 224 screen taken as the middle of the picture), mirrored
//side by side to fill any width and ended in a plain color above and
//below. Dimmed, so the platforms stand out.
//
//Holes: where the level's own walls covered BG2 the map has nothing, which
//shows as rectangles of the backdrop color in a wall. With a dark backdrop,
//such pixels in a row that is mostly drawn take the row's average color (a
//night sky's rows are mostly backdrop and stay).
static void draw_background(const ArenaStage *st, uint32_t *img, int stride, int w, int h, int px, int py) {
  const Ppu *p = &st->view->ppu;
  const ArenaDef *d = st->def;
  int bw = st->bg_x1 - st->bg_x0, bh = st->bg_y1 - st->bg_y0;
  uint32_t back = ppu_backdrop(p);
  for (int k = 0; k < bw * bh; k++) st->bg[k] = back;
  for (int prio = 0; prio < 2; prio++)
    ppu_draw_bg_map(p, 1, st->map2, VIEW_MAP_W, VIEW_MAP_H, st->bg_x0, st->bg_y0, prio, st->bg, bw, bw, bh);
  bool dark = (back >> 16 & 255) + (back >> 8 & 255) + (back & 255) < 96;
  for (int y = 0; y < bh && dark; y++) {
    uint32_t *row = st->bg + (size_t)y * (size_t)bw;
    int drawn = 0;
    unsigned long r = 0, g = 0, b = 0;
    for (int x = 0; x < bw; x++)
      if (row[x] != back) { drawn++; r += row[x] >> 16 & 255; g += row[x] >> 8 & 255; b += row[x] & 255; }
    if (drawn == bw || drawn * 5 < bw * 3) continue;
    uint32_t avg = (uint32_t)(r / (unsigned)drawn) << 16 | (uint32_t)(g / (unsigned)drawn) << 8 | (uint32_t)(b / (unsigned)drawn);
    for (int x = 0; x < bw; x++)
      if (row[x] == back) row[x] = avg;
  }
  uint32_t top = scale(row_average(st->bg, bw), BG_DIM), bottom = scale(row_average(st->bg + (size_t)(bh - 1) * (size_t)bw, bw), BG_DIM);
  int spanx = d->x1 - d->x0 - 256, spany = d->y1 - d->y0 - 224;
  int mx = (w - 256) / 2, my = (h - 224) / 2;
  int u0 = st->bg_xl + (spanx > 0 ? (px + mx - d->x0) * (st->bg_xr - st->bg_xl) / spanx : 0) - mx - st->bg_x0;
  int v0 = st->bg_yt + (spany > 0 ? (py + my - d->y0) * (st->bg_yb - st->bg_yt) / spany : 0) - my - st->bg_y0;
  for (int y = 0; y < h; y++) {
    int v = v0 + y;
    uint32_t *row = img + (size_t)y * (size_t)stride;
    if (v < 0 || v >= bh) {
      for (int x = 0; x < w; x++) row[x] = v < 0 ? top : bottom;
      continue;
    }
    const uint32_t *src = st->bg + (size_t)v * (size_t)bw;
    for (int x = 0; x < w; x++) {
      int u = (u0 + x) % (2 * bw);
      if (u < 0) u += 2 * bw;
      row[x] = scale(src[u < bw ? u : 2 * bw - 1 - u], BG_DIM);
    }
  }
}

//Rebuilt layouts: the top of everything stood on gets a light edge (the
//stages' floor blocks are often as dark as their backgrounds).
static void light_edges(const ArenaStage *st, uint32_t *img, int stride, int w, int h, int px, int py) {
  const ArenaDef *d = st->def;
  for (int wy = (py < d->y0 ? d->y0 : py) & ~15; wy < d->y1 && wy < py + h; wy += 16)
    for (int wx = (px < d->x0 ? d->x0 : px) & ~15; wx < d->x1 && wx < px + w; wx += 16) {
      int a = trio_geo_at(&st->geo, wx, wy), up = trio_geo_at(&st->geo, wx, wy - 16);
      if (!(trio_at_solid(a) || a == TRIO_AT_LADDER_TOP) || trio_at_solid(up) || up == TRIO_AT_LADDER_TOP) continue;
      for (int r = 0; r < 2; r++) {
        int y = wy + r - py;
        if (y < 0 || y >= h) continue;
        for (int x = wx - px < 0 ? 0 : wx - px; x < wx - px + 16 && x < w; x++)
          img[(size_t)y * (size_t)stride + (size_t)x] = lighten(img[(size_t)y * (size_t)stride + (size_t)x], r ? 64 : 150);
      }
    }
}

void stage_draw(const ArenaStage *st, uint32_t *img, int stride, int w, int h, int px, int py) {
  const Ppu *p = &st->view->ppu;
  if (st->bg) {
    //A rebuilt box: its background, then the layout over all of it.
    draw_background(st, img, stride, w, h, px, py);
    for (int prio = 0; prio < 2; prio++) ppu_draw_bg_map(p, 0, st->map1, VIEW_MAP_W, VIEW_MAP_H, px, py, prio, img, stride, w, h);
    shade_slabs(st, img, stride, w, h, px, py);
    light_edges(st, img, stride, w, h, px, py);
    return;
  }
  uint32_t back = ppu_backdrop(p);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) img[y * stride + x] = back;
  //The background layer keeps its parallax offset from the foreground: its
  //camera where the display copy's camera is, moved by as much as the
  //picture is from it.
  int bx, by;
  trio_bg2_camera(st->view, &bx, &by);
  int b2x = bx + (px - st->cx), b2y = by + (py - st->cy);
  //Mode 1 without BG3: background low, foreground low, background high,
  //foreground high.
  for (int prio = 0; prio < 2; prio++) {
    ppu_draw_bg_map(p, 1, st->map2, VIEW_MAP_W, VIEW_MAP_H, b2x, b2y, prio, img, stride, w, h);
    ppu_draw_bg_map(p, 0, st->map1, VIEW_MAP_W, VIEW_MAP_H, px, py, prio, img, stride, w, h);
  }
}

void stage_view(ArenaStage *st, int camx, int camy, int w, bool objs) {
  Snes *s = st->view;
  stage_clamp(st, &camx, &camy, w);
  //Moved a few pixels a frame at most, like the game's own camera.
  int cx = trio_cam_x(s), cy = trio_cam_y(s);
  int dx = camx - cx, dy = camy - cy;
  if (dx > 24) dx = 24;
  if (dx < -24) dx = -24;
  if (dy > 24) dy = 24;
  if (dy < -24) dy = -24;
  cx += dx;
  cy += dy;
  trio_camera(s, cx, cy);
  //The display copy's player stays out of sight at the camera.
  s->wram[trio_game(s)->player + 0x0E] &= (uint8_t)~1;
  trio_set_hp(s, 16);
  trio_quiet_enemies(s);
  //The arena's items as the game's own item objects (the kind placed in
  //levels: no pop-up, no timeout), put back where the arena has them every
  //frame. So that it picks nothing up, the hidden player stands at the top
  //of the camera, as far across it as it can get from every item.
  int best_x = cx + 128, best_d = -1;
  for (int x = cx + 16; x <= cx + 240; x += 16) {
    int d = 1 << 30;
    for (int k = 0; k < STAGE_ITEMS; k++)
      if (st->item_kind[k] > TRIO_ITEM_NONE) {
        int dx = abs(st->item_x[k] - x), dy = abs(st->item_y[k] - (cy + 24));
        if (dx + dy < d) d = dx + dy;
      }
    if (d > best_d) { best_d = d; best_x = x; }
  }
  trio_place(s, best_x, cy + 24, true);
  for (int k = 0; k < STAGE_ITEMS; k++) {
    if (st->item_kind[k] > TRIO_ITEM_NONE) trio_put_item(s, k, st->item_kind[k], st->item_x[k], st->item_y[k]);
    else trio_clear_item(s, k);
  }
  const TrioGame *g = trio_game(s);
  for (uint32_t a = g->item_first + STAGE_ITEMS * g->item_size; a < g->item_end; a += g->item_size) s->wram[a] = 0;
  st->pcx = st->cx;
  st->pcy = st->cy;
  st->cx = cx;
  st->cy = cy;
  st->enh.wide = (w - 256) / 2;
  st->enh.layer_mask = objs ? 0x13 : 0x03;
  st->enh.vmap_hint_x[0] = cx;
  st->enh.vmap_hint_y[0] = cy;
  int bx, by;
  trio_bg2_camera(s, &bx, &by);
  st->enh.vmap_hint_x[1] = bx;
  st->enh.vmap_hint_y[1] = by;
  if (getenv("ARENA_NOVMAP")) st->enh.vmap[1] = st->enh.vmap[0] = NULL;
  s->pad[0] = s->pad[1] = 0;
  bool skip = snes_skip_render;
  snes_skip_render = !st->render;
  snes_run_frame(s);
  snes_skip_render = skip;
  int16_t tmp[2048];
  while (apu_pop_samples(&s->apu, tmp, 1024) > 0) {}
}

bool stage_install(const ArenaStage *st, Snes *s) {
  if (!trio_geo_install(s, &st->geo)) return false;
  const TrioGame *g = trio_game(s);
  //Nothing of the copy's own stage stays around.
  for (uint32_t a = g->event_first; a < g->event_end; a += g->event_size) s->wram[a] = 0;
  for (uint32_t a = g->area_first; a < g->area_end; a += g->area_size) s->wram[a] = 0;
  trio_quiet(s);
  stage_follow(st, s);
  return true;
}

void stage_follow(const ArenaStage *st, Snes *s) {
  TrioPlayer p;
  trio_read_player(s, &p);
  int cx = p.x - 128, cy = p.y - 112;
  stage_clamp(st, &cx, &cy, 256);
  trio_camera(s, cx, cy);
  trio_quiet(s);
}
