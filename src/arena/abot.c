#include "abot.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

//CPU players. Arenas have floors at several heights, so a bot first finds
//its way to the floor its target stands on through a graph of the arena's
//platforms (jumps up, drops, jumps across), then fights like the old
//single-floor bot: keeps its distance, lines up shots, dodges.

typedef struct { int react; float aggro, dodge, charge; int spacing; } BotLevel;

static const BotLevel levels[4] = {
  {0},
  {24, 0.08f, 0.10f, 0.15f, 70},
  {12, 0.20f, 0.35f, 0.35f, 80},
  {4, 0.35f, 0.60f, 0.50f, 80},
};

static uint32_t rnd(ABot *b) {
  uint32_t x = b->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return b->rng = x ? x : 1;
}
static float rf(ABot *b) { return (rnd(b) >> 8) * (1.0f / 16777216.0f); }

void abot_init(ABot *b, int index, uint32_t seed) {
  memset(b, 0, sizeof(*b));
  b->rng = seed * 2654435761u + (uint32_t)index * 97u + 1;
  b->target = -1;
  b->goal_item = -1;
}

static bool alive(const ArenaFighter *f) { return f->on && !f->dead && f->stocks > 0; }

//---------------------------------------------------------------- platforms

//A platform: a run of floor blocks with room for the hero above, top at y.
typedef struct { int y, x0, x1; } Plat;
#define MAXPLAT 256
static Plat plats[MAXPLAT];
static int nplat;
//The arena the graph was built for (its definition: the stage struct is
//reused when the arena changes) and the match (moving platforms and failed
//climbs change it).
static const ArenaDef *plats_def;
static const Arena *plats_arena;
static int plats_frame;
static uint8_t reach[MAXPLAT][MAXPLAT];   //0 none, 1 walk off, 2 jump, 3 wall climb, 4 ladder
//Wall climbs: the column beside the wall the hero kicks up (block x) and the
//side the wall is on.
static int16_t climb_x[MAXPLAT][MAXPLAT];
static int8_t climb_side[MAXPLAT][MAXPLAT];
static uint8_t climb_from[MAXPLAT][MAXPLAT];   //failed tries of a wall climb

static bool standable(int a) { return trio_at_solid(a) || (a >= 1 && a <= 0x0C) || a == TRIO_AT_LADDER_TOP; }

#define JUMP_UP 52               //highest floor a jump reaches (the hero jumps 55 px)
#define CLOSE 32                 //nearest a bot stands to its target
#define JUMP_GAP 96              //widest gap a dash jump clears

