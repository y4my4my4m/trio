#include "trio.h"
#include <stdlib.h>
#include <string.h>
#include "game/recomp_rt.h"

static inline uint16_t rd16(const Snes *s, uint32_t a) { return (uint16_t)(s->wram[a & 0x1FFFF] | s->wram[(a + 1) & 0x1FFFF] << 8); }
static inline void wr16(Snes *s, uint32_t a, uint16_t v) { s->wram[a & 0x1FFFF] = (uint8_t)v; s->wram[(a + 1) & 0x1FFFF] = (uint8_t)(v >> 8); }
static inline uint32_t rd24(const Snes *s, uint32_t a) { return rd16(s, a) | (uint32_t)s->wram[(a + 2) & 0x1FFFF] << 16; }

//A byte of the bus at a 24-bit address: WRAM (banks $7E/$7F and the low
//mirror) or LoROM.
static uint8_t bus8(const Snes *s, uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16);
  if ((bank & 0xFE) == 0x7E) return s->wram[a & 0x1FFFF];
  if (!(bank & 0x40) && (a & 0xFFFF) < 0x2000) return s->wram[a & 0x1FFF];
  uint32_t off = ((uint32_t)(bank & 0x7F) << 15) | (a & 0x7FFF);
  return s->rom[off & (s->rom_size - 1)];
}

#define G(s) trio_game(s)

//---------------------------------------------------------------- upgrades

void trio_set_upgrades(Snes *s, const TrioUpgrades *up) {
  const TrioGame *g = G(s);
  int n = 0;
  for (int b = 0; b < 8; b++) n += up->hearts >> b & 1;
  int max = 16 + 2 * n;
  s->wram[g->parts] = (uint8_t)((s->wram[g->parts] & 0xF0) | (up->parts & 0x0F));
  s->wram[g->hearts] = up->hearts;
  s->wram[g->max_hp] = (uint8_t)max;
  switch (g->id) {
  case SNES_GAME_G1:
    //The Fireball upgrade: bit 7 of $1F7E (the move needs full health).
    if (up->special) s->wram[0x1F7E] |= 0x80;
    break;
  case SNES_GAME_G2:
    //The Uppercut: bit 7 of $1FB1 (obtained).
    if (up->special) s->wram[0x1FB1] |= 0x80;
    break;
  case SNES_GAME_G3:
    s->wram[0x1FD7] = (uint8_t)((s->wram[0x1FD7] & 0x0F) | (up->chips & 0xF0));
    break;
  }
}

//---------------------------------------------------------------- boot

static bool controllable(const Snes *s) {
  const TrioGame *g = G(s);
  const uint8_t *p = s->wram + g->player;
  return s->wram[g->mode_top] == 2 && p[0x01] == 2 && p[0x02] == TRIO_ACT_IDLE && (p[g->ground_off] & g->ground_mask);
}

bool trio_boot_stage(Snes *s, const uint8_t *rom, uint32_t rom_size, bool recomp, int stage, const TrioUpgrades *up) {
  snes_init(s, rom, rom_size);
  snes_reset(s);
  rt_install(s, recomp);
  const TrioGame *g = G(s);
  if (!g->player) return false;
  const uint8_t *p = s->wram + g->player;
  for (int fr = 0; fr < 6000; fr++) {
    uint16_t pad = 0;
    uint8_t top = s->wram[g->mode_top];
    if (top != 2 && fr >= g->start && (fr - g->start) % 60 < 2) pad = PAD_START;
    if (top == 2 && s->wram[g->mode_game] == 2) {
      if (s->wram[g->mode_sub] == g->load_sub) {
        s->wram[g->stage] = (uint8_t)stage;
        //TRIO_STAGE_SUB: the byte after the stage number too (G2's final
        //stage is stage 5 with $1FAE = 4).
        if (stage >> 8) s->wram[g->stage + 1] = (uint8_t)((stage >> 8) - 1);
      }
      if (up) trio_set_upgrades(s, up);
    }
    if (controllable(s)) {
      s->pad[0] = 0;
      if (up) {
        trio_set_upgrades(s, up);
        trio_set_hp(s, s->wram[g->max_hp]);
      }
      return true;
    }
    //G3's helmet part shows the stage map after the warp-in (action $78)
    //until the player moves on: end it.
    if (g->id == SNES_GAME_G3 && p[0x01] == 2 && p[0x02] == 0x78) {
      s->wram[g->player + 0x02] = TRIO_ACT_IDLE;
      s->wram[g->player + 0x03] = 0;
    }
    //Dialogues (G1's fortress, G3's intro) page on B.
    if (top == 2 && p[0x01] == 2 && fr % 40 < 2 && p[0x02] != TRIO_ACT_IDLE && p[0x02] != TRIO_ACT_WARP) pad = PAD_B;
    s->pad[0] = pad;
    s->pad[1] = 0;
    snes_run_frame(s);
  }
  return false;
}

