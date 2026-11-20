#include "arena.h"
#include "sprites.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEATH_FRAMES 120        //the game's death animation plays before the respawn
#define SPAWN_PROTECT 120
#define COUNTDOWN 150
#define WARP_HEIGHT 120         //respawns beam down from this far above the floor
#define KO_CREDIT 300           //a fall counts for the last hitter within this many frames
#define ENERGY_REGEN 120        //frames per unit of weapon energy regained
#define ITEM_EVERY 600          //frames between items appearing
#define SP_COOLDOWN 180         //frames between a fighter's specials (Fireball, Uppercut)

//---------------------------------------------------------------- shots

//What each shot kind (+$0A) does in battle, per game. Damage is in
//sixteenths of a life. still: counts while not moving (shields, orbiting
//and drilling weapons); pierce: keeps going after a hit; shield: stops the
//other fighters' shots and does no damage.
ShotInfo arena_shot_info(int game, int kind) {
  ShotInfo i = {-1, false, false, false, false, true};
  if (kind & 0x80) { i.dmg = 3; return i; }              //a boss's projectile
  switch (kind) {
  case 0x00: i.dmg = 1; return i;                        //uncharged shot
  case 0x01: i.dmg = 3; return i;                        //half charge
  case 0x02: i.dmg = 6; i.pierce = i.big = true; return i;   //G1 arm part's third level
  case 0x03: i.dmg = 5; i.pierce = i.big = true; return i;   //full charge
  }
  switch (game) {
  case SNES_GAME_G1:
    switch (kind) {
    case 0x04: i.dmg = 6; i.pierce = i.big = true; break;    //Fireball (the game's kills in one hit)
    case 0x07: i.dmg = 2; break;                             //weapon
    case 0x08: i.dmg = 1; break;                             //weapon
    case 0x09: i.dmg = 2; break;                             //weapon
    case 0x0A: i.dmg = 1; i.still = true; break;             //weapon
    case 0x0B: i.dmg = 2; i.pierce = i.still = true; break;  //weapon
    case 0x0C: i.dmg = 2; break;                             //weapon
    case 0x0D: i.dmg = 2; i.pierce = true; break;            //weapon
    case 0x0E: i.dmg = 2; break;                             //weapon
    }
    break;
  case SNES_GAME_G2:
    switch (kind) {
    case 0x07: i.dmg = 1; break;                             //weapon
    case 0x08: i.dmg = 1; break;                             //weapon
    case 0x1E: i.dmg = 2; break;                             //weapon debris
    case 0x0A: i.dmg = 2; i.pierce = true; break;            //weapon
    case 0x0B: i.dmg = 2; i.pierce = true; break;            //weapon
    case 0x0C: i.dmg = 2; i.still = true; break;             //weapon
    case 0x0D: i.dmg = 2; break;                             //weapon
    case 0x0E: i.dmg = 2; i.pierce = true; break;            //weapon
    case 0x0F: i.dmg = 6; i.pierce = i.big = i.still = true; break;  //weapon
    case 0x11: i.shield = true; i.still = true; i.dmg = 0; break;    //charged weapon: bubbles around the hero
    case 0x1D: i.dmg = 4; break;                             //the arm part's double charge (two of them)
    case 0x04: i.dmg = 6; i.pierce = i.still = i.big = true; break;   //Uppercut (the flame)
    default: if (kind >= 0x10 && kind <= 0x17) i.dmg = 3; break;     //charged weapons
    }
    break;
  case SNES_GAME_G3:
    switch (kind) {
    case 0x07: i.dmg = 2; break;                             //weapon
    case 0x18: i.dmg = 3; break;                             //charged weapon
    case 0x08: i.dmg = 2; break;                             //weapon
    case 0x09: i.dmg = 2; i.still = true; break;             //weapon
    case 0x1B: i.dmg = 2; i.still = i.pierce = true; break;  //charged weapon
    case 0x0A: i.dmg = 2; i.pierce = true; break;            //weapon
    case 0x0B: i.dmg = 0; break;                             //weapon's muzzle
    case 0x1C: i.dmg = 1; break;                             //weapon pellets
    case 0x0C: i.dmg = 2; break;                             //weapon
    case 0x0D: i.shield = true; i.still = true; i.dmg = 0; break;    //weapon
    case 0x0E: i.dmg = 2; i.still = i.pierce = true; break;  //weapon
    case 0x1D: i.dmg = 3; i.pierce = true; break;
    }
    break;
  }
  if (i.dmg < 0) { i.dmg = 2; i.known = false; }
  return i;
}

void arena_shotbox(int game, const TrioShot *s, int *x0, int *y0, int *x1, int *y1) {
  int hw = 8, hh = 8;
  ShotInfo in = arena_shot_info(game, s->kind);
  switch (s->kind) {
  case 0x00: hw = 4; hh = 3; break;
  case 0x01: hw = 8; hh = 6; break;
  case 0x02: case 0x03: hw = 12; hh = 12; break;
  }
  if (game == SNES_GAME_G3 && s->kind == 0x1C) { hw = 3; hh = 3; }
  if (game == SNES_GAME_G1 && s->kind == 0x04) { hw = 12; hh = 12; }   //Fireball
  if (game == SNES_GAME_G2 && s->kind == 0x04) { hw = 14; hh = 26; }   //Uppercut: the flame around the hero
  if (in.shield) { hw = 14; hh = 18; }
  if (game == SNES_GAME_G2 && s->kind == 0x0F) { hw = 120; hh = 100; }   //weapon fills the screen
  *x0 = s->x - hw;
  *x1 = s->x + hw;
  *y0 = s->y - hh;
  *y1 = s->y + hh;
}

void arena_hurtbox(const ArenaFighter *f, int *x0, int *y0, int *x1, int *y1) {
  *x0 = f->p.x - 7;
  *x1 = f->p.x + 7;
  *y0 = f->p.y - 14;
  *y1 = f->p.y + 17;
  if (f->kind == FK_BOSS) { *x0 -= 6; *x1 += 6; *y0 -= 12; }
  if (f->kind == FK_BLADE) *y0 -= TRIO_BLADE_RISE;
}

void arena_swordbox(const TrioPlayer *p, int *x0, int *y0, int *x1, int *y1) {
  if (p->action == TRIO_ACT_SWORD_AIR) {
    //The jump slash sweeps over his head and in front.
    *x0 = p->facing_right ? p->x - 16 : p->x - 40;
    *x1 = p->facing_right ? p->x + 40 : p->x + 16;
    *y0 = p->y - 40;
    *y1 = p->y + 12;
    return;
  }
  *x0 = p->facing_right ? p->x - 4 : p->x - 40;
  *x1 = p->facing_right ? p->x + 40 : p->x + 4;
  *y0 = p->y - 34;
  *y1 = p->y + 17;
}
#define SWORD_DAMAGE 6
//Frames into the slash the blade is out: on the ground, in the air.
#define SWORD_FROM 3
#define SWORD_TO 9
#define SWORD_AIR_FROM 5
#define SWORD_AIR_TO 12
#define SWORD_CUT 10             //the ground slash ends here

//---------------------------------------------------------------- helpers

static uint32_t rnd(Arena *a) {
  uint32_t x = a->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return a->rng = x ? x : 1;
}

static void add_fx(Arena *a, int kind, int x, int y) {
  if (a->nfx < (int)(sizeof(a->fx) / sizeof(a->fx[0]))) a->fx[a->nfx++] = (ArenaFx){kind, x, y};
}

static int game_of(const ArenaFighter *f) { return f->s ? f->s->game : f->base->game; }

static void read_fighter(ArenaFighter *f) {
  if (f->kind == FK_BOSS) {
    trio_read_boss(f->s, &f->p);
    //Standing is the controller's (between attacks).
    f->p.ground = f->ctl.holding && f->ctl.ground;
  } else {
    trio_read_player(f->s, &f->p);
  }
}

//At the start P1 and P2 face each other, and P3 and P4 (the nearest other
//fighter when the partner is out); a respawn faces the nearest.
static bool faces_right(const Arena *a, int i, int x) {
  int best = 1 << 30, tx = (a->st->def->x0 + a->st->def->x1) / 2;
  const ArenaFighter *me = &a->f[i], *mate = &a->f[i ^ 1];
  if (me->first_spawn && mate->on && a->st->spawn_x[i ^ 1] != x) return a->st->spawn_x[i ^ 1] > x;
  for (int k = 0; k < ARENA_MAX; k++) {
    const ArenaFighter *o = &a->f[k];
    if (k == i || !o->on || (o->s && o->stocks <= 0 && o->dead)) continue;
    int ox = o->first_spawn ? a->st->spawn_x[k] : o->p.x;
    if (abs(ox - x) < best && ox != x) { best = abs(ox - x); tx = ox; }
  }
  return tx > x;
}

//Back into his own game's copy (a respawn starts there with the blaster).
static void unhop(ArenaFighter *f) {
  if (f->kind != FK_X || f->cur_game == f->own_game || !f->helper[f->own_game]) return;
  Snes *cur = f->s;
  f->s = f->helper[f->own_game];
  f->helper[f->own_game] = NULL;
  f->helper[f->cur_game] = cur;
  f->cur_game = f->own_game;
}