static void build_plats(const ArenaStage *st) {
  nplat = 0;
  const TrioGeo *g = &st->geo;
  const ArenaDef *d = st->def;
  for (int y = d->y0 + 32; y < d->y1; y += 16) {
    int run0 = -1;
    for (int x = d->x0; x <= d->x1; x += 16) {
      bool ok = x < d->x1 && standable(trio_geo_at(g, x, y)) && trio_geo_at(g, x, y - 16) == 0 && trio_geo_at(g, x, y - 32) == 0;
      if (ok && run0 < 0) run0 = x;
      if (!ok && run0 >= 0) {
        if (nplat < MAXPLAT && x - run0 >= 32) plats[nplat++] = (Plat){y, run0, x - 1};
        run0 = -1;
      }
    }
  }
  memset(reach, 0, sizeof(reach));
  for (int a = 0; a < nplat; a++)
    for (int b = 0; b < nplat; b++) {
      if (a == b) continue;
      const Plat *A = &plats[a], *B = &plats[b];
      int gap = B->x0 > A->x1 ? B->x0 - A->x1 : A->x0 > B->x1 ? A->x0 - B->x1 : 0;
      int up = A->y - B->y;
      if (up > 0 && up <= JUMP_UP && gap <= JUMP_GAP - up) reach[a][b] = 2;
      else if (up >= -8 && up <= 0 && gap > 0 && gap <= JUMP_GAP) reach[a][b] = 2;
      //Walking off an edge lands on a floor below that edge.
      else if (up < -8 && ((B->x0 <= A->x1 + 40 && B->x1 >= A->x1 + 8) || (B->x1 >= A->x0 - 40 && B->x0 <= A->x0 - 8))) reach[a][b] = 1;
    }
  //Wall climbs: from a floor, up a wall beside it with wall kicks, to a
  //floor on top of that wall or beside its column higher up.
  for (int a = 0; a < nplat; a++) {
    const Plat *A = &plats[a];
    for (int x = A->x0 - 16; x <= A->x1 + 16; x += 16)
      for (int side = -1; side <= 1; side += 2) {
        int y = A->y - 16;
        if (trio_geo_at(g, x, y) != 0 || !trio_at_solid(trio_geo_at(g, x + side * 16, y))) continue;
        //Up the column while the wall goes on beside it.
        while (y > d->y0 && trio_geo_at(g, x, y - 16) == 0 && trio_at_solid(trio_geo_at(g, x + side * 16, y - 16))) y -= 16;
        int top = y;   //the wall's last block beside the column
        if (A->y - top < JUMP_UP + 16) continue;
        for (int b = 0; b < nplat; b++) {
          const Plat *B = &plats[b];
          if (b == a || reach[a][b] || B->y >= A->y - JUMP_UP || B->y < top - 48) continue;
          int wx = x + side * 16;
          bool on_top = B->x0 <= wx + 8 && B->x1 >= wx - 8;
          bool beside = side > 0 ? (B->x1 >= x - 56 && B->x1 < wx) : (B->x0 <= x + 72 && B->x0 > wx);
          if (!on_top && !beside) continue;
          //Open column up to B's standing height: a floor over a capped
          //column can't be reached from below.
          bool open = true;
          for (int yy = top - 16; yy >= B->y - 32 && open; yy -= 16) open = trio_geo_at(g, x, yy) == 0;
          if (!open) continue;
          reach[a][b] = 3;
          climb_from[a][b] = 0;
          climb_x[a][b] = (int16_t)(x + 8);
          climb_side[a][b] = (int8_t)side;
        }
      }
  }
  //Ladders: a ladder top on a floor, its column down to a floor below;
  //climbed up (UP) or down (DOWN) at the column, climb_x the column's middle.
  for (int b = 0; b < nplat; b++) {
    const Plat *B = &plats[b];
    for (int x = B->x0; x <= B->x1; x += 16) {
      if (trio_geo_at(g, x, B->y) != TRIO_AT_LADDER_TOP) continue;
      int y = B->y + 16;
      while (y < d->y1 && trio_geo_at(g, x, y) == TRIO_AT_LADDER) y += 16;
      for (int a = 0; a < nplat; a++) {
        const Plat *A = &plats[a];
        if (a == b || A->y != y || x < A->x0 - 8 || x > A->x1 + 8) continue;
        int cx = (x & ~15) + 8;
        reach[a][b] = reach[b][a] = 4;
        climb_x[a][b] = climb_x[b][a] = (int16_t)cx;
      }
    }
  }
  plats_def = st->def;
}

static int plat_at(int x, int feet) {
  int best = -1, bd = 1 << 30;
  for (int k = 0; k < nplat; k++) {
    const Plat *p = &plats[k];
    if (x < p->x0 - 12 || x > p->x1 + 12) continue;
    int dy = p->y - feet;
    if (dy < -6) continue;               //above the feet
    if (dy < bd) { bd = dy; best = k; }
  }
  return best;
}

//Breadth-first from platform a: q gets the platforms reachable from it
//(count returned), prev the step before each (-2 unreached).
static int flood(int a, int *prev, int *q) {
  int qh = 0, qt = 0;
  for (int k = 0; k < nplat; k++) prev[k] = -2;
  prev[a] = -1;
  q[qt++] = a;
  while (qh < qt) {
    int u = q[qh++];
    for (int v = 0; v < nplat; v++)
      if (reach[u][v] && prev[v] == -2) { prev[v] = u; q[qt++] = v; }
  }
  return qt;
}

