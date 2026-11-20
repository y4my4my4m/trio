#include "sprites.h"
#include "boss.h"
#include <stdlib.h>
#include <string.h>

//Battle mode draws every fighter's world sprites itself, in world
//coordinates, from the same data the game's sprite builder reads (docs/
//BATTLE.md). A copy's OAM only reaches about a screen around its own
//camera; built this way a fighter's shots stay visible across the arena.

static inline uint16_t rd16w(const Snes *s, uint32_t a) { return (uint16_t)(s->wram[a & 0x1FFFF] | s->wram[(a + 1) & 0x1FFFF] << 8); }

static uint8_t rom8(const Snes *s, uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16);
  if ((bank & 0xFE) == 0x7E) return s->wram[a & 0x1FFFF];
  uint32_t off = ((uint32_t)(bank & 0x7F) << 15) | (a & 0x7FFF);
  return s->rom[off & (s->rom_size - 1)];
}
static uint16_t rom16(const Snes *s, uint32_t a) { return (uint16_t)(rom8(s, a) | rom8(s, (a & 0xFF0000) | ((a + 1) & 0xFFFF)) << 8); }
static uint32_t rom24(const Snes *s, uint32_t a) { return rom16(s, a) | (uint32_t)rom8(s, (a & 0xFF0000) | ((a + 2) & 0xFFFF)) << 16; }

static void add(ArenaSprites *o, int ox, int oy, int x, int y, uint8_t tile, uint8_t attr, bool big) {
  if (o->n >= SPR_MAX) return;
  o->s[o->n++] = (ArenaSpr){ox + x, oy + y, tile, attr, big, ox, oy};
}

//G2 and G3: the Cx4 object list the frame's OAM was built from ($6220, 16
//bytes per object: x16, y16 world, attr, tile, attr bits, piece list ptr24;
//count at $6620). Same piece rules as the builder (src/snes/cx4.c).
//One entry of the list (at src in the Cx4 RAM).
static void cx4_object(const Snes *s, int src, ArenaSprites *o) {
  const uint8_t *ram = s->cx4.ram;
  int ox = (int16_t)(ram[src] | ram[src + 1] << 8), oy = (int16_t)(ram[src + 2] | ram[src + 3] << 8);
  uint8_t attr = ram[src + 4] | ram[src + 6], tile = ram[src + 5];
  uint32_t p = (uint32_t)(ram[src + 7] | ram[src + 8] << 8 | ram[src + 9] << 16);
  int cnt = rom8(s, p);
  if (cnt == 0) { add(o, ox, oy, 0, 0, tile, attr, true); return; }
  p++;
  for (; cnt > 0; cnt--, p += 4) {
    uint8_t fl = rom8(s, p);
    int big = fl & 0x20, sz = big ? 16 : 8;
    int x = (int8_t)rom8(s, p + 1), y = (int8_t)rom8(s, p + 2);
    if (attr & 0x40) x = -x - sz;
    if (attr & 0x80) y = -y - sz;
    add(o, ox, oy, x, y, (uint8_t)(tile + rom8(s, p + 3)), attr ^ (fl & 0xC0), big != 0);
  }
}

static void from_cx4(const Snes *s, ArenaSprites *o) {
  int nobj = s->cx4.ram[0x620];
  for (int n = 0; n < nobj && n < 64; n++) cx4_object(s, 0x220 + n * 16, o);
}