static void spawn(Arena *a, int i, bool first) {
  ArenaFighter *f = &a->f[i];
  const ArenaStage *st = a->st;
  unhop(f);
  f->wi = 0;
  f->sw_inject = 0;
  if (f->kind == FK_X) {
    f->max_hp = trio_max_hp(f->base);
    for (int g = 1; g <= 3; g++)
      if (f->helper[g] && f->helper_init[g]) trio_refill_weapons(f->helper[g], 64);
  }
  snes_clone(f->s, f->base);
  f->s->no_apu = true;
  int x = st->spawn_x[i], feet = st->spawn_y[i];
  if (a->force_spawn >= 0) {
    //Brought back into the fight (stalemate): this spawn point.
    x = st->spawn_x[a->force_spawn];
    feet = st->spawn_y[a->force_spawn];
  } else if (!first) {
    //A respawn lands on the spawn point farthest from the fighters still in.
    int best = -1;
    for (int k = 0; k < 4; k++) {
      int near = 1 << 30;
      for (int o = 0; o < ARENA_MAX; o++) {
        const ArenaFighter *of = &a->f[o];
        if (o == i || !of->on || of->dead || of->stocks <= 0) continue;
        int dx = of->p.x - st->spawn_x[k], dy = (of->p.y + 17 - st->spawn_y[k]) * 2;
        int d = dx * dx + dy * dy;
        if (d < near) near = d;
      }
      if (near > best) { best = near; x = st->spawn_x[k]; feet = st->spawn_y[k]; }
    }
  }
  bool right = faces_right(a, i, x);
  f->ctl = (TrioBossCtl){0};
  f->pend_move = f->pend_t = 0;
  f->sword_t = -1;
  f->sp_move = ARENA_SP_NONE;
  f->sp_cd = 0;
  f->have_old = false;
  memset(f->mot, 0, sizeof(f->mot));
  for (int g = 0; g < 4; g++) { f->helper_live[g] = false; f->nhshots[g] = 0; }
  if (f->kind == FK_BOSS) {
    trio_boss_place(f->s, x, feet, right);
    f->ctl.face_right = right;
  } else {
    //Beam down onto the spawn point, the game's own way.
    //From up to WARP_HEIGHT above the feet, but never from inside or over
    //a ceiling or a one-way platform (he would land on that): the beam
    //starts below the first such block above.
    int y = feet - 17;
    while (y > feet - 17 - WARP_HEIGHT && y - 24 > st->def->y0) {
      int at = trio_geo_at(&st->geo, x, y - 24);
      if (trio_at_solid(at) || at == TRIO_AT_LADDER_TOP) break;
      y -= 4;
    }
    trio_place(f->s, x, y, right);
    trio_set_hp(f->s, f->max_hp);
    trio_warp_in(f->s);
    stage_follow(st, f->s);
  }
  f->snd_seen = f->s->wram[trio_game(f->s)->snd_w] & 0x1E;
  read_fighter(f);
  f->cam_x = f->oam_cam_x = trio_cam_x(f->s);
  f->cam_y = f->oam_cam_y = trio_cam_y(f->s);
  f->prev = f->p;
  f->first_spawn = false;
  f->dead = false;
  f->dead_t = 0;
  f->spawn_t = first ? 0 : SPAWN_PROTECT;
  f->last_hitter = -1;
  f->nshots = 0;
  f->prev_pad = 0xFFFF;
}

void arena_init(Arena *a, const ArenaStage *st, ArenaSound *snd, const ArenaSlot slots[ARENA_MAX], ArenaRules rules) {
  memset(a, 0, sizeof(*a));
  a->st = st;
  a->snd = snd;
  a->rules = rules;
  if (a->rules.stocks <= 0) a->rules.stocks = 3;
  a->winner = -1;
  a->force_spawn = -1;
  a->wind_dir = 1;
  a->rng = 0x9E3779B9u;
  for (int i = 0; i < ARENA_MAX; i++) {
    ArenaFighter *f = &a->f[i];
    if (!slots[i].on || !slots[i].base) continue;
    f->on = true;
    f->cpu = slots[i].cpu;
    f->level = slots[i].level;
    f->base = slots[i].base;
    f->boss = trio_boss_kind(f->base);
    f->kind = f->boss ? FK_BOSS : trio_is_blade(f->base) ? FK_BLADE : FK_X;
    f->max_hp = f->kind == FK_BOSS ? 16 : trio_max_hp(f->base);
    f->specials = slots[i].specials && f->kind == FK_X;
    f->stocks = a->rules.stocks;
    f->s = malloc(sizeof(Snes));
    f->own_game = f->cur_game = f->base->game;
    for (int g = 1; g <= 3; g++) f->alt_base[g] = g == f->own_game ? f->base : slots[i].alt[g];
    //His blaster, then the specials of his game or of the games in the pool.
    f->nwl = 0;
    f->wl[f->nwl].game = (uint8_t)f->own_game;
    f->wl[f->nwl++].w = 0;
    for (int g = 1; g <= 3 && f->kind == FK_X && a->rules.weapons; g++) {
      bool in = slots[i].pool ? (slots[i].pool >> (g - 1) & 1) && f->alt_base[g] : g == f->own_game;
      for (int w = 1; in && w < trio_games[g].nweapons && f->nwl < 32; w++) {
        f->wl[f->nwl].game = (uint8_t)g;
        f->wl[f->nwl++].w = (uint8_t)w;
      }
    }
    f->first_spawn = true;
  }
  //Platforms start at opposite ends of their lanes.
  for (int k = 0; k < st->nlane && k < 2; k++) {
    ArenaMover *m = &a->mover[a->nmover++];
    m->lane = st->lane[k];
    m->dir = k & 1 ? -1 : 1;
    m->px = k & 1 ? m->lane.x1 : m->lane.x0;
    m->x = m->px << 8;
    m->col = (m->px - m->lane.x0 + 8) >> 4;
  }
  for (int i = 0; i < ARENA_MAX; i++)
    if (a->f[i].on) spawn(a, i, true);
  a->phase = AP_COUNTDOWN;
}

void arena_set_helper(Arena *a, int game, Snes *base) { if (game >= 1 && game <= 3) a->helper_base[game] = base; }

void arena_free(Arena *a) {
  for (int i = 0; i < ARENA_MAX; i++) {
    free(a->f[i].s);
    free(a->f[i].old_ppu);
    for (int g = 0; g < 4; g++) free(a->f[i].helper[g]);
  }
  memset(a, 0, sizeof(*a));
}

int arena_alive(const Arena *a) {
  int n = 0;
  for (int i = 0; i < ARENA_MAX; i++)
    if (a->f[i].on && a->f[i].stocks > 0) n++;
  return n;
}

static bool vulnerable(const ArenaFighter *f) {
  if (!f->on || f->dead || f->stocks <= 0 || f->spawn_t > 0) return false;
  if (f->sp_move == ARENA_SP_UPPERCUT) return false;   //invincible in it, as in G2
  if (f->kind == FK_BOSS) return trio_boss_vulnerable(f->s);
  return f->p.hp > 0 && f->p.action != TRIO_ACT_HURT && f->p.action != TRIO_ACT_DYING &&
         f->p.action != TRIO_ACT_WARP && f->p.iframes == 0;
}

//Lives are even: a hit costs dmg sixteenths of the fighter's life whatever
//its max HP (life upgrades) and armor (the body part cuts damage, not by a
//fixed ratio). The damage handed to the game is the smallest that costs
//that much, found on a scratch copy.
static int even_damage(const ArenaFighter *t, int dmg) {
  int want = (dmg * t->max_hp + 15) / 16;
  const TrioGame *g = trio_game(t->s);
  if (t->max_hp <= 16 && !(t->s->wram[g->parts] & 0x04)) return want;   //no body part
  static Snes tmp;
  int lo = 1, hi = 127, best = 127;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    snes_clone(&tmp, t->s);
    TrioPlayer p;
    trio_read_player(&tmp, &p);
    int before = p.hp;
    trio_damage(&tmp, mid, t->p.x - 16, t->p.y);
    trio_read_player(&tmp, &p);
    if (before - p.hp >= want || p.hp == 0) { best = mid; hi = mid - 1; }
    else lo = mid + 1;
  }
  return best;
}

static void hit(Arena *a, int ti, int ai, int dmg, int from_x) {
  ArenaFighter *t = &a->f[ti], *atk = &a->f[ai];
  if (t->kind == FK_BOSS) trio_boss_damage(t->s, dmg);
  else trio_damage(t->s, even_damage(t, dmg), from_x, t->p.y);
  read_fighter(t);
  t->last_hitter = ai;
  t->last_hit_t = a->frame;
  a->last_any_hit = a->frame;
  t->hit_flash = 8;
  atk->dealt += dmg;
}

//Boss control: Left/Right face and walk, B jumps (the controller in
//boss.c), X / Y / A start its attacks. An attack starts when the boss is
//back in its waiting pose on the ground; one pressed a little early is kept
//until then.
#define BOSS_BUFFER 20
static int boss_move(ArenaFighter *f, uint16_t pad) {
  if (pad & PAD_LEFT) f->ctl.face_right = false;
  if (pad & PAD_RIGHT) f->ctl.face_right = true;
  f->ctl.walk = pad & PAD_LEFT ? -1 : pad & PAD_RIGHT ? 1 : 0;
  //B jumps and Down + B drops through a platform (the controller, boss.c);
  //X, Y and A start the boss's own attacks.
  f->ctl.pad = pad;
  int m = pad & PAD_X ? TRIO_MOVE_JUMP : pad & PAD_A ? TRIO_MOVE_SPECIAL : pad & PAD_Y ? TRIO_MOVE_SHOT : TRIO_MOVE_NONE;
  if (m != TRIO_MOVE_NONE) { f->pend_move = m; f->pend_t = BOSS_BUFFER; }
  else if (f->pend_t > 0 && --f->pend_t == 0) f->pend_move = TRIO_MOVE_NONE;
  return f->pend_move;
}