//Air straight above (x, y) up to height top, in this fighter's copy.
static bool open_above(const Snes *s, int x, int y, int top) {
  for (int yy = y - 24; yy >= top; yy -= 16)
    if (trio_at_solid(trio_attr(s, x, yy))) return false;
  return true;
}

//First platform on the way from a toward (gx, gy): to the goal's platform
//b when it can be reached, else to the reachable platform nearest the goal.
//*final gets the platform the path ends on.
static int next_plat(int a, int b, int gx, int gy, int *final) {
  *final = -1;
  if (a < 0) return -1;
  int prev[MAXPLAT], q[MAXPLAT];
  int qt = flood(a, prev, q);
  int end = b >= 0 && prev[b] != -2 ? b : -1;
  if (end < 0) {
    int bd = 1 << 30;
    for (int k = 0; k < qt; k++) {
      const Plat *p = &plats[q[k]];
      int cx = gx < p->x0 ? p->x0 : gx > p->x1 ? p->x1 : gx;
      int d = abs(cx - gx) + 2 * abs(p->y - (gy + 17));
      if (d < bd) { bd = d; end = q[k]; }
    }
  }
  *final = end;
  if (end < 0 || end == a) return a;
  int v = end;
  while (prev[v] != a) v = prev[v];
  return v;
}

//Safe ground somewhere under (x, feet): the first block that isn't air or
//water, down to the bottom of the box, is a floor and not a hazard.
static bool safe_below(const TrioGeo *g, const ArenaDef *d, int x, int feet) {
  for (int y = feet; y < d->y1 + 16; y += 16) {
    int at = trio_geo_at(g, x, y);
    if (at == 0 || at == TRIO_AT_WATER || at == TRIO_AT_WATER_TOP || at == TRIO_AT_LADDER) continue;
    return standable(at);
  }
  return false;
}

//---------------------------------------------------------------- senses

static int pick_target(ABot *b, const Arena *a, int me) {
  const ArenaFighter *m = &a->f[me];
  int best = -1, bs = 1 << 30;
  for (int i = 0; i < ARENA_MAX; i++) {
    const ArenaFighter *t = &a->f[i];
    if (i == me || !alive(t)) continue;
    int d = abs(t->p.x - m->p.x) + 2 * abs(t->p.y - m->p.y) + (t->spawn_t > 0 ? 300 : 0) + (int)(rf(b) * 80);
    if (d < bs) { bs = d; best = i; }
  }
  return best;
}

//Clear shot along the blaster's height from the shooter to the target.
static bool clear_line(const Snes *s, int x0, int x1, int y) {
  int step = x1 > x0 ? 8 : -8;
  for (int x = x0 + step * 2; step > 0 ? x < x1 : x > x1; x += step)
    if (trio_at_solid(trio_attr(s, x, y))) return false;
  return true;
}

//An enemy shot about to reach this player.
static bool threatened(const Arena *a, int me) {
  const TrioPlayer *p = &a->f[me].p;
  for (int i = 0; i < ARENA_MAX; i++) {
    const ArenaFighter *o = &a->f[i];
    if (i == me || !o->on) continue;
    for (int k = 0; k < o->nshots; k++) {
      const TrioShot *s = &o->shots[k];
      int dx = p->x - s->x;
      bool toward = s->right ? dx > 0 : dx < 0;
      if (s->moving && toward && abs(dx) < 90 && abs(s->y - p->y) < 22) return true;
    }
  }
  return false;
}

//---------------------------------------------------------------- boss

//A boss: walks and jumps between its attacks (B, held for height; Down + B
//off a platform), and picks one of its attacks (X jump attack, Y, A) when
//it is back in its wait on the ground.
static bool safe_below(const TrioGeo *g, const ArenaDef *d, int x, int feet);