//---------------------------------------------------------------- player

static int rise(const Snes *s) { return trio_is_blade(s) ? TRIO_BLADE_RISE : 0; }

void trio_read_player(const Snes *s, TrioPlayer *p) {
  const TrioGame *g = G(s);
  const uint32_t b = g->player;
  const uint8_t *o = s->wram + b;
  p->x = rd16(s, b + 0x05);
  p->y = rd16(s, b + 0x08) + rise(s);
  p->vx = (int16_t)rd16(s, b + 0x1A);
  p->vy = (int16_t)rd16(s, b + 0x1C);
  p->action = o[0x02];
  p->step = o[0x03];
  p->facing_right = (o[0x11] & 0x40) != 0;
  p->ground = (o[g->ground_off] & g->ground_mask) != 0;
  p->hp = o[0x27] & 0x7F;
  //G1 counts down from $B4 while charging (level at +$58).
  if (g->charge_down) p->charge = o[0x58] ? 0xB4 - o[g->charge_off] : 0;
  else p->charge = o[g->charge_off];
  p->iframes = o[0x6B];
  p->shots = o[0x35];
}

void trio_place(Snes *s, int x, int y, bool facing_right) {
  const uint32_t b = G(s)->player;
  y -= rise(s);
  wr16(s, b + 0x05, (uint16_t)x);
  wr16(s, b + 0x22, (uint16_t)x);
  wr16(s, b + 0x08, (uint16_t)y);
  wr16(s, b + 0x24, (uint16_t)y);
  s->wram[b + 0x04] = 0;
  s->wram[b + 0x07] = 0;
  wr16(s, b + 0x1A, 0);
  wr16(s, b + 0x1C, 0);
  if (facing_right) s->wram[b + 0x11] |= 0x40;
  else s->wram[b + 0x11] &= (uint8_t)~0x40;
}

void trio_warp_in(Snes *s) {
  const uint32_t b = G(s)->player;
  s->wram[b + 0x02] = TRIO_ACT_WARP;
  s->wram[b + 0x03] = 0;
}

int trio_anim(const Snes *s) { return s->wram[G(s)->player + 0x17] & 0x7F; }

void trio_set_hp(Snes *s, int hp) {
  uint8_t *o = &s->wram[G(s)->player + 0x27];
  *o = (uint8_t)(0x80 | (hp < 0 ? 0 : hp > 0x7F ? 0x7F : hp));
}

int trio_max_hp(const Snes *s) {
  int m = s->wram[G(s)->max_hp];
  return m ? m : 16;
}

//---------------------------------------------------------------- Blade (G3)