//---------------------------------------------------------------- ghosts

//The games' shot tables hold 8 or 9 objects, and a weapon's tiles replace
//the last weapon's in the copy. A shot that has to leave its copy (to make
//room, or because its weapon is being put away) is taken over by the arena:
//it flies on in a straight line at the speed it had, drawn from a picture
//of it taken now, and hits like any shot. Shields, weapons held at the hero,
//shots at rest and kinds not listed in arena_shot_info (glows, sparks)
//stay the copy's. False if it was not taken.
static bool ghost_take(Arena *a, const ArenaFighter *f, const Snes *src, int game, const TrioShot *sh) {
  ShotInfo si = arena_shot_info(game, sh->kind);
  if (!si.known || si.dmg <= 0 || si.still || si.shield || !sh->moving || (!sh->vx && !sh->vy)) return false;
  //A step this long is the copy moving the object, not its flight.
  if (abs(sh->vx) > 24 || abs(sh->vy) > 24) return false;
  ArenaGhost *g = NULL;
  for (int k = 0; k < GHOST_MAX && !g; k++)
    if (!a->ghost[k].on) g = &a->ghost[k];
  if (!g) return false;
  static ArenaSprites sp;
  spr_collect(src, &sp);
  //A shot of the weapon he has put away: its tiles are in the snapshot.
  const Ppu *ppu = &src->ppu;
  int idx = (sh->slot - (int)trio_game(src)->shot_first) / TRIO_OBJ_SIZE;
  if (src == f->s && f->have_old && idx >= 0 && idx < 32 && f->shot_weapon[idx] != trio_weapon(src)) ppu = f->old_ppu;
  for (int y = 0; y < GHOST_SIZE; y++)
    for (int x = 0; x < GHOST_SIZE; x++) g->pic[y][x] = GHOST_CLEAR;
  int n = 0;
  for (int i = sp.n - 1; i >= 0; i--) {
    const ArenaSpr *p = &sp.s[i];
    if (abs(p->ox - sh->x) > 2 || abs(p->oy - sh->y) > 2) continue;
    ppu_draw_sprite(ppu, p->x - sh->x + GHOST_SIZE / 2, p->y - sh->y + GHOST_SIZE / 2, p->tile, p->attr, p->big, &g->pic[0][0], GHOST_SIZE, GHOST_SIZE, GHOST_SIZE);
    n++;
  }
  if (!n) return false;
  g->on = true;
  g->owner = (int)(f - a->f);
  g->game = game;
  g->kind = sh->kind;
  g->right = sh->right;
  g->x = sh->x;
  g->y = sh->y;
  g->vx = sh->vx;
  g->vy = sh->vy;
  g->t = 0;
  return true;
}

//---------------------------------------------------------------- frame

//Input of a fighter copy for this frame: weapon switching and Blade's sword
//button are the arena's, the rest goes to the game.
static void hop(Arena *a, ArenaFighter *f, int g);

//A one-way platform: a ladder's top with no ladder under it (the added
//platforms, moving or not).
static bool oneway_at(const Snes *s, int x, int y) {
  return trio_attr(s, x, y) == TRIO_AT_LADDER_TOP && trio_attr(s, x, y + 16) != TRIO_AT_LADDER;
}

//The games climb down a ladder's top on Down, which the specials' motions
//start with: while the hero stands on a one-way platform it is solid in his copy
//for the frame (firm), then put back (soften).
typedef struct Firm { int n, x[3], y; } Firm;
static void firm(Snes *s, int x, int feet, Firm *fm) {
  fm->n = 0;
  fm->y = feet;
  for (int dx = -16; dx <= 16; dx += 16)
    if (oneway_at(s, x + dx, feet)) {
      fm->x[fm->n++] = x + dx;
      trio_put_block(s, x + dx, feet, TRIO_AT_SOLID);
    }
}
static void soften(Snes *s, const Firm *fm) {
  for (int k = 0; k < fm->n; k++) trio_put_block(s, fm->x[k], fm->y, TRIO_AT_LADDER_TOP);
}

//L / R go through the fighter's weapon list. A weapon of another game moves
//him into that game's copy first. In the copy the weapon before it is put
//up and the game's own switch (one R) takes it out, with its colors and
//meter.
//Returns true when the game's own L / R does it (the next weapon of the
//same game): the press goes through.
static bool switch_weapon(Arena *a, ArenaFighter *f, int d) {
  int wi = ((f->wi + d) % f->nwl + f->nwl) % f->nwl;
  int g = f->wl[wi].game, w = f->wl[wi].w;
  int nw = trio_game(f->s)->nweapons, cw = trio_weapon(f->s);
  //Another weapon of the same game: its tiles and colors replace this
  //one's in the copy. The copy's picture chip is kept as it is now
  //(old_ppu) and this weapon's shots go on in the copy, drawn from that.
  //What it replaces: shots of the weapon before this one, which leave the
  //copy (ghost_take) or end; and a weapon held at the hero or a shield ends with
  //the switch (its object sits on the hero, whose own tiles are the copy's).
  if (g == f->cur_game) {
    const TrioGame *gm = trio_game(f->s);
    for (int k = 0; k < f->nshots; k++) {
      TrioShot *sh = &f->shots[k];
      int idx = (sh->slot - (int)gm->shot_first) / TRIO_OBJ_SIZE;
      if (sh->x < -5000 || idx < 0 || idx >= 32) continue;
      ShotInfo si = arena_shot_info(g, sh->kind);
      bool held = si.known && (si.still || si.shield);
      if (f->shot_weapon[idx] == cw && !held) continue;
      if (f->shot_weapon[idx] == cw && !cw) continue;   //the blaster's own (its glow)
      ghost_take(a, f, f->s, g, sh);
      trio_kill_shot(f->s, sh->slot);
      sh->x = -10000;
    }
    if (!f->old_ppu) f->old_ppu = malloc(sizeof(Ppu));
    if (f->old_ppu) { *f->old_ppu = f->s->ppu; f->have_old = true; }
  }
  if (g == f->cur_game && w == ((cw + d) % nw + nw) % nw) { f->wi = wi; return true; }
  if (g != f->cur_game) hop(a, f, g);
  if (g != f->cur_game) return false;
  nw = trio_game(f->s)->nweapons;
  trio_set_weapon(f->s, (w - 1 + nw) % nw);
  f->sw_inject = PAD_R;
  f->wi = wi;
  return false;
}

static uint16_t game_pad(Arena *a, ArenaFighter *f, uint16_t pad) {
  uint16_t pressed = pad & (uint16_t)~f->prev_pad;
  if (!a->rules.weapons) pad &= (uint16_t)~(PAD_L | PAD_R);
  if (f->kind == FK_X && a->rules.weapons) {
    uint16_t lr = pressed & (PAD_L | PAD_R);
    pad &= (uint16_t)~(PAD_L | PAD_R);
    if (f->sw_inject) { pad |= f->sw_inject; f->sw_inject = 0; }
    else if (lr && f->nwl > 1) {
      int d = lr & PAD_R ? 1 : -1;
      if (switch_weapon(a, f, d)) pad |= d > 0 ? PAD_R : PAD_L;
    }
  }
  //One-way platforms: Down + B drops through, as in platform fighters: the hero is put just
  //under it, falling (the games would hang him on it as on a ladder).
  if (f->p.ground && (pad & PAD_DOWN) && (pressed & PAD_B) && oneway_at(f->s, f->p.x, f->p.y + 17)) {
    const TrioGame *gm = trio_game(f->s);
    trio_place(f->s, f->p.x, f->p.y + 17, f->p.facing_right);
    f->s->wram[gm->player + 0x02] = TRIO_ACT_FALL;
    f->s->wram[gm->player + 0x03] = 0;
    f->drop_t = 10;
  }
  if (f->drop_t > 0) {
    f->drop_t--;
    pad &= (uint16_t)~(PAD_B | PAD_DOWN);
  }
  const TrioGame *g = trio_game(f->s);
  //(The games' wait for the hero's shots to be gone before a switch is skipped
  //in battle mode: ovr_*SwitchShots, src/game/battle_hooks.c.)
  if (f->kind == FK_BLADE) {
    //X: the sword at once. The games' slashes ($4C on the ground, $4E in
    //the air) come after the charged charged blaster and end with a pellet; here
    //the button starts the slash itself and the pellet is left out
    //(run_fighter). The ground slash is cut short once the blade is back.
    uint8_t *act = &f->s->wram[g->player + 0x02];
    int a0 = f->p.action;
    bool can = a0 == TRIO_ACT_IDLE || a0 == TRIO_ACT_LAND || a0 == TRIO_ACT_RUN || a0 == TRIO_ACT_DASH ||
                a0 == TRIO_ACT_JUMP || a0 == TRIO_ACT_FALL || a0 == TRIO_ACT_WALLKICK;
    if ((pressed & PAD_X) && f->sword_t < 0 && can) {
      act[0] = f->p.ground ? TRIO_ACT_SWORD : TRIO_ACT_SWORD_AIR;
      act[1] = 0;
    } else if (a0 == TRIO_ACT_SWORD && f->sword_t >= SWORD_CUT) {
      act[0] = TRIO_ACT_IDLE;
      act[1] = 0;
    }
  }
  return pad;
}