//G1: the CPU builder (80:D6BD) reads each object: sprite set +$16 (table
//8D:8000, 3 bytes each), frame +$17, tile base +$18, Y offset +$19, attr
//and facing +$11. Pieces: count, then dx, dy, tile, attr (bit 5 large),
//mirrored with the facing.
static void g1_object(const Snes *s, uint32_t a, ArenaSprites *o) {
  const uint8_t *ob = s->wram + a;
  if (!(ob[0x0E] & 1)) return;
  uint8_t face = ob[0x11] & 0x40, pal = ob[0x11] & 0x3F, base = ob[0x18];
  int ox = (int16_t)rd16w(s, a + 0x05), oy = (int16_t)rd16w(s, a + 0x08) + (int8_t)ob[0x19];
  uint32_t set = rom24(s, 0x8D8000 + 3u * ob[0x16]);
  uint32_t p = rom24(s, (set & 0xFF0000) | (uint16_t)(set + 3u * (ob[0x17] & 0x7F)));
  int cnt = rom8(s, p);
  if (cnt > 64) return;   //(G1 Raider's walker has 44)
  for (int k = 0; k < cnt; k++) {
    uint32_t q = (p & 0xFF0000) | (uint16_t)(p + 4u * (unsigned)k);
    uint8_t fattr = rom8(s, (q & 0xFF0000) | (uint16_t)(q + 4));
    bool big = fattr & 0x20;
    int dx = (int8_t)rom8(s, (q & 0xFF0000) | (uint16_t)(q + 1)), dy = (int8_t)rom8(s, (q & 0xFF0000) | (uint16_t)(q + 2));
    if (face) dx = -dx - (big ? 16 : 8);
    uint8_t tile = (uint8_t)(rom8(s, (q & 0xFF0000) | (uint16_t)(q + 3)) + base);
    uint8_t attr = (uint8_t)(((fattr & 0xCE) | pal) ^ face);
    add(o, ox, oy, dx, dy, tile, attr, big);
  }
}

static void g1_table(const Snes *s, uint32_t first, uint32_t end, uint32_t size, ArenaSprites *o) {
  for (uint32_t a = first; a < end; a += size)
    if (s->wram[a]) g1_object(s, a, o);
}

void spr_collect_items(const Snes *s, ArenaSprites *o) {
  o->n = 0;
  const TrioGame *g = trio_game(s);
  if (!s->has_cx4) {
    if (s->game == SNES_GAME_G1) g1_table(s, g->item_first, g->item_end, g->item_size, o);
    return;
  }
  //G2 / G3: the Cx4 list's objects standing where an item is.
  const uint8_t *ram = s->cx4.ram;
  int nobj = ram[0x620];
  for (int n = 0; n < nobj && n < 64; n++) {
    int src = 0x220 + n * 16;
    int ox = (int16_t)(ram[src] | ram[src + 1] << 8), oy = (int16_t)(ram[src + 2] | ram[src + 3] << 8);
    bool item = false;
    for (uint32_t a = g->item_first; a < g->item_end && !item; a += g->item_size)
      item = s->wram[a] && abs((int16_t)rd16w(s, a + 0x05) - ox) <= 1 && abs((int16_t)rd16w(s, a + 0x08) - oy) <= 1;
    if (item) cx4_object(s, src, o);
  }
}

void spr_collect(const Snes *s, ArenaSprites *o) {
  o->n = 0;
  if (s->has_cx4) { from_cx4(s, o); return; }
  if (s->game != SNES_GAME_G1) return;
  //Front to back: the player's effects, the player, his shots, the boss
  //and enemies, their shots, the stage's effects (death orbs).
  //A boss fighter's copy: the hero is only its stand-in target (src/arena/boss.c),
  //not shown.
  if (trio_boss_kind(s) == TRIO_BOSS_NONE) {
    g1_table(s, 0x0C38, 0x0C98, 0x20, o);
    g1_object(s, 0x0BA8, o);
    g1_table(s, 0x1228, 0x1428, 0x40, o);
  }
  g1_table(s, 0x0C98, 0x0E18, 0x20, o);
  g1_table(s, 0x0E68, 0x1228, 0x40, o);
  g1_table(s, 0x1428, 0x1628, 0x40, o);
  g1_table(s, 0x1928, 0x1D08, 0x20, o);
}

void spr_draw(const Snes *s, const ArenaSprites *o, uint32_t *img, int stride, int w, int h, int dx, int dy) {
  //The first sprite is in front: draw from the back.
  for (int i = o->n - 1; i >= 0; i--) {
    const ArenaSpr *p = &o->s[i];
    ppu_draw_sprite(&s->ppu, p->x + dx, p->y + dy, p->tile, p->attr, p->big, img, stride, w, h);
  }
}