//The story event that hands Blade the controls (03:AC57) sets $0A8E to 2
//and bit 1 of $1FFF; the flags switch the player code, and a pause and
//unpause makes the game load his palette and HUD on the way out of the menu
//(00:D664 -> 04:DCE1, which picks set $5C while $0A8E is set). A hop then
//streams his standing pose with his own tiles.
void trio_make_blade(Snes *s) {
  if (s->game != SNES_GAME_G3) return;
  s->wram[0x0A8E] = 2;
  s->wram[0x1FFF] |= 2;
  TrioPlayer p;
  trio_read_player(s, &p);
  int cx = trio_cam_x(s), cy = trio_cam_y(s);
  static const struct { int frames; uint16_t pad; } seq[] = {{2, PAD_START}, {60, 0}, {2, PAD_START}, {110, 0}, {6, PAD_B}, {60, 0}};
  for (size_t k = 0; k < sizeof(seq) / sizeof(seq[0]); k++)
    for (int i = 0; i < seq[k].frames; i++) {
      s->pad[0] = seq[k].pad;
      s->pad[1] = 0;
      trio_camera(s, cx, cy);
      trio_quiet(s);
      snes_run_frame(s);
    }
  s->pad[0] = 0;
  trio_place(s, p.x, p.y, p.facing_right);
  for (int i = 0; i < 30; i++) {
    trio_camera(s, cx, cy);
    trio_quiet(s);
    snes_run_frame(s);
  }
}

bool trio_is_blade(const Snes *s) { return s->game == SNES_GAME_G3 && s->wram[0x0A8E] != 0; }

//---------------------------------------------------------------- shots

static int read_objs(const Snes *s, uint32_t first, uint32_t end, TrioShot *out, int max, int kind_or) {
  int n = 0;
  for (uint32_t a = first; a < end && n < max; a += TRIO_OBJ_SIZE) {
    if (!s->wram[a]) continue;
    TrioShot *o = &out[n++];
    o->slot = (int)a;
    o->x = rd16(s, a + 0x05);
    o->y = rd16(s, a + 0x08);
    o->kind = kind_or | s->wram[a + 0x0A];
    o->right = (s->wram[a + 0x11] & 0x40) != 0;
    o->moving = false;
    o->vx = o->vy = 0;
  }
  return n;
}

int trio_read_shots(const Snes *s, TrioShot *out, int max) {
  const TrioGame *g = G(s);
  return read_objs(s, g->shot_first, g->shot_end, out, max, 0);
}

int trio_read_enemy_shots(const Snes *s, TrioShot *out, int max) {
  const TrioGame *g = G(s);
  return read_objs(s, g->eshot_first, g->eshot_end, out, max, 0x80);
}

void trio_kill_shot(Snes *s, int slot) {
  //Far outside every level: the game's on-screen test removes it.
  wr16(s, (uint32_t)slot + 0x05, 0x7000);
  wr16(s, (uint32_t)slot + 0x08, 0x7000);
}

//---------------------------------------------------------------- camera

void trio_camera(Snes *s, int camx, int camy) {
  const TrioGame *g = G(s);
  if (camx < 0) camx = 0;
  if (camy < 0) camy = 0;
  for (int i = 0; i < 2; i++) {
    wr16(s, g->lim_xmin[i], (uint16_t)camx);
    wr16(s, g->lim_xmax[i], (uint16_t)camx);
  }
  for (int i = 0; i < g->nlim_y; i++) wr16(s, g->lim_y[i], (uint16_t)camy);
  wr16(s, g->cam_x, (uint16_t)camx);
  wr16(s, g->cam_y, (uint16_t)camy);
  wr16(s, g->scroll_h, (uint16_t)camx);
  wr16(s, g->scroll_v, (uint16_t)camy);
}

int trio_cam_x(const Snes *s) { return rd16(s, G(s)->cam_x); }
int trio_cam_y(const Snes *s) { return rd16(s, G(s)->cam_y); }

static void clear_table(Snes *s, uint32_t first, uint32_t end, uint32_t size) {
  for (uint32_t a = first; a < end; a += size) s->wram[a] = 0;
}

void trio_quiet_enemies(Snes *s) {
  const TrioGame *g = G(s);
  clear_table(s, g->enemy_first, g->enemy_end, TRIO_OBJ_SIZE);
  clear_table(s, g->eshot_first, g->eshot_end, TRIO_OBJ_SIZE);
}