//The games allow only so many of the hero's shots out at once (the count at
//player +$35 against the blaster's 3 and each weapon's own limit), and the
//switch to another weapon waits on it too. Shots fly the whole arena here,
//so they would hold that count for seconds: only the current weapon's shots
//within about a screen of the fighter count. The others keep flying and
//hitting.
#define NEAR_X 224
#define NEAR_Y 160
static void near_shots(Arena *a, ArenaFighter *f) {
  const TrioGame *g = trio_game(f->s);
  uint8_t *n = &f->s->wram[g->player + 0x35];
  int near = 0, w = trio_weapon(f->s);
  for (int k = 0; k < f->nshots; k++) {
    const TrioShot *sh = &f->shots[k];
    int idx = (sh->slot - (int)g->shot_first) / TRIO_OBJ_SIZE;
    if (sh->x < -5000 || (idx >= 0 && idx < 32 && f->shot_weapon[idx] != w)) continue;
    if (abs(sh->x - f->p.x) < NEAR_X && abs(sh->y - f->p.y) < NEAR_Y) near++;
  }
  if (near < *n) *n = (uint8_t)near;
  //The shot table has room for only a few (8 or 9): with shots crossing
  //the whole arena it fills up and nothing more can be fired. Two slots
  //are kept free: the farthest shots beyond about a screen leave the copy
  //and fly on as the arena's (ghost_take).
  int cap = (int)(g->shot_end - g->shot_first) / TRIO_OBJ_SIZE;
  int used = 0;
  for (int k = 0; k < f->nshots; k++) used += f->shots[k].x > -5000;
  while (used > cap - 2) {
    int far = -1, fd = 0;
    for (int k = 0; k < f->nshots; k++) {
      const TrioShot *sh = &f->shots[k];
      if (sh->x < -5000) continue;
      int d = abs(sh->x - f->p.x) + abs(sh->y - f->p.y);
      if ((abs(sh->x - f->p.x) >= NEAR_X || abs(sh->y - f->p.y) >= NEAR_Y) && d > fd) { fd = d; far = k; }
    }
    if (far < 0) break;
    ghost_take(a, f, f->s, game_of(f), &f->shots[far]);
    trio_kill_shot(f->s, f->shots[far].slot);
    f->shots[far].x = -10000;
    used--;
  }
}

static void forward_sounds(Arena *a, ArenaFighter *f) {
  TrioSnd q[16];
  int n = trio_sound_take(f->s, &f->snd_seen, q, 16);
  for (int k = 0; k < n; k++) {
    if (getenv("ARENA_SND_LOG")) fprintf(stderr, "frame %d: P%d (%s) sound %02X %02X\n", a->frame, (int)(f - a->f) + 1, trio_games[game_of(f)].name, q[k].id, q[k].param);
    const TrioGame *g = trio_game(f->s);
    int which = q[k].id == g->sfx_hurt || q[k].id == g->sfx_death ? SND_BODY : SND_ACT;
    if (q[k].id < 0xF0 && a->snd) snd_play_from(a->snd, (int)(f - a->f), which, g->id, q[k].id, q[k].param);
  }
}

//---------------------------------------------------------------- specials

//The motions, read the way the games read them: each step within 20
//frames of the last, then Y. Fireball: down, down-forward, forward, Y.
//Uppercut: forward, down, down-forward, Y.
#define MOTION_WINDOW 20
static int motions(ArenaFighter *f) {
  uint16_t pad = f->pad, press = pad & (uint16_t)~f->prev_pad;
  uint16_t fwd = f->p.facing_right ? PAD_RIGHT : PAD_LEFT;
  bool down = pad & PAD_DOWN, fw = pad & fwd;
  //Steps per move: the directions to hold at each step (D down, F forward, DF both).
  static const uint8_t steps[3][3] = {{0}, {1, 3, 2}, {2, 1, 3}};   //1 D, 2 F, 3 DF
  int dir = (down ? 1 : 0) | (fw ? 2 : 0);
  for (int m = ARENA_SP_FIREBALL; m <= ARENA_SP_UPPERCUT; m++) {
    if (f->mot[m] > 0 && ++f->mot_t[m] > MOTION_WINDOW) f->mot[m] = 0;
    if (f->mot[m] == 3) {
      if (press & PAD_Y) { f->mot[m] = 0; return m; }
      continue;
    }
    if (dir == steps[m][f->mot[m]]) { f->mot[m]++; f->mot_t[m] = 0; }
    else if (f->mot[m] == 0 || dir != steps[m][f->mot[m] - 1]) { if (dir == steps[m][0]) { f->mot[m] = 1; f->mot_t[m] = 0; } }
  }
  return ARENA_SP_NONE;
}

static int sp_game(int move) { return move == ARENA_SP_FIREBALL ? SNES_GAME_G1 : SNES_GAME_G2; }

//X in the helper copy is put where the fighter stands and given the
//motion, so the game starts the special itself (with the tiles of its
//pose and fireball: forcing its action $42 leaves them unloaded). G1 also
//needs the blaster out.
static bool sp_start(Arena *a, ArenaFighter *f, int move) {
  int g = sp_game(move);
  int act = f->p.action;
  Snes *hb = f->alt_base[g] ? f->alt_base[g] : a->helper_base[g];
  if (!hb || !f->p.ground || (act != TRIO_ACT_IDLE && act != TRIO_ACT_LAND && act != TRIO_ACT_RUN && act != TRIO_ACT_DASH)) return false;
  Snes *h = f->helper[g];
  if (!h) h = f->helper[g] = malloc(sizeof(Snes));
  if (!h) return false;
  if (!f->helper_init[g]) {
    snes_clone(h, hb);
    h->no_apu = true;
    f->helper_init[g] = true;
  }
  if (!f->helper_live[g]) f->nhshots[g] = 0;
  const TrioGame *gm = trio_game(h);
  uint32_t p = gm->player;
  trio_place(h, f->p.x, f->p.y, f->p.facing_right);
  trio_set_hp(h, 16);
  h->wram[p + 0x02] = TRIO_ACT_IDLE;
  h->wram[p + 0x03] = 0;
  trio_set_weapon(h, 0);
  uint16_t F = f->p.facing_right ? PAD_RIGHT : PAD_LEFT, D = PAD_DOWN;
  static const int n = 8;
  const uint16_t had[8] = {0, 0, D, D, D | F, D | F, F, F | PAD_Y};
  const uint16_t sho[8] = {0, 0, F, F, D, D, D | F, D | F | PAD_Y};
  memcpy(f->sp_seq, move == ARENA_SP_FIREBALL ? had : sho, sizeof(had));
  f->sp_seq_n = n;
  f->sp_seq_i = 0;
  f->sp_go = false;
  f->hsnd_seen[g] = h->wram[gm->snd_w] & 0x1E;
  f->sp_move = move;
  f->sp_t = 0;
  f->sp_game = g;
  if (getenv("ARENA_SP_LOG")) fprintf(stderr, "special %d by %s X at frame %d\n", move, trio_games[game_of(f)].name, a->frame);
  f->helper_live[g] = true;
  return true;
}

static void read_shot_list(Snes *s, TrioShot *list, int *n) {
  TrioShot old[16];
  int nold = *n;
  memcpy(old, list, sizeof(old));
  *n = trio_read_shots(s, list, 16);
  for (int k = 0; k < *n; k++)
    for (int j = 0; j < nold; j++)
      if (old[j].slot == list[k].slot && (old[j].x != list[k].x || old[j].y != list[k].y)) {
        list[k].moving = true;
        list[k].vx = list[k].x - old[j].x;
        list[k].vy = list[k].y - old[j].y;
      }
}