static uint16_t boss_think(ABot *b, const Arena *a, const ArenaFighter *m, const TrioPlayer *t) {
  const TrioPlayer *p = &m->p;
  int dx = t->x - p->x, dy = t->y - p->y, dir = dx >= 0 ? 1 : -1, adx = abs(dx);
  bool facing = p->facing_right == (dir > 0);
  uint16_t bp = adx > 90 || !facing ? (dir > 0 ? PAD_RIGHT : PAD_LEFT) : 0;
  //No walking (or steering in the air) over a pit or spikes: turning to face
  //the target is still allowed when standing.
  if (bp) {
    int ahead = p->x + (bp & PAD_RIGHT ? 24 : -24);
    if (!safe_below(&a->st->geo, a->st->def, ahead, p->y + 17)) {
      if (p->ground && !facing) bp = (uint16_t)(bp & (PAD_LEFT | PAD_RIGHT));   //a tap turns it
      else bp = p->ground ? 0 : (uint16_t)(bp & PAD_RIGHT ? PAD_LEFT : PAD_RIGHT);
    }
  }
  bool idle = trio_boss_idle(m->s);
  //Jumps: toward a target above, over whatever stops a walk, held for
  //height; off a platform toward a target below.
  if (b->jump_hold > 0) { b->jump_hold--; bp |= PAD_B; }
  if (p->ground && (bp & (PAD_LEFT | PAD_RIGHT)) && p->x == b->last_x) b->stuck_t++;
  else b->stuck_t = 0;
  b->last_x = p->x;
  bool can_jump = idle && p->ground && !(b->last & PAD_B);
  if (can_jump && (b->stuck_t > 8 || (dy < -40 && rf(b) < 0.06f + 0.02f * m->level)) &&
      safe_below(&a->st->geo, a->st->def, p->x + dir * 48, p->y + 17)) {
    bp |= PAD_B;
    b->jump_hold = 20;
    b->stuck_t = 0;
  } else if (can_jump && dy > 56 && adx < 120 && rf(b) < 0.03f) {
    bp |= PAD_DOWN | PAD_B;
  }
  //Attacks, from the ground; the chosen button is held until one starts.
  if (b->tap_cd > 0 && idle) b->tap_cd--;
  if (!idle) b->holding_y = 0;
  else if (!b->holding_y && b->tap_cd == 0 && p->ground) {
    float r = rf(b);
    if (adx > 150 || dy < -40) b->holding_y = r < 0.6f ? PAD_X : PAD_A;
    else if (abs(dy) < 40) b->holding_y = r < 0.45f ? PAD_Y : r < 0.75f ? PAD_A : PAD_X;
    else b->holding_y = r < 0.5f ? PAD_A : PAD_X;
    //The jump attack and the special can carry it a long way (dashes,
    //leaps): only the shot when the floor ahead ends in a pit.
    int face = p->facing_right ? 1 : -1;
    bool safe = true;
    for (int k = 1; k <= 6 && safe; k++) safe = safe_below(&a->st->geo, a->st->def, p->x + face * 24 * k, p->y + 17);
    if (!safe) b->holding_y = PAD_Y;
    b->tap_cd = 40 - 10 * m->level + (int)(rf(b) * 20);
  }
  if (idle && p->ground) bp |= (uint16_t)b->holding_y;
  b->last = bp;
  return bp;
}

//---------------------------------------------------------------- think