void trio_quiet(Snes *s) {
  const TrioGame *g = G(s);
  trio_quiet_enemies(s);
  clear_table(s, g->item_first, g->item_end, g->item_size);
}

//---------------------------------------------------------------- items

//The games' own pickups (ids and subs are the same in all three): id 2 life
//(sub 0 large, 2 small), 1 weapon energy (0 large, 1 small), 4 1UP. Sub bit
//7 is the kind placed in a level: no pop-up, no timeout.
static const uint8_t item_ids[TRIO_ITEM_COUNT][2] = {
  [TRIO_ITEM_LIFE_S] = {2, 2}, [TRIO_ITEM_LIFE_L] = {2, 0},
  [TRIO_ITEM_WEAPON_S] = {1, 1}, [TRIO_ITEM_WEAPON_L] = {1, 0}, [TRIO_ITEM_1UP] = {4, 0},
};

bool trio_put_item(Snes *s, int slot, int kind, int x, int y) {
  const TrioGame *g = G(s);
  uint32_t a = g->item_first + (uint32_t)slot * g->item_size;
  if (kind <= TRIO_ITEM_NONE || kind >= TRIO_ITEM_COUNT || a + g->item_size > g->item_end) return false;
  uint8_t id = item_ids[kind][0], sub = (uint8_t)(item_ids[kind][1] | 0x80);
  bool fresh = !s->wram[a] || s->wram[a + 0x0A] != id || s->wram[a + 0x0B] != sub;
  if (fresh) {
    memset(s->wram + a, 0, g->item_size);
    s->wram[a] = 1;
    s->wram[a + 0x0A] = id;
    s->wram[a + 0x0B] = sub;
  }
  wr16(s, a + 0x05, (uint16_t)x);
  wr16(s, a + 0x08, (uint16_t)y);
  return fresh;
}

void trio_clear_item(Snes *s, int slot) {
  const TrioGame *g = G(s);
  uint32_t a = g->item_first + (uint32_t)slot * g->item_size;
  if (a + g->item_size <= g->item_end) s->wram[a] = 0;
}

//---------------------------------------------------------------- calls

#define SENTINEL_PC 0xFFF0  //bank $00; the routine's RTL lands here

bool trio_call(Snes *s, TrioCall call, uint16_t d, uint16_t a, uint16_t x, uint16_t y, int max_insns) {
  if (!call.pc) return false;
  Cpu saved = s->cpu;
  uint8_t scratch[0x20];
  memcpy(scratch, s->wram, sizeof(scratch));
  Cpu *c = &s->cpu;
  //JSL frame: PB, then (return - 1) high, low.
  uint16_t ret = SENTINEL_PC - 1;
  s->wram[c->s & 0x1FFF] = 0x00; c->s--;
  s->wram[c->s & 0x1FFF] = (uint8_t)(ret >> 8); c->s--;
  s->wram[c->s & 0x1FFF] = (uint8_t)ret; c->s--;
  c->e = false;
  c->pb = (uint8_t)(call.pc >> 16);
  c->pc = (uint16_t)call.pc;
  c->d = d;
  c->db = call.db;
  c->p = (uint8_t)(call.x16 ? 0x24 : 0x34);  //I, M; X per the call
  c->a = a;
  c->x = x;
  c->y = y;
  c->nmi_pending = false;
  c->irq_line = false;
  c->waiting = false;
  int i;
  for (i = 0; i < max_insns && !(c->pb == 0x00 && c->pc == SENTINEL_PC); i++) cpu_step(c);
  bool done = c->pb == 0x00 && c->pc == SENTINEL_PC;
  bool nmi = s->cpu.nmi_pending;
  s->cpu = saved;
  s->cpu.nmi_pending |= nmi;
  memcpy(s->wram, scratch, sizeof(scratch));
  return done;
}