//The helpers after the fighter's frame: the hero there hidden unless he is doing
//the Uppercut (then he is the fighter: the fighter's own hero follows him,
//hidden in turn, battle.c), and the fireball or flame read as shots.
static void helpers_step(Arena *a, ArenaFighter *f) {
  for (int g = 1; g <= 3; g++) {
    Snes *h = f->helper[g];
    if (!h || !f->helper_live[g]) continue;
    const TrioGame *gm = trio_game(h);
    uint32_t p = gm->player;
    bool mine = f->sp_move && f->sp_game == g;
    //Once the helper's the hero is in the move he is the one shown (battle.c).
    bool body = mine && f->sp_go;
    trio_set_hp(h, 16);
    if (body) h->wram[p + 0x0E] |= 1;
    else h->wram[p + 0x0E] &= (uint8_t)~1;
    stage_follow(a->st, h);
    h->pad[0] = mine && f->sp_seq_i < f->sp_seq_n ? f->sp_seq[f->sp_seq_i++] : 0;
    h->pad[1] = 0;
    Firm fm = {0};
    if (mine) {
      TrioPlayer hp;
      trio_read_player(h, &hp);
      if (hp.ground) firm(h, hp.x, hp.y + 17, &fm);
    }
    snes_run_frame(h);
    soften(h, &fm);
    if (!body) h->wram[p + 0x0E] &= (uint8_t)~1;
    TrioSnd q[16];
    int n = trio_sound_take(h, &f->hsnd_seen[g], q, 16);
    for (int k = 0; k < n; k++)
      if (q[k].id < 0xF0 && a->snd) snd_play_from(a->snd, (int)(f - a->f), SND_ACT, g, q[k].id, q[k].param);
    read_shot_list(h, f->hshots[g], &f->nhshots[g]);
    if (mine) {
      TrioPlayer hp;
      trio_read_player(h, &hp);
      if (!f->sp_go && hp.action == 0x42) { f->sp_go = true; f->sp_t = 0; }
      f->sp_t++;
      if (body && f->sp_move == ARENA_SP_UPPERCUT) {
        //The fighter goes where the Uppercut takes the hero.
        uint32_t fp = trio_game(f->s)->player;
        trio_place(f->s, hp.x, hp.y, f->p.facing_right);
        f->s->wram[fp + 0x02] = TRIO_ACT_FALL;
        f->s->wram[fp + 0x03] = 0;
        f->p.x = hp.x;
        f->p.y = hp.y;
      }
      bool done = !f->sp_go ? f->sp_t > f->sp_seq_n + 12
                : f->sp_move == ARENA_SP_FIREBALL ? f->sp_t >= 36 : hp.action != 0x42 || f->sp_t > 150;
      if (done) f->sp_move = ARENA_SP_NONE;
    }
    int live = 0;
    for (int k = 0; k < f->nhshots[g]; k++) live += f->hshots[g][k].x > -5000;
    f->helper_live[g] = (f->sp_move && f->sp_game == g) || live > 0;
  }
}

//Weapon energy for all his copies.
static void refill(ArenaFighter *f, int amount) {
  trio_refill_weapons(f->s, amount);
  for (int g = 1; g <= 3; g++)
    if (f->helper[g] && f->helper_init[g]) trio_refill_weapons(f->helper[g], amount);
}

//Into his copy of game g: the hero there takes his place, speed (standing or
//falling), facing and share of life; the copy he leaves runs on, its hero
//hidden, while its shots fly.
static void hop(Arena *a, ArenaFighter *f, int g) {
  if (g < 1 || g > 3 || g == f->cur_game || !f->alt_base[g]) return;
  Snes *h = f->helper[g];
  if (!h) h = f->helper[g] = malloc(sizeof(Snes));
  if (!h) return;
  if (!f->helper_init[g]) {
    snes_clone(h, f->alt_base[g]);
    h->no_apu = true;
    f->helper_init[g] = true;
    f->helper_live[g] = false;
  }
  int og = f->cur_game;
  Snes *old = f->s;
  const TrioGame *om = trio_game(old), *nm = trio_game(h);
  old->wram[om->player + 0x0E] &= (uint8_t)~1;
  memcpy(f->hshots[og], f->shots, sizeof(f->shots));
  f->nhshots[og] = f->nshots;
  int live = 0;
  for (int k = 0; k < f->nshots; k++) live += f->shots[k].x > -5000;
  f->helper[og] = old;
  f->helper_init[og] = true;
  f->helper_live[og] = live > 0;
  f->hsnd_seen[og] = old->wram[om->snd_w] & 0x1E;
  //The new copy.
  memcpy(f->shots, f->hshots[g], sizeof(f->shots));
  f->nshots = f->helper_live[g] ? f->nhshots[g] : 0;
  f->helper[g] = NULL;
  f->helper_live[g] = false;
  f->s = h;
  f->cur_game = g;
  f->have_old = false;
  int max = trio_max_hp(h), hp = (f->p.hp * max + f->max_hp / 2) / (f->max_hp > 0 ? f->max_hp : 16);
  f->max_hp = max;
  trio_place(h, f->p.x, f->p.y, f->p.facing_right);
  h->wram[nm->player + 0x02] = f->p.ground ? TRIO_ACT_IDLE : TRIO_ACT_FALL;
  h->wram[nm->player + 0x03] = 0;
  h->wram[nm->player + 0x0E] |= 1;
  trio_set_hp(h, hp < 1 ? 1 : hp);
  stage_follow(a->st, h);
  f->snd_seen = h->wram[nm->snd_w] & 0x1E;
  f->cam_x = f->oam_cam_x = trio_cam_x(h);
  f->cam_y = f->oam_cam_y = trio_cam_y(h);
  read_fighter(f);
}

static void run_fighter(Arena *a, int i, bool live) {
  ArenaFighter *f = &a->f[i];
  f->pad = live && !f->dead ? f->pad : 0;
  if (f->kind == FK_BOSS) {
    int move = f->dead ? TRIO_MOVE_NONE : boss_move(f, f->pad);
    //Target: the nearest opponent on the side the boss faces.
    f->ctl.has_target = false;
    int best = 1 << 30;
    for (int k = 0; k < ARENA_MAX; k++) {
      const ArenaFighter *o = &a->f[k];
      if (k == i || !o->on || o->dead || o->stocks <= 0) continue;
      int dx = o->p.x - f->p.x;
      if ((dx >= 0) != f->ctl.face_right || abs(dx) >= best) continue;
      best = abs(dx);
      f->ctl.has_target = true;
      f->ctl.tx = o->p.x;
      f->ctl.ty = o->p.y;
    }
    bool was = f->ctl.holding;
    trio_boss_frame(f->s, &f->ctl, move, a->st);
    if (was && !f->ctl.holding) { f->pend_move = TRIO_MOVE_NONE; f->pend_t = 0; }
    f->s->pad[0] = 0;
  } else {
    stage_follow(a->st, f->s);
    uint16_t pad = f->pad;
    //A special from another game: the fighter waits while the helper does it.
    if (f->sp_cd > 0) f->sp_cd--;
    if (f->sp_move) pad = 0;
    else if (f->specials && live) {
      //The motion's last press starts the special: in a helper copy, or in
      //his own game by the game itself. Dropped while he cools down.
      int m = motions(f);
      if (m && f->sp_cd > 0) pad &= (uint16_t)~PAD_Y;
      else if (m && sp_game(m) != game_of(f)) {
        if (sp_start(a, f, m)) { pad &= (uint16_t)~PAD_Y; f->sp_cd = SP_COOLDOWN; }
      } else if (m) f->sp_cd = SP_COOLDOWN;
    }
    //(game_pad can move him into another game's copy.)
    uint16_t gp = game_pad(a, f, pad);
    f->s->pad[0] = gp;
    near_shots(a, f);
  }
  f->prev_pad = f->pad;
  f->s->pad[1] = 0;
  f->oam_cam_x = f->cam_x;
  f->oam_cam_y = f->cam_y;
  f->cam_x = trio_cam_x(f->s);
  f->cam_y = trio_cam_y(f->s);
  Firm fm = {0};
  if (f->kind != FK_BOSS && f->p.ground && !f->drop_t) firm(f->s, f->p.x, f->p.y + 17, &fm);
  snes_run_frame(f->s);
  soften(f->s, &fm);
  //A shot leaving far away takes one off a count we made smaller.
  if (f->kind != FK_BOSS) {
    uint8_t *n = &f->s->wram[trio_game(f->s)->player + 0x35];
    if (*n & 0x80) *n = 0;
  }
  forward_sounds(a, f);
  f->prev = f->p;
  read_fighter(f);
  //The games' warp-in ends by setting the hero 16 px lower than the beam landed
  //(their spawn heights allow for it); here that is inside the floor, where
  //he can't walk. Back up onto the floor's top.
  if (f->kind != FK_BOSS && f->prev.action == TRIO_ACT_WARP && f->p.action != TRIO_ACT_WARP) {
    int top = f->p.y + 17;
    while (top > a->st->def->y0 && trio_at_solid(trio_attr(f->s, f->p.x, top - 1))) top = ((top - 1) & ~15);
    if (top != f->p.y + 17) {
      trio_place(f->s, f->p.x, top - 17, f->p.facing_right);
      read_fighter(f);
    }
  }
  TrioShot old[16];
  int nold = f->nshots;
  memcpy(old, f->shots, sizeof(old));
  f->nshots = f->kind == FK_BOSS ? trio_read_enemy_shots(f->s, f->shots, 16) : trio_read_shots(f->s, f->shots, 16);
  //Live projectiles are the ones that moved: this skips the charge glow and
  //shots stopped by walls, which share the shot slots.
  for (int k = 0; k < f->nshots; k++)
    for (int j = 0; j < nold; j++) {
      TrioShot *sh = &f->shots[k];
      if (old[j].slot != sh->slot) continue;
      if (old[j].x != sh->x || old[j].y != sh->y) sh->moving = true;
      sh->vx = sh->x - old[j].x;
      sh->vy = sh->y - old[j].y;
      //G3's arm-part charge shot, once it leaves the hero, is moved to 128 px off
      //the camera's edge behind him (01:C47A) and flies in from there; in
      //an arena that is out of nowhere. It goes on from where it was.
      if (f->kind == FK_X && game_of(f) == SNES_GAME_G3 && sh->kind == 0x03 && abs(sh->x - old[j].x) > 96) {
        int x = old[j].x + (sh->right ? 8 : -8);
        f->s->wram[sh->slot + 0x05] = (uint8_t)x;
        f->s->wram[sh->slot + 0x06] = (uint8_t)(x >> 8);
        sh->x = x;
        sh->vx = x - old[j].x;
      }
    }
  //Shots that weren't there last frame were fired with the weapon out now.
  if (f->kind != FK_BOSS) {
    const TrioGame *g = trio_game(f->s);
    for (int k = 0; k < f->nshots; k++) {
      bool seen = false;
      for (int j = 0; j < nold && !seen; j++) seen = old[j].slot == f->shots[k].slot;
      int idx = (f->shots[k].slot - (int)g->shot_first) / TRIO_OBJ_SIZE;
      if (!seen && idx >= 0 && idx < 32) f->shot_weapon[idx] = (int8_t)trio_weapon(f->s);
      //Blade's slashes end with a charged blaster pellet: not with the sword button.
      bool slash = f->p.action == TRIO_ACT_SWORD || f->p.action == TRIO_ACT_SWORD_AIR;
      if (!seen && f->kind == FK_BLADE && slash) { trio_kill_shot(f->s, f->shots[k].slot); f->shots[k].x = -10000; }
    }
  }
  if (f->kind == FK_X) helpers_step(a, f);
  if (f->spawn_t > 0) f->spawn_t--;
  if (f->hit_flash > 0) f->hit_flash--;
  if (a->rules.weapons && f->kind == FK_X && a->frame % ENERGY_REGEN == 0) refill(f, 1);
  bool slash = f->p.action == TRIO_ACT_SWORD || f->p.action == TRIO_ACT_SWORD_AIR;
  if (f->kind == FK_BLADE && slash) {
    if (f->prev.action != f->p.action || f->sword_t < 0) { f->sword_t = 0; f->sword_hit = 0; }
    else f->sword_t++;
  } else {
    f->sword_t = -1;
  }
}