uint16_t abot_think(ABot *b, const Arena *a, int me) {
  const ArenaFighter *m = &a->f[me];
  const BotLevel *L = &levels[m->level < 1 ? 1 : m->level > 3 ? 3 : m->level];
  uint16_t pad = 0;
  if (!alive(m) || a->phase != AP_FIGHT) { b->last = 0; b->holding_y = 0; b->jump_hold = 0; return 0; }
  //Rebuilt for a new arena or a new match.
  if (plats_def != a->st->def || plats_arena != a || a->frame < plats_frame) { build_plats(a->st); plats_arena = a; }
  plats_frame = a->frame;

  if (--b->retarget_t <= 0 || b->target < 0 || !alive(&a->f[b->target])) {
    b->target = pick_target(b, a, me);
    b->retarget_t = 120 + (int)(rf(b) * 120);
  }
  if (b->target < 0) { b->last = 0; return 0; }
  const TrioPlayer *p = &m->p, *t = &a->f[b->target].p;
  if (m->kind == FK_BOSS) return boss_think(b, a, m, t);

  //Where to go: the target, or an item when one is near or it is hurt.
  int gx = t->x, gy = t->y;
  b->goal_item = -1;
  int best_item = 1 << 30;
  for (int k = 0; k < ARENA_ITEMS; k++) {
    const ArenaItem *it = &a->items[k];
    if (it->kind == IT_NONE) continue;
    bool want = it->kind == IT_HEART || (it->kind != IT_WEAPON && p->hp < m->max_hp * 2 / 3) || (it->kind == IT_WEAPON && a->rules.weapons);
    int d = abs(it->x - p->x) + abs(it->y - p->y);
    if (want && d < 260 && d < best_item) { best_item = d; b->goal_item = k; gx = it->x; gy = it->y - 17; }
  }

  //No hits for a long time and the target out of reach: roam to a
  //floor this bot can get to and look for someone else.
  if (b->wander_t > 0) {
    b->wander_t--;
    if (b->goal_item < 0) { gx = b->wander_x; gy = b->wander_y; }
  } else if (p->ground && b->idle_fire_t > 480 && b->goal_item < 0 && a->frame % 30 == 0) {
    int here = plat_at(p->x, p->y + 17), there = plat_at(gx, gy + 17);
    if (here >= 0) {
      int prev[MAXPLAT], q[MAXPLAT];
      int n = flood(here, prev, q);
      if (there < 0 || prev[there] == -2) {
        const Plat *w = &plats[q[(int)(rf(b) * n) % n]];
        b->wander_x = w->x0 + (int)(rf(b) * (w->x1 - w->x0));
        b->wander_y = w->y - 17;
        b->wander_t = 240;
        b->retarget_t = 0;
      }
    }
  }

  int dx = t->x - p->x, dy = t->y - p->y;
  int dir = dx >= 0 ? 1 : -1, adx = abs(dx);
  bool facing_target = p->facing_right == (dir > 0);

  if (b->jump_hold > 0) { b->jump_hold--; pad |= PAD_B; }
  if (b->dash_cd > 0) b->dash_cd--;
  if (b->tap_cd > 0) b->tap_cd--;
  if (b->weapon_cd > 0) b->weapon_cd--;
  bool can_jump = p->ground && !(b->last & PAD_B);

  //Dodge incoming shots by jumping.
  if (threatened(a, me)) b->threat_t++;
  else b->threat_t = 0;
  if (b->threat_t == L->react && can_jump && rf(b) < L->dodge) { pad |= PAD_B; b->jump_hold = 14; }

  //Navigation: the next platform toward the goal.
  int here = plat_at(p->x, p->y + 17), there = plat_at(gx, gy + 17), final = -1;
  int next = p->ground ? next_plat(here, there, gx, gy, &final) : -1;
  //On the goal's platform, or on the nearest one to it that can be reached.
  bool same = here >= 0 && (here == there || here == final);
  if (getenv("ABOT_LOG") && a->frame % 60 == 0) {
    fprintf(stderr, "bot %d f%d x%d y%d g%d act%02X here %d there %d next %d final %d same %d goal %d,%d",
            me, a->frame, p->x, p->y, p->ground, p->action, here, there, next, final, same, gx, gy);
    if (here >= 0) fprintf(stderr, " [%d..%d @%d]", plats[here].x0, plats[here].x1, plats[here].y);
    if (next >= 0) fprintf(stderr, " -> [%d..%d @%d] r%d", plats[next].x0, plats[next].x1, plats[next].y, here >= 0 ? reach[here][next] : -1);
    fprintf(stderr, "\n");
  }
  uint16_t move = 0;
  if (b->climb_side && (p->ground && b->climb_t > 20)) {
    //Landed: the climb is over, and counts as failed off its floor.
    if (here != b->climb_to && b->climb_from >= 0 && ++climb_from[b->climb_from][b->climb_to] >= 3) reach[b->climb_from][b->climb_to] = 0;
    b->climb_side = 0;
  }
  if (b->climb_side && ++b->climb_t > 300) {
    //A climb that doesn't get there: after a few tries nobody uses it.
    if (b->climb_from >= 0 && ++climb_from[b->climb_from][b->climb_to] >= 3) reach[b->climb_from][b->climb_to] = 0;
    b->climb_side = 0;
  }
  if (b->ladder_dir && (++b->ladder_t > 400 || (p->ground && b->ladder_t > 10))) b->ladder_dir = 0;
  if (b->ladder_dir) {
    //On a ladder: hold the way to the other floor until standing on it.
    move = b->ladder_dir < 0 ? PAD_UP : PAD_DOWN;
    if (abs(p->x - b->ladder_x) > 3 && b->ladder_t < 4) move |= b->ladder_x > p->x ? PAD_RIGHT : PAD_LEFT;
  } else if (b->climb_side && !p->ground) {
    //Wall climb: hold toward the wall, kick off it on every slide, and
    //steer onto the floor once above it.
    const Plat *B = &plats[b->climb_to];
    if (p->y + 17 < B->y - 6) {
      int bc = (B->x0 + B->x1) / 2;
      move = bc > p->x ? PAD_RIGHT : PAD_LEFT;
    } else {
      move = b->climb_side > 0 ? PAD_RIGHT : PAD_LEFT;
      if (p->action == TRIO_ACT_WALLSLIDE && !(b->last & PAD_B)) { pad |= PAD_B; b->jump_hold = 6; }
    }
  } else if (!p->ground) {
    //Airborne: keep steering the way the jump was meant to go, but let go
    //of a wall unless the goal is above.
    if (b->air_dir) move = b->air_dir > 0 ? PAD_RIGHT : PAD_LEFT;
    if (p->action == TRIO_ACT_WALLSLIDE && (gy > p->y - 24 || !open_above(m->s, p->x, p->y, gy))) move = 0;
  } else if (next >= 0 && next != here && reach[here][next] == 4) {
    int cx = climb_x[here][next];
    if (abs(p->x - cx) > 3) move = cx > p->x ? PAD_RIGHT : PAD_LEFT;
    else {
      b->ladder_dir = plats[next].y < plats[here].y ? -1 : 1;
      b->ladder_x = cx;
      b->ladder_t = 0;
      move = b->ladder_dir < 0 ? PAD_UP : PAD_DOWN;
    }
  } else if (next >= 0 && next != here && reach[here][next] == 3) {
    int cx = climb_x[here][next];
    if (abs(p->x - cx) > 4) move = cx > p->x ? PAD_RIGHT : PAD_LEFT;
    else if (can_jump) {
      pad |= PAD_B;
      b->jump_hold = 12;
      b->climb_side = climb_side[here][next];
      b->climb_to = next;
      b->climb_from = here;
      b->climb_t = 0;
      move = b->climb_side > 0 ? PAD_RIGHT : PAD_LEFT;
    }
  } else if (next >= 0 && next != here) {
    const Plat *A = &plats[here], *B = &plats[next];
    if (reach[here][next] == 1) {
      //Walk off the edge on B's side.
      int side = (B->x0 + B->x1) / 2 > (A->x0 + A->x1) / 2 ? 1 : -1;
      move = side > 0 ? PAD_RIGHT : PAD_LEFT;
      b->air_dir = side;
    } else {
      //Jump: from beside B when it is overhead (floors are solid from
      //below), else from A's edge toward it.
      int bc = (B->x0 + B->x1) / 2, tx;
      bool over = B->x0 < A->x1 && B->x1 > A->x0;
      if (over) {
        int lft = B->x0 - 14, rgt = B->x1 + 14;
        bool lok = lft >= A->x0 && lft <= A->x1, rok = rgt >= A->x0 && rgt <= A->x1;
        tx = lok && (!rok || abs(p->x - lft) < abs(p->x - rgt)) ? lft : rok ? rgt : (p->x < bc ? A->x0 + 8 : A->x1 - 8);
      } else {
        tx = bc > p->x ? A->x1 - 10 : A->x0 + 10;
      }
      int side = bc > tx ? 1 : -1;
      if (abs(p->x - tx) > 6) move = tx > p->x ? PAD_RIGHT : PAD_LEFT;
      else if (can_jump) {
        pad |= PAD_B;
        b->jump_hold = 18;
        b->air_dir = side;
        move = side > 0 ? PAD_RIGHT : PAD_LEFT;
        //Dash first across a wide gap.
        int gap = B->x0 > A->x1 ? B->x0 - A->x1 : A->x0 > B->x1 ? A->x0 - B->x1 : 0;
        if (gap > 40) pad |= PAD_A;
      }
    }
  } else if (b->goal_item >= 0 && !same) {
    //Same floor as the goal (or no way found): go straight there.
    if (abs(gx - p->x) > 6) move = gx > p->x ? PAD_RIGHT : PAD_LEFT;
    b->air_dir = gx > p->x ? 1 : -1;
    if (next < 0 && dy < -40 && can_jump && rf(b) < 0.05f) { pad |= PAD_B; b->jump_hold = 18; }
  } else {
    //Fighting on the same floor: spacing, approach when far, back off
    //when close (unless cornered).
    int want = L->spacing;
    if (b->idle_fire_t > 200) want = 36;
    const Plat *A = &plats[here];
    bool cornered = (dir < 0 && p->x > A->x1 - 40) || (dir > 0 && p->x < A->x0 + 40);
    //A shot leaves the blaster about 20 px ahead: closer than CLOSE it comes
    //out behind the target, so two bots on one spot would never hit.
    int close = want - 40 > CLOSE ? want - 40 : CLOSE;
    if (adx > want + 30) move = dir > 0 ? PAD_RIGHT : PAD_LEFT;
    else if (adx < close && !cornered) move = dir > 0 ? PAD_LEFT : PAD_RIGHT;
    else if (!facing_target) move = dir > 0 ? PAD_RIGHT : PAD_LEFT;
    //Don't back off over an edge.
    if (move == (dir > 0 ? PAD_LEFT : PAD_RIGHT) && ((dir > 0 && p->x < A->x0 + 20) || (dir < 0 && p->x > A->x1 - 20))) move = 0;
    if (cornered && adx < 50 && can_jump && rf(b) < 0.08f) { pad |= PAD_B; b->jump_hold = 16; b->air_dir = dir; move = dir > 0 ? PAD_RIGHT : PAD_LEFT; }
    if (p->ground && b->dash_cd == 0 && adx > 140 && rf(b) < 0.03f + 0.02f * m->level) { pad |= PAD_A; b->dash_cd = 50; }
  }
  //Never walk off into a pit or onto spikes: on the ground, a step toward
  //a column with no safe floor under it (down to the bottom of the box)
  //is dropped unless it is a jump.
  if (p->ground && (move & (PAD_LEFT | PAD_RIGHT)) && !(pad & PAD_B) && !b->climb_side) {
    int ahead = p->x + (move & PAD_RIGHT ? 14 : -14);
    if (!safe_below(&a->st->geo, a->st->def, ahead, p->y + 17)) move &= (uint16_t)~(PAD_LEFT | PAD_RIGHT | PAD_A);
  }
  pad |= move;

  //Stuck against something while walking: jump (and wall-kick).
  if (p->ground && (move & (PAD_LEFT | PAD_RIGHT)) && abs(p->x - b->last_x) == 0) b->stuck_t++;
  else b->stuck_t = 0;
  b->last_x = p->x;
  if (b->stuck_t > 10 && !(b->last & PAD_B)) { pad |= PAD_B; b->jump_hold = 18; b->stuck_t = 0; }
  //Wall-kick up only when the goal is above.
  if (!b->climb_side && !p->ground && p->action == TRIO_ACT_WALLSLIDE && gy < p->y - 24 && !(b->last & PAD_B) && rf(b) < 0.3f &&
      open_above(m->s, p->x, p->y, gy)) pad |= PAD_B;

  //Shooting. The line of fire is checked against the arena at the blaster's
  //height, so cover blocks it.
  bool open = clear_line(m->s, p->x, t->x, p->y - 4);
  bool lined = abs(dy) < (p->ground ? 26 : 40) && facing_target && adx < 360 && open;
  if (getenv("ABOT_LOG") && a->frame % 60 == 30)
    fprintf(stderr, "bot %d fire: tgt %d dx %d dy %d open %d facing %d lined %d hold %d idle %d\n", me, b->target, dx, dy, open, facing_target, lined, b->holding_y, b->idle_fire_t);
  if (m->dealt != b->last_dealt) { b->last_dealt = m->dealt; b->idle_fire_t = 0; }
  b->idle_fire_t++;
  if (b->charge_cd > 0) b->charge_cd--;
  float aggro = L->aggro + (b->idle_fire_t > 240 ? 0.3f : 0);
  //Blade's sword (X) when the target is in reach (the jump slash in the air).
  if (m->kind == FK_BLADE && adx < 44 && dy > (p->ground ? -30 : -50) && dy < 30 && facing_target && rf(b) < 0.25f + 0.1f * m->level && !(b->last & PAD_X)) pad |= PAD_X;
  if (b->holding_y) {
    b->holding_y++;
    bool ready = b->holding_y >= b->charge_want;
    if (!((ready && lined) || b->holding_y > 170)) pad |= PAD_Y;
    else { b->holding_y = 0; b->charge_cd = 40; }
  } else if (lined && b->tap_cd == 0 && rf(b) < aggro) {
    if (b->charge_cd == 0 && rf(b) < L->charge * 0.5f) {
      b->holding_y = 1;
      b->charge_want = rf(b) < 0.6f ? 40 : 90;
      pad |= PAD_Y;
    } else if (!(b->last & PAD_Y)) {
      pad |= PAD_Y;
      b->tap_cd = 7;
    }
  } else if (!lined && b->charge_cd == 0 && rf(b) < 0.004f * m->level) {
    b->holding_y = 1;
    b->charge_want = 90;
    pad |= PAD_Y;
  }

  //HERO FULL's specials: the Fireball at a target in line ahead, the
  //Uppercut at one close by and level or above. The motion takes over the
  //pad for a few frames.
  if (m->specials && m->kind == FK_X && b->seq_n == 0 && p->ground && facing_target && m->sp_move == ARENA_SP_NONE) {
    uint16_t fwd = p->facing_right ? PAD_RIGHT : PAD_LEFT;
    float r = rf(b);
    if (adx > 60 && adx < 220 && abs(dy) < 20 && open && r < 0.01f + 0.005f * m->level) {
      b->seq[0] = PAD_DOWN;
      b->seq[1] = PAD_DOWN | fwd;
      b->seq[2] = fwd;
      b->seq[3] = fwd | PAD_Y;
      b->seq_n = 4;
    } else if (adx < 40 && dy < 8 && dy > -60 && r < 0.03f + 0.01f * m->level) {
      b->seq[0] = fwd;
      b->seq[1] = PAD_DOWN;
      b->seq[2] = PAD_DOWN | fwd;
      b->seq[3] = PAD_DOWN | fwd | PAD_Y;
      b->seq_n = 4;
    }
    b->seq_i = b->seq_t = 0;
  }
  if (b->seq_n) {
    pad = b->seq[b->seq_i];
    //Y only on the first frame of the last step (a press).
    if (b->seq_i == 3 && b->seq_t > 0) pad &= (uint16_t)~PAD_Y;
    if (++b->seq_t >= 3) { b->seq_t = 0; if (++b->seq_i >= b->seq_n) b->seq_n = 0; }
    b->last = pad;
    return pad;
  }

  //Weapons: now and then another weapon (back to the blaster when dry).
  if (a->rules.weapons && m->kind == FK_X && !b->holding_y && b->weapon_cd == 0) {
    int w = trio_weapon(m->s);
    bool dry = w > 0 && trio_weapon_energy(m->s, w) < 2;
    if ((dry || rf(b) < 0.006f) && !(b->last & (PAD_L | PAD_R))) {
      pad |= rf(b) < 0.5f ? PAD_R : PAD_L;
      b->weapon_cd = 30;
    }
  }
  b->last = pad;
  return pad;
}