void trio_damage(Snes *s, int dmg, int from_x, int from_y) {
  const TrioGame *g = G(s);
  //An enemy slot (enemies are switched off) as the stand-in attacker:
  //contact damage at +$26, position at +$05 / +$08.
  uint32_t att = g->enemy_end - TRIO_OBJ_SIZE;
  uint8_t keep[TRIO_OBJ_SIZE];
  memcpy(keep, s->wram + att, sizeof(keep));
  memset(s->wram + att, 0, TRIO_OBJ_SIZE);
  s->wram[att + 0x26] = (uint8_t)(dmg < 1 ? 1 : dmg > 127 ? 127 : dmg);
  wr16(s, att + 0x05, (uint16_t)from_x);
  wr16(s, att + 0x08, (uint16_t)from_y);
  trio_call(s, g->damage, (uint16_t)att, 0, (uint16_t)g->player, 0, 20000);
  memcpy(s->wram + att, keep, sizeof(keep));
}

//---------------------------------------------------------------- level

static uint32_t attr_table(const Snes *s) { return rd24(s, G(s)->attr_ptr); }

int trio_attr(const Snes *s, int x, int y) {
  const TrioGame *g = G(s);
  if (x < 0 || y < 0 || x >= 0x2000 || y >= 0x2000) return -1;
  uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
  if (!scr) return -1;
  uint16_t blk = rd16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3));
  return bus8(s, attr_table(s) + blk) & 0x3F;
}

static void put_block(uint8_t *map, int w, int h, int bx, int by, const uint16_t t[4]) {
  (void)w;
  for (int q = 0; q < 4; q++) {
    int tx = bx * 2 + (q & 1), ty = by * 2 + (q >> 1);
    uint8_t *e = map + ((size_t)tx * (size_t)h + (size_t)ty) * 2;
    e[0] = (uint8_t)t[q];
    e[1] = (uint8_t)(t[q] >> 8);
  }
}

static void block_tiles(const Snes *s, uint32_t tab, uint16_t blk, uint16_t out[4]) {
  uint32_t base = (tab & 0xFF0000) | (uint16_t)(tab + blk * 8u);
  for (int k = 0; k < 4; k++)
    out[k] = (uint16_t)(bus8(s, (base & 0xFF0000) | (uint16_t)(base + k * 2)) | bus8(s, (base & 0xFF0000) | (uint16_t)(base + k * 2 + 1)) << 8);
}

void trio_block_tiles(const Snes *s, uint16_t blk, uint16_t out[4]) {
  block_tiles(s, rd24(s, G(s)->tiles_ptr), blk, out);
}

void trio_put_block(Snes *s, int x, int y, uint16_t blk) {
  const TrioGame *g = G(s);
  if (x < 0 || y < 0 || x >= 0x2000 || y >= 0x2000) return;
  uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
  if (!scr) return;
  wr16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3), blk);
}