//Shields stop the other fighters' shots that touch them.
static void shields(Arena *a) {
  for (int di = 0; di < ARENA_MAX; di++) {
    ArenaFighter *d = &a->f[di];
    if (!d->on || d->dead) continue;
    for (int k = 0; k < d->nshots; k++) {
      ShotInfo si = arena_shot_info(game_of(d), d->shots[k].kind);
      if (!si.shield) continue;
      int bx0, by0, bx1, by1;
      arena_shotbox(game_of(d), &d->shots[k], &bx0, &by0, &bx1, &by1);
      for (int ai = 0; ai < ARENA_MAX; ai++) {
        ArenaFighter *at = &a->f[ai];
        if (ai == di || !at->on) continue;
        for (int j = 0; j < at->nshots; j++) {
          TrioShot *sh = &at->shots[j];
          ShotInfo ti = arena_shot_info(game_of(at), sh->kind);
          if (ti.shield || ti.dmg == 0 || sh->x < -5000) continue;
          int sx0, sy0, sx1, sy1;
          arena_shotbox(game_of(at), sh, &sx0, &sy0, &sx1, &sy1);
          if (sx0 >= bx1 || sx1 <= bx0 || sy0 >= by1 || sy1 <= by0) continue;
          if (at->kind != FK_BOSS) trio_kill_shot(at->s, sh->slot);
          sh->x = -10000;
          add_fx(a, FX_BLOCK, (sx0 + sx1) / 2, (sy0 + sy1) / 2);
        }
      }
    }
  }
}

//Shots of fighter ai (from its copy src, of game game) against the others.
static void shot_list_hit(Arena *a, int ai, Snes *src, int game, TrioShot *list, int n) {
  ArenaFighter *atk = &a->f[ai];
  for (int k = 0; k < n; k++) {
    TrioShot *sh = &list[k];
    ShotInfo si = arena_shot_info(game, sh->kind);
    if (si.dmg <= 0 || sh->x < -5000) continue;
    if (!sh->moving && !si.still) continue;
    int sx0, sy0, sx1, sy1;
    arena_shotbox(game, sh, &sx0, &sy0, &sx1, &sy1);
    for (int ti = 0; ti < ARENA_MAX; ti++) {
      ArenaFighter *t = &a->f[ti];
      if (ti == ai || !vulnerable(t)) continue;
      int hx0, hy0, hx1, hy1;
      arena_hurtbox(t, &hx0, &hy0, &hx1, &hy1);
      if (sx0 >= hx1 || sx1 <= hx0 || sy0 >= hy1 || sy1 <= hy0) continue;
      //Warlord's shield: shots from in front stop there.
      if (t->kind == FK_BOSS && trio_boss_blocks(t->s, sh->right ? sh->x - 32 : sh->x + 32, t->ctl.face_right)) {
        if (!si.pierce && atk->kind != FK_BOSS) { trio_kill_shot(src, sh->slot); sh->x = -10000; }
        add_fx(a, FX_BLOCK, (sx0 + sx1) / 2, (sy0 + sy1) / 2);
        break;
      }
      //The attacker stands where the shot came from, so knockback goes with it.
      hit(a, ti, ai, si.dmg, sh->right ? t->p.x - 16 : t->p.x + 16);
      add_fx(a, si.dmg >= 4 ? FX_BIG_HIT : FX_HIT, (hx0 + hx1) / 2, (hy0 + hy1) / 2);
      if (!si.pierce && !si.still && atk->kind != FK_BOSS) { trio_kill_shot(src, sh->slot); sh->x = -10000; }
      break;
    }
  }
}

static void shots_hit(Arena *a) {
  for (int ai = 0; ai < ARENA_MAX; ai++) {
    ArenaFighter *atk = &a->f[ai];
    //An eliminated fighter's copy is no longer run: its last shots are gone.
    if (!atk->on || (atk->stocks <= 0 && atk->dead)) continue;
    shot_list_hit(a, ai, atk->s, game_of(atk), atk->shots, atk->nshots);
    for (int g = 1; g <= 3; g++)
      if (atk->helper_live[g]) shot_list_hit(a, ai, atk->helper[g], g, atk->hshots[g], atk->nhshots[g]);
  }
}

//The arena's own shots: they fly, stop at the other fighters' shields and
//hit the other fighters; gone a screen outside the box, like the copies'.
static void ghosts_step(Arena *a, bool live) {
  const ArenaDef *d = a->st->def;
  for (int k = 0; k < GHOST_MAX; k++) {
    ArenaGhost *g = &a->ghost[k];
    if (!g->on) continue;
    g->x += g->vx;
    g->y += g->vy;
    if (++g->t > 600 || g->x < d->x0 - 256 || g->x > d->x1 + 256 || g->y < d->y0 - 256 || g->y > d->y1 + 256) { g->on = false; continue; }
    if (!live) continue;
    TrioShot sh = {.x = g->x, .y = g->y, .kind = g->kind, .right = g->right, .moving = true};
    ShotInfo si = arena_shot_info(g->game, g->kind);
    int sx0, sy0, sx1, sy1;
    arena_shotbox(g->game, &sh, &sx0, &sy0, &sx1, &sy1);
    for (int di = 0; di < ARENA_MAX && g->on; di++) {
      const ArenaFighter *df = &a->f[di];
      if (di == g->owner || !df->on || df->dead) continue;
      for (int j = 0; j < df->nshots && g->on; j++) {
        if (!arena_shot_info(game_of(df), df->shots[j].kind).shield) continue;
        int bx0, by0, bx1, by1;
        arena_shotbox(game_of(df), &df->shots[j], &bx0, &by0, &bx1, &by1);
        if (sx0 >= bx1 || sx1 <= bx0 || sy0 >= by1 || sy1 <= by0) continue;
        g->on = false;
        add_fx(a, FX_BLOCK, g->x, g->y);
      }
    }
    for (int ti = 0; ti < ARENA_MAX && g->on; ti++) {
      ArenaFighter *t = &a->f[ti];
      if (ti == g->owner || !vulnerable(t)) continue;
      int hx0, hy0, hx1, hy1;
      arena_hurtbox(t, &hx0, &hy0, &hx1, &hy1);
      if (sx0 >= hx1 || sx1 <= hx0 || sy0 >= hy1 || sy1 <= hy0) continue;
      if (t->kind == FK_BOSS && trio_boss_blocks(t->s, g->right ? g->x - 32 : g->x + 32, t->ctl.face_right)) {
        if (!si.pierce) g->on = false;
        add_fx(a, FX_BLOCK, g->x, g->y);
        break;
      }
      hit(a, ti, g->owner, si.dmg, g->right ? t->p.x - 16 : t->p.x + 16);
      add_fx(a, si.dmg >= 4 ? FX_BIG_HIT : FX_HIT, (hx0 + hx1) / 2, (hy0 + hy1) / 2);
      if (!si.pierce) g->on = false;
      break;
    }
  }
}

static void swords(Arena *a) {
  for (int ai = 0; ai < ARENA_MAX; ai++) {
    ArenaFighter *atk = &a->f[ai];
    bool air = atk->p.action == TRIO_ACT_SWORD_AIR;
    if (!atk->on || atk->dead || atk->sword_t < (air ? SWORD_AIR_FROM : SWORD_FROM) || atk->sword_t > (air ? SWORD_AIR_TO : SWORD_TO)) continue;
    int sx0, sy0, sx1, sy1;
    arena_swordbox(&atk->p, &sx0, &sy0, &sx1, &sy1);
    for (int ti = 0; ti < ARENA_MAX; ti++) {
      ArenaFighter *t = &a->f[ti];
      if (ti == ai || (atk->sword_hit >> ti & 1) || !vulnerable(t)) continue;
      int hx0, hy0, hx1, hy1;
      arena_hurtbox(t, &hx0, &hy0, &hx1, &hy1);
      if (sx0 >= hx1 || sx1 <= hx0 || sy0 >= hy1 || sy1 <= hy0) continue;
      hit(a, ti, ai, SWORD_DAMAGE, atk->p.x);
      atk->sword_hit |= (uint8_t)(1 << ti);
      add_fx(a, FX_BIG_HIT, (hx0 + hx1) / 2, (hy0 + hy1) / 2);
    }
  }
}

//A boss in one of its attacks hurts whoever its body touches (punches,
//charges, stomps: in its own copy they would hit the stand-in hero).
#define BODY_DAMAGE 2
static void bodies(Arena *a) {
  for (int ai = 0; ai < ARENA_MAX; ai++) {
    ArenaFighter *atk = &a->f[ai];
    if (!atk->on || atk->dead || atk->kind != FK_BOSS || atk->ctl.holding || atk->stocks <= 0) continue;
    int bx0, by0, bx1, by1;
    arena_hurtbox(atk, &bx0, &by0, &bx1, &by1);
    for (int ti = 0; ti < ARENA_MAX; ti++) {
      ArenaFighter *t = &a->f[ti];
      if (ti == ai || !vulnerable(t)) continue;
      int hx0, hy0, hx1, hy1;
      arena_hurtbox(t, &hx0, &hy0, &hx1, &hy1);
      if (bx0 >= hx1 || bx1 <= hx0 || by0 >= hy1 || by1 <= hy0) continue;
      hit(a, ti, ai, BODY_DAMAGE, atk->p.x);
      add_fx(a, FX_HIT, (hx0 + hx1) / 2, (hy0 + hy1) / 2);
    }
  }
}

//---------------------------------------------------------------- items

static bool item_floor(const TrioGeo *g, int x, int y) {
  int at = trio_geo_at(g, x, y);
  return trio_at_solid(at) || (at >= 1 && at <= 0x0C) || at == TRIO_AT_LADDER_TOP;
}

static void drop_item(Arena *a, int kind, int x, int y) {
  for (int k = 0; k < ARENA_ITEMS; k++)
    if (a->items[k].kind == IT_NONE) {
      a->items[k] = (ArenaItem){kind, x, y, 0, 0};
      return;
    }
}

static void items(Arena *a) {
  const ArenaStage *st = a->st;
  if (a->rules.items && a->phase == AP_FIGHT && ++a->item_t >= ITEM_EVERY) {
    a->item_t = (int)(rnd(a) % 180);
    //Falling onto one of the box's open floors (a drop from the top could
    //land on a ceiling out of reach).
    int x = (st->def->x0 + st->def->x1) / 2, y = st->def->y0 + 8;
    if (st->nfloor) {
      int k = (int)(rnd(a) % (uint32_t)st->nfloor);
      x = st->floor_x[k];
      y = st->floor_y[k] - 48;
    }
    uint32_t r = rnd(a) % 100;
    int kind = r < 45 ? IT_LIFE_S : r < 70 ? IT_LIFE_L : r < 93 ? IT_WEAPON : IT_HEART;
    drop_item(a, kind, x, y);
  }
  for (int k = 0; k < ARENA_ITEMS; k++) {
    ArenaItem *it = &a->items[k];
    if (it->kind == IT_NONE) continue;
    it->t++;
    //Falls onto the floors, 1/16 px units.
    if (!item_floor(&st->geo, it->x, it->y + 1)) {
      it->vy += 4;
      if (it->vy > 64) it->vy = 64;
      for (int s = 0; s < it->vy / 16 + 1; s++)
        if (!item_floor(&st->geo, it->x, it->y + 1)) it->y++;
    } else {
      it->vy = 0;
    }
    if (it->y > st->def->y1 + 32 || it->t > 60 * 20) { it->kind = IT_NONE; continue; }
    //Picked up by the first fighter touching it.
    for (int i = 0; i < ARENA_MAX; i++) {
      ArenaFighter *f = &a->f[i];
      if (!f->on || f->dead || f->stocks <= 0 || f->kind == FK_BOSS) continue;
      if (abs(f->p.x - it->x) > 12 || it->y < f->p.y - 20 || it->y > f->p.y + 20) continue;
      const TrioGame *g = trio_game(f->s);
      switch (it->kind) {
      case IT_LIFE_S: case IT_LIFE_L: {
        int add = (it->kind == IT_LIFE_S ? 4 : 8) * f->max_hp / 16;
        int hp = f->p.hp + add;
        trio_set_hp(f->s, hp > f->max_hp ? f->max_hp : hp);
        if (a->snd) snd_play_from(a->snd, i, SND_ACT, g->id, g->id == SNES_GAME_G1 ? 0x0C : 0x15, 0);
        break;
      }
      case IT_WEAPON:
        refill(f, 32);
        if (a->snd) snd_play_from(a->snd, i, SND_ACT, g->id, g->id == SNES_GAME_G1 ? 0x0C : 0x15, 0);
        break;
      case IT_HEART:
        f->stocks++;
        if (a->snd) snd_play_from(a->snd, i, SND_ACT, g->id, g->id == SNES_GAME_G1 ? 0x29 : 0x19, 0);
        break;
      }
      add_fx(a, it->kind == IT_HEART ? FX_1UP : FX_ITEM, it->x, it->y);
      if (getenv("ARENA_ITEM_LOG")) fprintf(stderr, "item %d taken by P%d (kind %d) at frame %d\n", it->kind, i + 1, f->kind, a->frame);
      it->kind = IT_NONE;
      break;
    }
  }
}

//---------------------------------------------------------------- hazards

//Moves a fighter sideways by dx in its copy (unless into a wall).
static void nudge(ArenaFighter *f, int dx) {
  if (!dx) return;
  int side = f->kind == FK_BOSS ? 18 : 8;
  int nx = f->p.x + dx + (dx > 0 ? side : -side);
  if (trio_at_solid(trio_attr(f->s, nx, f->p.y)) || trio_at_solid(trio_attr(f->s, nx, f->p.y + 12))) return;
  uint32_t o = f->kind == FK_BOSS ? trio_game(f->s)->enemy_first : trio_game(f->s)->player;
  uint16_t x = (uint16_t)(f->s->wram[o + 0x05] | f->s->wram[o + 0x06] << 8);
  x = (uint16_t)(x + dx);
  f->s->wram[o + 0x05] = (uint8_t)x;
  f->s->wram[o + 0x06] = (uint8_t)(x >> 8);
  if (f->kind != FK_BOSS) {
    f->s->wram[o + 0x22] = (uint8_t)x;
    f->s->wram[o + 0x23] = (uint8_t)(x >> 8);
  }
  f->p.x += dx;
}

static bool in_haz(const ArenaHazard *h, int x, int y) { return x >= h->x0 && x < h->x1 && y >= h->y0 && y < h->y1; }

#define FIRE_CYCLE 300
#define FIRE_WARN 60
#define FIRE_BURN 70
int arena_fire_phase(const Arena *a, int k, int *height) {
  int t = (a->frame + k * FIRE_CYCLE / 2) % FIRE_CYCLE;
  int burn0 = FIRE_CYCLE - FIRE_BURN, warn0 = burn0 - FIRE_WARN;
  if (height) *height = 0;
  if (t < warn0) return 0;
  if (t < burn0) return 1;
  int u = t - burn0, h = u < 10 ? u * 256 / 10 : u > FIRE_BURN - 10 ? (FIRE_BURN - u) * 256 / 10 : 256;
  if (height) *height = h;
  return 2;
}

//Hurt by the stage: like a hit, the last fighter to hit him keeps the KO.
static void hurt(Arena *a, int ti, int dmg, int from_x) {
  ArenaFighter *t = &a->f[ti];
  if (t->kind == FK_BOSS) trio_boss_damage(t->s, dmg);
  else trio_damage(t->s, even_damage(t, dmg), from_x, t->p.y);
  read_fighter(t);
  t->hit_flash = 8;
  add_fx(a, FX_HIT, t->p.x, t->p.y);
}