void trio_build_bg1_map(const Snes *s, uint8_t *map, int w, int h) {
  const TrioGame *g = G(s);
  memset(map, 0, (size_t)w * (size_t)h * 2);
  uint32_t tab = rd24(s, g->tiles_ptr);
  for (int by = 0; by < h / 2 && by < 512; by++)
    for (int bx = 0; bx < w / 2 && bx < 512; bx++) {
      int x = bx * 16, y = by * 16;
      uint8_t scr = s->wram[g->screens + (((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F)];
      if (!scr) continue;
      uint16_t blk = rd16(s, g->blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3));
      uint16_t t[4];
      block_tiles(s, tab, blk, t);
      put_block(map, w, h, bx, by, t);
    }
}

//BG2: G1 and G2 keep its maps at fixed places; G3 has pointers ($1FF1 screen
//map, $1FEC block map base, $1FEE / $1FF0 block tiles).
static void bg2_maps(const Snes *s, uint32_t *screens, uint32_t *blocks, uint32_t *tiles) {
  const TrioGame *g = G(s);
  if (g->id == SNES_GAME_G3) {
    *screens = rd16(s, 0x1FF1);
    *blocks = rd16(s, 0x1FEC);
    *tiles = (uint32_t)s->wram[0x1FF0] << 16 | rd16(s, 0x1FEE);
  } else {
    *screens = g->bg2_screens;
    *blocks = g->bg2_blocks;
    *tiles = rd24(s, g->bg2_tiles_ptr);
  }
}

void trio_build_bg2_map(const Snes *s, uint8_t *map, int w, int h) {
  uint32_t screens, blocks, tab;
  bg2_maps(s, &screens, &blocks, &tab);
  memset(map, 0, (size_t)w * (size_t)h * 2);
  for (int by = 0; by < h / 2 && by < 512; by++)
    for (int bx = 0; bx < w / 2 && bx < 512; bx++) {
      int x = bx * 16, y = by * 16;
      uint8_t scr = s->wram[(screens + (uint32_t)((((y >> 8) & 0x1F) << 5) + ((x >> 8) & 0x1F))) & 0xFFFF];
      if (!scr) continue;
      uint16_t blk = rd16(s, (blocks + scr * 0x200u + (uint32_t)((y & 0xF0) << 1) + (uint32_t)((x & 0xF0) >> 3)) & 0xFFFF);
      uint16_t t[4];
      block_tiles(s, tab, blk, t);
      put_block(map, w, h, bx, by, t);
    }
}

void trio_bg2_camera(const Snes *s, int *x, int *y) {
  const TrioGame *g = G(s);
  *x = rd16(s, g->bg2_cam_x);
  *y = rd16(s, g->bg2_cam_y);
}

//---------------------------------------------------------------- geometry

void trio_geo_read(const Snes *s, TrioGeo *g, int x0, int y0, int x1, int y1) {
  g->x0 = x0 & ~0xFF;
  g->y0 = y0 & ~0xFF;
  g->sw = ((x1 + 255) >> 8) - (g->x0 >> 8);
  g->sh = ((y1 + 255) >> 8) - (g->y0 >> 8);
  g->attr = calloc((size_t)g->sw * g->sh * 256, 1);
  for (int by = 0; by < g->sh * 16; by++)
    for (int bx = 0; bx < g->sw * 16; bx++) {
      int a = trio_attr(s, g->x0 + bx * 16, g->y0 + by * 16);
      g->attr[by * g->sw * 16 + bx] = (uint8_t)(a < 0 ? 0 : a);
    }
}

void trio_geo_free(TrioGeo *g) {
  free(g->attr);
  g->attr = NULL;
}

uint8_t trio_geo_at(const TrioGeo *g, int x, int y) {
  int bx = (x - g->x0) >> 4, by = (y - g->y0) >> 4;
  if (x < g->x0 || y < g->y0 || bx >= g->sw * 16 || by >= g->sh * 16) return 0;
  return g->attr[by * g->sw * 16 + bx];
}

void trio_geo_set(TrioGeo *g, int x, int y, uint8_t a) {
  int bx = (x - g->x0) >> 4, by = (y - g->y0) >> 4;
  if (x < g->x0 || y < g->y0 || bx >= g->sw * 16 || by >= g->sh * 16) return;
  g->attr[by * g->sw * 16 + bx] = a;
}

//The BG1 block map runs up to BG2's (G1, G2: $A600; G3: its pointer).
static int max_screens(const Snes *s) {
  const TrioGame *g = G(s);
  uint32_t screens, blocks, tiles;
  bg2_maps(s, &screens, &blocks, &tiles);
  uint32_t end = blocks > g->blocks ? blocks : g->screens;
  if (end > g->screens) end = g->screens;
  return (int)((end - g->blocks) / 0x200);
}

bool trio_geo_install(Snes *s, const TrioGeo *geo) {
  const TrioGame *g = G(s);
  int max = max_screens(s);
  int need = geo->sw * geo->sh + 1;   //+ the identity table
  if (need >= max) return false;
  //Identity table in the last screen of the block map; block n has attribute n.
  uint32_t tab = g->blocks + (uint32_t)(max - 1) * 0x200;
  for (int i = 0; i < 0x40; i++) s->wram[tab + i] = (uint8_t)i;
  wr16(s, g->attr_ptr, (uint16_t)tab);
  s->wram[g->attr_ptr + 2] = 0x7E;
  memset(s->wram + g->screens, 0, 0x400);
  int id = 1;
  for (int sy = 0; sy < geo->sh; sy++)
    for (int sx = 0; sx < geo->sw; sx++, id++) {
      int X = (geo->x0 >> 8) + sx, Y = (geo->y0 >> 8) + sy;
      if (X > 31 || Y > 31) continue;
      s->wram[g->screens + Y * 32 + X] = (uint8_t)id;
      for (int by = 0; by < 16; by++)
        for (int bx = 0; bx < 16; bx++) {
          uint8_t a = geo->attr[(sy * 16 + by) * geo->sw * 16 + sx * 16 + bx];
          wr16(s, g->blocks + (uint32_t)id * 0x200 + (uint32_t)(by * 32 + bx * 2), a);
        }
    }
  return true;
}

//---------------------------------------------------------------- weapons

//Energy byte of special weapon w: bit 7 HUD refresh, bit 6 owned, bits 0-5
//energy (G1 and G2 keep a fraction in the byte below it).
static uint8_t *energy(Snes *s, int w) { return &s->wram[G(s)->weapon_energy + 2u * (unsigned)(w - 1)]; }

int trio_weapon(const Snes *s) { return (s->wram[G(s)->player + 0x33] >> 1) % G(s)->nweapons; }

void trio_set_weapon(Snes *s, int w) { s->wram[G(s)->player + 0x33] = (uint8_t)(2 * w); }

int trio_weapon_energy(const Snes *s, int w) {
  if (w <= 0 || w >= G(s)->nweapons) return G(s)->weapon_full;
  return s->wram[G(s)->weapon_energy + 2u * (unsigned)(w - 1)] & 0x3F;
}

void trio_set_weapon_energy(Snes *s, int w, int e) {
  if (w <= 0 || w >= G(s)->nweapons) return;
  uint8_t *b = energy(s, w);
  int full = G(s)->weapon_full;
  *b = (uint8_t)((*b & 0x40) | 0x80 | (e < 0 ? 0 : e > full ? full : e));
}

void trio_unlock_weapons(Snes *s) {
  for (int w = 1; w < G(s)->nweapons; w++) *energy(s, w) = (uint8_t)(0xC0 | G(s)->weapon_full);
}

void trio_refill_weapons(Snes *s, int amount) {
  for (int w = 1; w < G(s)->nweapons; w++) {
    uint8_t *e = energy(s, w);
    if (!(*e & 0x40)) continue;
    int v = (*e & 0x3F) + amount, full = G(s)->weapon_full;
    if (v > full) v = full;
    *e = (uint8_t)((*e & 0xC0) | 0x80 | v);
  }
}

//---------------------------------------------------------------- sound

int trio_sound_take(const Snes *s, uint8_t *seen, TrioSnd *out, int max) {
  const TrioGame *g = G(s);
  uint8_t w = s->wram[g->snd_w] & 0x1E;
  int n = 0;
  while (*seen != w && n < max) {
    out[n].id = s->wram[g->snd_queue + *seen];
    out[n].param = s->wram[g->snd_queue + *seen + 1];
    n++;
    *seen = (uint8_t)((*seen + 2) & 0x1E);
  }
  *seen = w;
  return n;
}

void trio_sound_push(Snes *s, uint8_t id, uint8_t param) {
  const TrioGame *g = G(s);
  uint8_t w = s->wram[g->snd_w] & 0x1E;
  s->wram[g->snd_queue + w] = id;
  s->wram[g->snd_queue + w + 1] = param;
  s->wram[g->snd_w] = (uint8_t)((w + 2) & 0x1E);
}