static void hazards(Arena *a) {
  const ArenaStage *st = a->st;
  if (!st->nhaz) return;
  //The gust turns about every 6 s.
  if (a->frame % 360 == 0) a->wind_dir = a->wind_dir > 0 ? -1 : 1;
  for (int i = 0; i < ARENA_MAX; i++) {
    ArenaFighter *f = &a->f[i];
    if (!f->on || f->dead || f->stocks <= 0 || f->p.action == TRIO_ACT_WARP) continue;
    bool iced = false;
    for (int k = 0; k < st->nhaz; k++) {
      const ArenaHazard *h = &st->haz[k];
      int feet = f->p.y + 17;
      switch (h->kind) {
      case HZ_ICE:
        if (f->p.ground && in_haz(h, f->p.x, feet - 1)) iced = true;
        break;
      case HZ_WIND:
        if (in_haz(h, f->p.x, f->p.y)) nudge(f, a->frame % 3 == 0 || !f->p.ground ? a->wind_dir : 0);
        break;
      case HZ_FIRE: {
        int hgt;
        if (arena_fire_phase(a, k, &hgt) != 2 || !vulnerable(f)) break;
        int top = h->y1 - (h->y1 - h->y0) * hgt / 256;
        if (f->p.x + 7 > h->x0 && f->p.x - 7 < h->x1 && feet > top && f->p.y - 14 < h->y1) hurt(a, i, 3, (h->x0 + h->x1) / 2);
        break;
      }
      }
    }
    //Ice: the speed he had is kept and changes slowly; what the game moved
    //him this frame is made up to it.
    int moved = (f->p.x - f->prev.x) * 256;
    if (iced && abs(moved) < 8 * 256) {
      f->slide += (moved - f->slide) / 14;
      int want = f->slide / 256, dx = want - (f->p.x - f->prev.x);
      if (dx) nudge(f, dx);
    } else {
      f->slide = moved;
    }
  }
}

//---------------------------------------------------------------- step

//---------------------------------------------------------------- platforms

#define MOVER_SPEED 0xA0        //1/256 px per frame

//Collision of platform m in copy s: ladder tops where it is, the arena's
//own blocks along the rest of its lane.
static void mover_blocks(const Arena *a, const ArenaMover *m, Snes *s) {
  const ArenaLane *l = &m->lane;
  int n = (l->x1 - l->x0) / 16 + l->w + 1;
  for (int c = 0; c < n; c++) {
    int x = l->x0 + c * 16;
    bool on = c >= m->col && c < m->col + l->w;
    trio_put_block(s, x, l->y, on ? TRIO_AT_LADDER_TOP : trio_geo_at(&a->st->geo, x, l->y));
  }
}

//Moves the platforms, carries whoever stands on one and puts their
//collision into every copy.
static void movers_step(Arena *a, bool moving) {
  for (int k = 0; k < a->nmover; k++) {
    ArenaMover *m = &a->mover[k];
    const ArenaLane *l = &m->lane;
    int old = m->px, old_col = m->col;
    if (moving) {
      m->x += m->dir * MOVER_SPEED;
      if (m->dir > 0 && m->x >= l->x1 << 8) { m->x = l->x1 << 8; m->dir = -1; }
      if (m->dir < 0 && m->x <= l->x0 << 8) { m->x = l->x0 << 8; m->dir = 1; }
    }
    m->px = m->x >> 8;
    m->col = (m->px - l->x0 + 8) >> 4;
    int dx = m->px - old;
    for (int i = 0; i < ARENA_MAX; i++) {
      ArenaFighter *f = &a->f[i];
      if (!f->on || !f->s || (f->stocks <= 0 && f->dead)) continue;
      int feet = f->p.y + 17;
      int c0 = l->x0 + old_col * 16, c1 = c0 + l->w * 16;
      bool riding = dx && !f->dead && f->p.ground && feet == l->y && f->p.x >= c0 - 6 && f->p.x < c1 + 6;
      if (riding) {
        uint32_t o = f->kind == FK_BOSS ? trio_game(f->s)->enemy_first : trio_game(f->s)->player;
        uint16_t x = (uint16_t)(f->s->wram[o + 0x05] | f->s->wram[o + 0x06] << 8);
        x = (uint16_t)(x + dx);
        f->s->wram[o + 0x05] = (uint8_t)x;
        f->s->wram[o + 0x06] = (uint8_t)(x >> 8);
        if (f->kind != FK_BOSS) {
          f->s->wram[o + 0x22] = (uint8_t)x;
          f->s->wram[o + 0x23] = (uint8_t)(x >> 8);
        }
        f->p.x += dx;
      }
      mover_blocks(a, m, f->s);
      for (int g = 1; g <= 3; g++)
        if (f->helper[g] && f->helper_init[g]) mover_blocks(a, m, f->helper[g]);
    }
  }
}

//No hits for 15 s and a CPU fighter far from everyone (a boss in a pit it
//can't climb out of, a bot on a ledge it can't leave): it beams back in at
//the spawn point nearest an opponent, keeping its HP and lives. People are
//never moved.
#define STALE_FRAMES (15 * 60)
static void unstick(Arena *a) {
  if (a->frame - a->last_any_hit < STALE_FRAMES) return;
  a->last_any_hit = a->frame;
  for (int i = 0; i < ARENA_MAX; i++) {
    ArenaFighter *f = &a->f[i];
    if (!f->on || !f->cpu || f->dead || f->stocks <= 0) continue;
    int near = 1 << 30;
    for (int k = 0; k < ARENA_MAX; k++) {
      const ArenaFighter *o = &a->f[k];
      if (k == i || !o->on || o->dead || o->stocks <= 0) continue;
      //Height counts double: floors apart are usually walls apart too.
      int d = abs(o->p.x - f->p.x) + 2 * abs(o->p.y - f->p.y);
      if (d < near) near = d;
    }
    if (near < 320 || near == 1 << 30) continue;
    //The spawn point nearest an opponent (not on top of one).
    int best = -1, bd = 1 << 30;
    for (int s = 0; s < 4; s++)
      for (int k = 0; k < ARENA_MAX; k++) {
        const ArenaFighter *o = &a->f[k];
        if (k == i || !o->on || o->dead || o->stocks <= 0) continue;
        int d = abs(o->p.x - a->st->spawn_x[s]) + abs(o->p.y + 17 - a->st->spawn_y[s]);
        if (d >= 48 && d < bd) { bd = d; best = s; }
      }
    if (best < 0) continue;
    int hp = f->p.hp;
    a->force_spawn = best;
    spawn(a, i, false);
    a->force_spawn = -1;
    if (f->kind == FK_BOSS) trio_boss_set_hp(f->s, hp);
    else trio_set_hp(f->s, hp);
    read_fighter(f);
    f->spawn_t = 0;
  }
}

void arena_step(Arena *a, const uint16_t pads[ARENA_MAX]) {
  a->nfx = 0;
  a->frame++;
  a->phase_t++;
  if (a->phase == AP_COUNTDOWN && a->phase_t >= COUNTDOWN) { a->phase = AP_FIGHT; a->phase_t = 0; }
  bool live = a->phase == AP_FIGHT;
  movers_step(a, a->phase != AP_COUNTDOWN);

  for (int i = 0; i < ARENA_MAX; i++) {
    ArenaFighter *f = &a->f[i];
    if (!f->on || (f->stocks <= 0 && f->dead)) continue;
    f->pad = pads[i];
    run_fighter(a, i, live);
  }

  if (live) {
    shields(a);
    shots_hit(a);
    swords(a);
    bodies(a);
  }
  ghosts_step(a, live);
  items(a);
  if (live) hazards(a);

  //Deaths, stocks, respawns.
  for (int i = 0; i < ARENA_MAX; i++) {
    ArenaFighter *f = &a->f[i];
    if (!f->on) continue;
    bool gone = f->p.hp == 0 || f->p.action == TRIO_ACT_DYING || f->p.y > a->st->def->y1 + 96;
    if (f->kind == FK_BOSS) gone = f->p.hp == 0 || f->p.y > a->st->def->y1 + 96;
    if (!f->dead && gone && live) {
      f->dead = true;
      f->dead_t = 0;
      f->falls++;
      f->stocks--;
      if (f->last_hitter >= 0 && a->frame - f->last_hit_t < KO_CREDIT) a->f[f->last_hitter].kos++;
      add_fx(a, FX_KO, f->p.x, f->p.y);
      //A life capsule is left behind.
      if (a->rules.items && f->stocks > 0) drop_item(a, IT_LIFE_S, f->p.x, f->p.y < a->st->def->y1 ? f->p.y : a->st->def->y1 - 32);
    }
    if (f->dead) {
      f->dead_t++;
      if (f->dead_t >= DEATH_FRAMES && f->stocks > 0) spawn(a, i, false);
    }
  }

  if (live) unstick(a);
  if (a->phase == AP_FIGHT) a->fight_t++;
  if (a->phase == AP_FIGHT && arena_alive(a) <= 1) {
    a->phase = AP_GAME;
    a->phase_t = 0;
    for (int i = 0; i < ARENA_MAX; i++)
      if (a->f[i].on && a->f[i].stocks > 0) a->winner = i;
  } else if (a->phase == AP_FIGHT && a->rules.time_limit && a->fight_t >= a->rules.time_limit) {
    //TIME: the most lives left wins, then the most life (as a share of a
    //full one), then the most damage dealt; a tie on all three is a draw.
    a->phase = AP_GAME;
    a->phase_t = 0;
    a->time_up = true;
    long best = -1;
    bool tie = false;
    for (int i = 0; i < ARENA_MAX; i++) {
      const ArenaFighter *f = &a->f[i];
      if (!f->on || f->stocks <= 0) continue;
      int hp = f->dead ? 0 : f->p.hp * 64 / (f->max_hp > 0 ? f->max_hp : 16);
      long sc = (long)f->stocks * 1000000 + hp * 10000L + (f->dealt < 9999 ? f->dealt : 9999);
      if (sc > best) { best = sc; a->winner = i; tie = false; }
      else if (sc == best) tie = true;
    }
    if (tie) a->winner = -1;
  } else if (a->phase == AP_GAME && a->phase_t >= 150) {
    a->phase = AP_OVER;
  }
}
