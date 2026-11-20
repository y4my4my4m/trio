#include "boss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/recomp_rt.h"

//Each boss runs in a copy of its own game: the enemy table, enemy shots
//and player are that game's (TrioGame); the boss goes in the first enemy slot.
static uint32_t boss_slot(const Snes *s) { return trio_game(s)->enemy_first; }

//Tables the stage fills in WRAM that a boss reads: G3's object graphics
//($7F:8000, by object +$10; its frames are streamed from there, 04:BCC0),
//G1's palette slots ($7F:83xx; G1 Raider's armor takes +$11 from $7F:8349,
//83:E860). Carried from the source room into the fighter copy.
static void stage_tab(int game, uint32_t *at, uint32_t *len) {
  *at = game == SNES_GAME_G3 ? 0x18000 : game == SNES_GAME_G1 ? 0x18300 : 0;
  *len = game == SNES_GAME_G3 ? 0x200 : game == SNES_GAME_G1 ? 0x100 : 0;
}

//Object fields used here (enemies share the object layout; see trio.h).
enum {
  B_STATE = 0x01,          //BossDef.fight while fighting
  B_PATTERN = 0x02,        //attack pattern
  B_STEP = 0x03,           //step within it
  B_ID = 0x0A,
  B_FLAGS = 0x11,          //bit 6 facing right, bit 3 hit flash
  B_HP = 0x27,
  B_HIT = 0x28,            //BossDef.hit on a hit
};

enum { ROUTE_DOOR, ROUTE_WARLORD, ROUTE_G1_RAIDER, ROUTE_G1_WARLORD, ROUTE_G2_WARLORD, ROUTE_G1_RAIDER_ARMOR };

typedef struct BossDef {
  const char *name;
  int game;                //SNES_GAME_*
  int route;               //how the source copy gets into the boss's room
  struct { int stage, x, floor, cam_y, glide_y; bool no_glide; } room;   //where the route starts
  int door_walk;           //frames of walking right that take the hero through the door
  int pre_id;              //object of a first phase to cut short (0 = none)
  int id;                  //the boss's object in its fight loop
  int fight;               //state (+$01) of its fight loop
  int idle, idle2;         //patterns the AI waits in between moves (idle2: another, -1 none)
  int idle_step;           //the wait is this step of the idle pattern (-1: any)
  int chooser;             //pattern that picks the next move at once (-1 none): put back to the wait
  int banned;              //a move that doesn't work in an arena (-1 none): put back to the wait too
  int iframes_off, iframes;   //invincibility counter after a hit, and its start value
  int hit;                 //+$28 on a hit
  int pattern[4];          //pattern per TRIO_MOVE_*
  int feet;                //floor y - object y while standing
  int hp_scale;            //HP per fighter HP (bosses have 32, fighters 16)
  int taken;               //percent of the damage taken (balance: 100 = a fighter's)
  //Walking. Neither boss walks in its fight and neither has walking frames,
  //so a walk steps through its standing frames (+$17 picks the frame).
  uint8_t walk_frames[4];
  int nwalk;
  int walk_speed;          //1/256 px per frame
  //The controller between attacks (boss.c, control): body half width and
  //height for collision, jump speed (1/256 px per frame), and the frames
  //(+$17) shown rising and falling (0: the wait's own).
  int half_w, height, jump_speed;
  uint8_t f_rise, f_fall;
} BossDef;

static const BossDef defs[TRIO_BOSS_COUNT] = {
  [TRIO_BOSS_RAIDER] = {
    //Raider's stage (14): the corridor before his door, then his room at
    //x 4880-5360. The walker (object $63) comes first; on foot
    //he is object $61: pattern 0 jumps at the target firing, 2 rises and
    //fires a spread, 6 rises and fires one shot, 4 waits.
    .name = "RAIDER",
    .idle_step = -1, .chooser = -1, .banned = -1,
    .game = SNES_GAME_G3,
    .fight = 2, .idle2 = -1, .iframes_off = 0x37, .iframes = 0x3C, .hit = 1,
    .room = {.stage = 14, .x = 4408, .floor = 704, .cam_y = 520, .glide_y = 16},
    .door_walk = 1400,
    .pre_id = 0x63,
    .id = 0x61,
    .idle = 4,
    .pattern = {[TRIO_MOVE_JUMP] = 0, [TRIO_MOVE_SHOT] = 6, [TRIO_MOVE_SPECIAL] = 2},
    .feet = 25,
    .hp_scale = 2,
    .taken = 100,
    .walk_frames = {0x00, 0x01, 0x02, 0x03},
    .nwalk = 4,
    .walk_speed = 0x140,
    .half_w = 12, .height = 44, .jump_speed = 0x580, .f_rise = 0x04, .f_fall = 0x06,
  },
  [TRIO_BOSS_WARLORD] = {
    //Warlord's stage (13) is a shaft climbed from the start; his room is above
    //the hole at x 1888-1936 in its floor (y 2752). His first form is
    //object $5E: pattern 0 waits, 2 fires a barrage of cannon shots, 4 is
    //his jumping attack.
    .name = "WARLORD",
    .idle_step = -1, .chooser = -1, .banned = -1,
    .game = SNES_GAME_G3,
    .fight = 2, .idle2 = -1, .iframes_off = 0x37, .iframes = 0x3C, .hit = 1,
    .route = ROUTE_WARLORD,
    .room = {.stage = 13, .x = 384, .floor = 3776, .cam_y = 3584, .no_glide = true},
    .id = 0x5E,
    .idle = 0,
    .pattern = {[TRIO_MOVE_JUMP] = 4, [TRIO_MOVE_SHOT] = 2, [TRIO_MOVE_SPECIAL] = 4},
    .feet = 33,
    .hp_scale = 2,
    .taken = 100,
    .walk_frames = {0x00, 0x13},
    .nwalk = 2,
    .walk_speed = 0x120,
    .half_w = 14, .height = 60, .jump_speed = 0x5C0, .f_rise = 0x81, .f_fall = 0x81,
  },
  [TRIO_BOSS_G1_RAIDER] = {
    //Warlord fortress 1 (stage 09). With $1F7D = 2 (the game's retry path)
    //his walker deletes itself and he spawns on foot (object $69): state
    //4 is his fight; pattern 4 waits, 0 jumps at the target (+$38 = 1:
    //stops at the top and drops fire bombs), 2 fires his shoulder cannon, 6
    //dashes into a kick.
    .name = "G1 RAIDER",
    .idle_step = -1, .chooser = -1, .banned = -1,
    .game = SNES_GAME_G1,
    .route = ROUTE_G1_RAIDER,
    .room = {.stage = 9},
    .id = 0x69,
    .fight = 4, .idle = 4, .idle2 = -1, .iframes_off = 0x3A, .iframes = 0x18, .hit = 5,
    .pattern = {[TRIO_MOVE_JUMP] = 0, [TRIO_MOVE_SHOT] = 2, [TRIO_MOVE_SPECIAL] = 6},
    .feet = 21,
    .hp_scale = 2,
    .taken = 75,
    .walk_speed = 0x140,
    .half_w = 12, .height = 40, .jump_speed = 0x580,
  },
  [TRIO_BOSS_G1_RAIDER_ARMOR] = {
    //Warlord fortress 1 the first time ($1F7D clear): Raider in his walker,
    //object $67, state 2. Pattern 0 picks the next move the frame it starts
    //(put back to the wait, $0A, a short standing pose); 4 walks at the
    //target, 2 punches, 6 jumps (then $10 in the air, $12 the landing), $0C
    //charges across the room ($0E, $18 after it).
    .name = "G1 RAIDER ARMOR",
    .idle_step = -1, .chooser = 0, .banned = -1,
    .game = SNES_GAME_G1,
    .route = ROUTE_G1_RAIDER_ARMOR,
    .room = {.stage = 9},
    .id = 0x67,
    .fight = 2, .idle = 0x12, .idle2 = -1, .iframes_off = 0x3A, .iframes = 0x18, .hit = 5,
    .pattern = {[TRIO_MOVE_JUMP] = 6, [TRIO_MOVE_SHOT] = 2, [TRIO_MOVE_SPECIAL] = 0x0C},
    .feet = 34,
    .hp_scale = 8,
    .taken = 100,
    .walk_speed = 0x100,
    .half_w = 18, .height = 56, .jump_speed = 0x500,
  },
  [TRIO_BOSS_G1_WARLORD] = {
    //Warlord fortress 4 (stage 0C): his sword form, object $65. State $0A is
    //his fight; patterns 8 (guard) and $0E wait, 0 dashes (a slash on
    //contact), 2 jumps to the wall and dives, 4 fires his blaster twice, 6
    //slashes (the green arc).
    .name = "G1 WARLORD",
    .idle_step = -1, .chooser = -1, .banned = -1,
    .game = SNES_GAME_G1,
    .route = ROUTE_G1_WARLORD,
    .room = {.stage = 12},
    .id = 0x65,
    .fight = 0x0A, .idle = 8, .idle2 = 0x0E, .iframes_off = 0x35, .iframes = 0x3C, .hit = 5,
    //(2, the wall jump, needs his room's walls.)
    .pattern = {[TRIO_MOVE_JUMP] = 0, [TRIO_MOVE_SHOT] = 4, [TRIO_MOVE_SPECIAL] = 6},
    .feet = 33,
    .hp_scale = 2,
    .taken = 60,
    .walk_speed = 0x140,
    .half_w = 14, .height = 56, .jump_speed = 0x5C0,
  },
  [TRIO_BOSS_G2_WARLORD] = {
    //G2's final stage (stage 5 with $1FAE = 4): at the bottom of the long
    //shaft, object $67. State 2 is his fight. Pattern 0 picks his next move
    //the frame it starts, so it is put back to his wait: step 6 of pattern
    //$0A (after the energy balls), a standing pose timed by +$34. 2 is the
    //claw dash (a slash when close), 6 the energy beam, 8 the jump to the
    //wall and the drop, $0A the spread of energy balls.
    .name = "G2 WARLORD",
    .idle_step = 6, .chooser = 0, .banned = 8,
    .game = SNES_GAME_G2,
    .route = ROUTE_G2_WARLORD,
    .room = {.stage = 5 | TRIO_STAGE_SUB(4)},
    .id = 0x67,
    .fight = 2, .idle = 0x0A, .idle2 = -1, .iframes_off = 0x37, .iframes = 0x3C, .hit = 1,
    //(8 needs his room's walls: in an arena it flies off the top. The game
    //chains into it after a slash, so it is banned.)
    .pattern = {[TRIO_MOVE_JUMP] = 2, [TRIO_MOVE_SHOT] = 0x0A, [TRIO_MOVE_SPECIAL] = 6},
    .feet = 32,
    .hp_scale = 2,
    .taken = 90,
    .walk_speed = 0x140,
    .half_w = 14, .height = 56, .jump_speed = 0x5C0,
  },
};

static bool is_idle(const BossDef *d, int pattern) { return pattern == d->idle || pattern == d->idle2; }
//In its wait (the idle pattern, at the wait's step).
static bool in_wait(const BossDef *d, const uint8_t *o) {
  return is_idle(d, o[B_PATTERN]) && (d->idle_step < 0 || o[B_STEP] == d->idle_step);
}

//The source each kind was last installed from: its tiles are put back every
//frame (the arena stage's own loaders write into the OBJ tables).
static const TrioBossSrc *installed[TRIO_BOSS_COUNT];

static inline uint16_t rd16(const Snes *s, uint32_t a) { return (uint16_t)(s->wram[a] | s->wram[a + 1] << 8); }
static inline void wr16(Snes *s, uint32_t a, uint16_t v) { s->wram[a] = (uint8_t)v; s->wram[a + 1] = (uint8_t)(v >> 8); }

const char *trio_boss_name(int kind) { return kind > 0 && kind < TRIO_BOSS_COUNT ? defs[kind].name : ""; }

static const BossDef *def_of(const Snes *s) {
  uint32_t b = boss_slot(s);
  int id = s->wram[b + B_ID];
  for (int k = 1; k < TRIO_BOSS_COUNT; k++)
    if (s->wram[b] && defs[k].id == id && defs[k].game == trio_game(s)->id) return &defs[k];
  return NULL;
}

int trio_boss_game(int kind) { return kind > 0 && kind < TRIO_BOSS_COUNT ? defs[kind].game : 0; }

int trio_boss_kind(const Snes *s) {
  const BossDef *d = def_of(s);
  return d ? (int)(d - defs) : TRIO_BOSS_NONE;
}

//---------------------------------------------------------------- source

//Camera straight to (tx, ty), 8 px a frame, with the player carried at the
//top of the level. Only the scroll limits are written: the spawner compares
//the camera with its previous position ($1E7A / $1E7C), which the game keeps.
static void cam_glide(Snes *s, int tx, int ty) {
  int cx = rd16(s, 0x1E5D), cy = rd16(s, 0x1E60);
  while (cx != tx || cy != ty) {
    cx += cx < tx ? (tx - cx > 8 ? 8 : tx - cx) : -(cx - tx > 8 ? 8 : cx - tx);
    cy += cy < ty ? (ty - cy > 8 ? 8 : ty - cy) : -(cy - ty > 8 ? 8 : cy - ty);
    wr16(s, 0x09D8 + 0x05, (uint16_t)(cx + 128));
    wr16(s, 0x09D8 + 0x08, 16);
    static const uint32_t xl[] = {0x1E66, 0x1E6E, 0x1E68, 0x1E70}, yl[] = {0x1E6A, 0x1E6C, 0x1E78, 0x1E7E};
    for (int i = 0; i < 4; i++) { wr16(s, xl[i], (uint16_t)cx); wr16(s, yl[i], (uint16_t)cy); }
    wr16(s, 0x1E5D, (uint16_t)cx);
    wr16(s, 0x00B5, (uint16_t)cx);
    wr16(s, 0x1E60, (uint16_t)cy);
    s->wram[0x09D8 + 0x27] = 16;
    s->pad[0] = 0;
    snes_run_frame(s);
  }
}

static void put_player(Snes *s, int x, int y) {
  const uint32_t p = trio_game(s)->player;
  wr16(s, p + 0x05, (uint16_t)x);
  wr16(s, p + 0x22, (uint16_t)x);
  wr16(s, p + 0x08, (uint16_t)y);
  wr16(s, p + 0x24, (uint16_t)y);
  wr16(s, p + 0x1C, 0);
}

//Warlord's room the way the game builds it: the camera up the shaft into the
//room, the room's area objects (category 2, table $1D18, 0x10 each: the
//camera zone $00/$06 and the two at the hole), then the hero through the hole and
//back with the camera free to follow. The spawner then brings Warlord in with
//his graphics.
static void route_warlord(Snes *s) {
  cam_glide(s, 1784, 3100);
  cam_glide(s, 1784, 2568);
  put_player(s, 1840, 2735);
  static const struct { uint8_t id, sub; uint16_t x, y; } area[] = {
    {0x00, 0x06, 1920, 2688}, {0x1A, 0x10, 1920, 2816}, {0x18, 0x21, 1912, 2816},
  };
  for (size_t k = 0; k < sizeof(area) / sizeof(area[0]); k++) {
    uint32_t e = 0x1D18;
    while (s->wram[e] && e < 0x1E18) e += 0x10;
    memset(s->wram + e, 0, 0x10);
    s->wram[e + 0x0A] = area[k].id;
    s->wram[e + 0x0B] = area[k].sub;
    wr16(s, e + 0x05, area[k].x);
    wr16(s, e + 0x08, area[k].y);
    s->wram[e] = 1;
  }
  for (int i = 0; i < 20; i++) { s->wram[0x09D8 + 0x27] = 16; s->pad[0] = 0; snes_run_frame(s); }
  wr16(s, 0x1E6A, 0); wr16(s, 0x1E6C, 0x1F00); wr16(s, 0x1E78, 0); wr16(s, 0x1E7E, 0x1F00);
  for (int i = 0; i < 40; i++) { put_player(s, 1912, 2840); s->wram[0x09D8 + 0x27] = 16; s->pad[0] = 0; snes_run_frame(s); }
  for (int y = 2840; y >= 2735; y -= 4) { put_player(s, 1912, y); s->wram[0x09D8 + 0x27] = 16; s->pad[0] = 0; snes_run_frame(s); }
  put_player(s, 1840, 2735);
}

//G1: the player and the camera to (x, y). set_limits: the scroll limits
//(any game).
static void g1_tp(Snes *s, int x, int y) {
  const TrioGame *g = trio_game(s);
  put_player(s, x, y);
  int cx = x > 128 ? x - 128 : 0, cy = y > 112 ? y - 112 : 0;
  wr16(s, g->cam_x, (uint16_t)cx);
  wr16(s, g->cam_y, (uint16_t)cy);
  wr16(s, g->cam_px, (uint16_t)cx);
  wr16(s, g->cam_py, (uint16_t)cy);
}

static void set_limits(Snes *s, int xmin, int xmax, int ymin, int ymax) {
  const TrioGame *g = trio_game(s);
  for (int i = 0; i < 2; i++) { wr16(s, g->lim_xmin[i], (uint16_t)xmin); wr16(s, g->lim_xmax[i], (uint16_t)xmax); }
  for (int i = 0; i < g->nlim_y; i++) wr16(s, g->lim_y[i], (uint16_t)(i % 2 ? ymax : ymin));
}

static void step(Snes *s, uint16_t pad) {
  s->wram[trio_game(s)->player + 0x27] = 16;
  s->pad[0] = pad;
  snes_run_frame(s);
}

static uint32_t find_obj(const Snes *s, int id);

//G1 Raider: through the first shutter of fortress 1 and its cutscene room,
//then, with $1F7D = 2 (Raider's retry path: no walker), over the block
//and through the second shutter into his room; A pages the dialogues.
static void route_g1_raider(Snes *s, bool armor) {
  g1_tp(s, 2120, 1439);
  set_limits(s, 1990, 2600, 1327, 1327);
  for (int fr = 0; fr < 250; fr++) step(s, PAD_RIGHT);
  for (int fr = 0; fr < 300; fr++) step(s, fr % 20 < 2 ? PAD_A : 0);
  if (!armor) s->wram[0x1F7D] = 2;
  for (int fr = 0; fr < 3000; fr++) {
    uint32_t v = find_obj(s, armor ? 0x67 : 0x69);
    if (v && s->wram[v + B_STATE] == (armor ? 2 : 4)) return;
    uint16_t pad = v ? (fr % 20 < 2 ? PAD_A : 0) : (uint16_t)(PAD_RIGHT | (fr % 30 < 10 ? PAD_B : 0));
    step(s, pad);
  }
}

//G1 Warlord: up fortress 4's shaft to the top, where his object builds the
//room; the guard (object $26) is cut to 1 HP and shot, and A pages the
//dialogues until Warlord fights.
static void route_g1_warlord(Snes *s) {
  const uint32_t p = trio_game(s)->player;
  for (int y = rd16(s, p + 0x08); y > 270; y -= 24)
    for (int i = 0; i < 4; i++) { put_player(s, 128, y); step(s, 0); }
  for (int i = 0; i < 30; i++) { put_player(s, 128, 180); step(s, 0); }
  bool shot = false;
  for (int fr = 0; fr < 4000; fr++) {
    uint32_t sg = find_obj(s, 0x65), dog = find_obj(s, 0x26);
    if (sg && s->wram[sg + B_STATE] == 0x0A) return;
    uint16_t pad = fr % 20 < 2 ? PAD_A : 0;
    if (dog && s->wram[dog + B_STATE] == 4 && !shot) {
      s->wram[dog + 0x27] = 1;
      //Beside it, facing it, an uncharged shot.
      int dx = rd16(s, dog + 0x05);
      put_player(s, dx > 128 ? dx - 48 : dx + 48, rd16(s, p + 0x08));
      if (dx > 128) s->wram[p + 0x11] |= 0x40; else s->wram[p + 0x11] &= (uint8_t)~0x40;
      pad = fr % 8 < 2 ? PAD_Y : 0;
      if (!s->wram[dog]) shot = true;
    }
    if (dog && s->wram[dog + B_STATE] >= 6) shot = true;
    step(s, pad);
  }
}

//G2 Warlord: Blade's spawn in the final stage is marked done (his intro and
//Warlord's would run together), then the camera glides along the top and
//down the shaft with the hero carried; only the scroll limits move it, so the
//spawner sees it scroll. the hero is put in Warlord's room and A pages his intro.
static void route_g2_warlord(Snes *s) {
  const TrioGame *g = trio_game(s);
  for (uint32_t a = 0xFA02; a + 5 <= 0xFF00; a += 5)
    if (rd16(s, a + 3) == 0xE704) { s->wram[a] = 1; break; }
  static const int pts[3][2] = {{3200, 224}, {3584, 224}, {3584, 3328}};
  for (int k = 0; k < 3; k++) {
    int cx = rd16(s, g->cam_x), cy = rd16(s, g->cam_y);
    while (cx != pts[k][0] || cy != pts[k][1]) {
      int dx = pts[k][0] - cx, dy = pts[k][1] - cy;
      cx += dx > 8 ? 8 : dx < -8 ? -8 : dx;
      cy += dy > 8 ? 8 : dy < -8 ? -8 : dy;
      put_player(s, cx + 128, cy + 16);
      set_limits(s, cx, cx, cy, cy);
      step(s, 0);
    }
  }
  for (int i = 0; i < 4; i++) { put_player(s, 3633, 3503); step(s, 0); }
  for (int fr = 0; fr < 4000; fr++) {
    uint32_t sg = find_obj(s, 0x67);
    if (sg && s->wram[sg + B_STATE] == 2) return;
    step(s, fr % 20 < 2 ? PAD_A : 0);
  }
}

//The enemy slot holding object `id`, or 0.
static uint32_t find_obj(const Snes *s, int id) {
  const TrioGame *g = trio_game(s);
  for (uint32_t a = g->enemy_first; a < g->enemy_end; a += TRIO_OBJ_SIZE)
    if (s->wram[a] && s->wram[a + B_ID] == id) return a;
  return 0;
}

bool trio_boss_source(TrioBossSrc *src, int kind, const uint8_t *rom, uint32_t rom_size, bool recomp) {
  memset(src, 0, sizeof(*src));
  if (kind <= 0 || kind >= TRIO_BOSS_COUNT) return false;
  const BossDef *d = &defs[kind];
  static Snes s;
  bool ok = trio_boot_stage(&s, rom, rom_size, recomp, d->room.stage, NULL) && trio_game(&s)->id == d->game;
  //Routes scripted to the fight; the boss is then taken in its wait.
  bool x1 = d->route == ROUTE_G1_RAIDER || d->route == ROUTE_G1_RAIDER_ARMOR || d->route == ROUTE_G1_WARLORD || d->route == ROUTE_G2_WARLORD;
  if (ok && x1) {
    if (d->route == ROUTE_G1_RAIDER || d->route == ROUTE_G1_RAIDER_ARMOR) route_g1_raider(&s, d->route == ROUTE_G1_RAIDER_ARMOR);
    else if (d->route == ROUTE_G1_WARLORD) route_g1_warlord(&s);
    else route_g2_warlord(&s);
  } else if (ok && !d->room.no_glide) {
    //To the room with the camera, the player carried at the top of the level.
    int cx = trio_cam_x(&s), cy = trio_cam_y(&s), tx = d->room.x - 128, ty = d->room.cam_y;
    while (cx != tx || cy != ty) {
      cx += cx < tx ? (tx - cx > 8 ? 8 : tx - cx) : -(cx - tx > 8 ? 8 : cx - tx);
      cy += cy < ty ? (ty - cy > 8 ? 8 : ty - cy) : -(cy - ty > 8 ? 8 : cy - ty);
      trio_place(&s, cx + 128, d->room.glide_y, true);
      trio_camera(&s, cx, cy);
      trio_set_hp(&s, 16);
      s.pad[0] = 0;
      snes_run_frame(&s);
    }
  }
  if (ok && !x1) {
    trio_place(&s, d->room.x, d->room.floor - 17, true);
    for (int i = 0; i < 30; i++) { trio_camera(&s, d->room.x - 128, d->room.cam_y); trio_quiet(&s); s.pad[0] = 0; snes_run_frame(&s); }
    //trio_camera pins the scroll limits: open them sideways so the camera
    //follows the hero to the door (the spawner brings the door in as it scrolls).
    if (d->route == ROUTE_DOOR) set_limits(&s, 0, 0x1F00, d->room.cam_y, d->room.cam_y);
  }
  if (ok && d->route == ROUTE_WARLORD) route_warlord(&s);
  bool cut = false;
  for (int fr = 0; ok && fr < 6000; fr++) {
    uint32_t slot = find_obj(&s, d->pre_id && !cut ? d->pre_id : d->id);
    const uint8_t *o = s.wram + (slot ? slot : boss_slot(&s));
    s.wram[trio_game(&s)->player + 0x27] = 16;
    if (x1) {
      s.pad[0] = fr % 20 < 2 ? PAD_A : 0;
    } else if (d->route == ROUTE_DOOR) {
      //Through the door (B pages the dialogue), then keep shooting.
      if (fr < d->door_walk) s.pad[0] = fr % 16 < 2 && fr > 350 ? PAD_B : PAD_RIGHT;
      else s.pad[0] = fr % 8 < 2 ? PAD_Y : (fr % 16 < 2 ? PAD_B : 0);
    } else {
      s.pad[0] = fr % 16 < 2 ? PAD_B : 0;  //the dialogue
    }
    //A first phase is cut to one HP.
    if (d->pre_id && !cut && slot && o[B_ID] == d->pre_id && o[B_STATE] == 2) { s.wram[slot + B_HP] = 1; cut = true; }
    snes_run_frame(&s);
    if (getenv("BOSS_LOG") && fr % (getenv("BOSS_LOG_EVERY") ? atoi(getenv("BOSS_LOG_EVERY")) : 100) == 0) {
      TrioPlayer p;
      trio_read_player(&s, &p);
      fprintf(stderr, "boss %s f%d: x %d y %d act %02X hp %d cam %d,%d |", d->name, fr, p.x, p.y, p.action, p.hp, trio_cam_x(&s), trio_cam_y(&s));
      const TrioGame *g = trio_game(&s);
      for (uint32_t e = g->enemy_first; e < g->enemy_end; e += TRIO_OBJ_SIZE)
        if (s.wram[e]) fprintf(stderr, " %02X:s%02X p%02X @%d,%d", s.wram[e + B_ID], s.wram[e + B_STATE], s.wram[e + B_PATTERN], rd16(&s, e + 5), rd16(&s, e + 8));
      fprintf(stderr, "\n");
    }
    slot = find_obj(&s, d->id);
    o = s.wram + (slot ? slot : boss_slot(&s));
    //(G1 clears +$0E bit 0, drawn, while an object is off the camera: the
    //wait is taken while it is drawn.)
    if (slot && o[B_STATE] == d->fight && in_wait(d, o) && (d->game != SNES_GAME_G1 || (o[0x0E] & 1))) {
      src->kind = kind;
      memcpy(src->obj, o, TRIO_OBJ_SIZE);
      uint8_t size;
      ppu_obj_info(&s.ppu, &src->name, &src->sel, &size);
      memcpy(src->tiles[0], s.ppu.vram + ((uint32_t)src->name * 2 & 0xFFFF), 0x2000);
      memcpy(src->tiles[1], s.ppu.vram + ((uint32_t)(src->name + src->sel) * 2 & 0xFFFF), 0x2000);
      memcpy(src->cgram, s.ppu.cgram + 128, sizeof(src->cgram));
      memcpy(src->mirror, s.wram + 0x0400, sizeof(src->mirror));
      uint32_t at, len;
      stage_tab(d->game, &at, &len);
      if (len) memcpy(src->gfx_tab, s.wram + at, len);
      src->ok = true;
      break;
    }
  }
  return src->ok;
}

//---------------------------------------------------------------- fighter

static void put_tiles(Snes *s, const TrioBossSrc *src) {
  memcpy(s->ppu.vram + ((uint32_t)src->name * 2 & 0xFFFF), src->tiles[0], 0x2000);
  memcpy(s->ppu.vram + ((uint32_t)(src->name + src->sel) * 2 & 0xFFFF), src->tiles[1], 0x2000);
}

void trio_boss_install(Snes *s, const TrioBossSrc *src, int x, int floor_y) {
  const uint32_t B = boss_slot(s);
  if (!src->ok) return;
  const BossDef *d = &defs[src->kind];
  installed[src->kind] = src;
  put_tiles(s, src);
  memcpy(s->ppu.cgram + 128, src->cgram, sizeof(src->cgram));
  memcpy(s->wram + 0x0400, src->mirror, sizeof(src->mirror));
  uint32_t at, len;
  stage_tab(d->game, &at, &len);
  if (len) memcpy(s->wram + at, src->gfx_tab, len);
  const TrioGame *g = trio_game(s);
  for (uint32_t a = g->enemy_first; a < g->enemy_end; a += TRIO_OBJ_SIZE) s->wram[a] = 0;
  for (uint32_t a = g->eshot_first; a < g->eshot_end; a += TRIO_OBJ_SIZE) s->wram[a] = 0;
  memcpy(s->wram + B, src->obj, TRIO_OBJ_SIZE);
  wr16(s, B + 0x05, (uint16_t)x);
  wr16(s, B + 0x08, (uint16_t)(floor_y - d->feet));
  s->wram[B + d->iframes_off] = 0;
  if (g->id == SNES_GAME_G3) wr16(s, TRIO_BOSS_METER, B);
  //Camera on it: enemies off the camera delete themselves.
  trio_camera(s, x - 128, floor_y - 160);
}

void trio_boss_place(Snes *s, int x, int floor_y, bool right) {
  const uint32_t B = boss_slot(s);
  const BossDef *d = def_of(s);
  if (!d) return;
  wr16(s, B + 0x05, (uint16_t)x);
  wr16(s, B + 0x08, (uint16_t)(floor_y - d->feet));
  wr16(s, B + 0x1A, 0);
  wr16(s, B + 0x1C, 0);
  if (right) s->wram[B + B_FLAGS] |= 0x40;
  else s->wram[B + B_FLAGS] &= (uint8_t)~0x40;
  trio_camera(s, x - 128, floor_y - 160);
}

void trio_read_boss(const Snes *s, TrioPlayer *p) {
  const uint32_t B = boss_slot(s);
  const uint8_t *o = s->wram + B;
  const BossDef *d = def_of(s);
  memset(p, 0, sizeof(*p));
  p->x = rd16(s, B + 0x05);
  //Same convention as the hero: body center, feet 17 below.
  p->y = (int16_t)rd16(s, B + 0x08) + (d ? d->feet : 17) - 17;
  p->vx = (int16_t)rd16(s, B + 0x1A);
  p->vy = (int16_t)rd16(s, B + 0x1C);
  p->facing_right = (o[B_FLAGS] & 0x40) != 0;
  int scale = d ? d->hp_scale : 1;
  p->hp = d && o[0] ? (o[B_HP] + scale - 1) / scale : 0;
  p->ground = d && in_wait(d, o);
  p->iframes = d ? o[d->iframes_off] : 0;
  p->action = !d ? TRIO_ACT_DYING : o[d->iframes_off] ? TRIO_ACT_HURT : TRIO_ACT_IDLE;
}

bool trio_boss_blocks(const Snes *s, int x, bool face_right) {
  const uint32_t B = boss_slot(s);
  const BossDef *d = def_of(s);
  if (!d || d != &defs[TRIO_BOSS_WARLORD]) return false;
  const uint8_t *o = s->wram + B;
  //Only while he waits (or walks): his attacks open his guard.
  if (!is_idle(d, o[B_PATTERN])) return false;
  int bx = rd16(s, B + 0x05);
  return face_right ? x > bx : x < bx;
}

bool trio_boss_vulnerable(const Snes *s) {
  const uint32_t B = boss_slot(s);
  const uint8_t *o = s->wram + B;
  const BossDef *d = def_of(s);
  return d && o[B_STATE] == d->fight && o[B_HP] > 0 && o[d->iframes_off] == 0;
}

void trio_boss_damage(Snes *s, int dmg) {
  const uint32_t B = boss_slot(s);
  const BossDef *d = def_of(s);
  if (!d) return;
  uint8_t *o = s->wram + B;
  //Balance: some bosses take less than a fighter would (tools/balance.c).
  int raw = (dmg * d->hp_scale * d->taken + 50) / 100;
  int hp = o[B_HP] - (raw < 1 ? 1 : raw);
  o[B_HP] = (uint8_t)(hp < 0 ? 0 : hp);
  o[B_HIT] = (uint8_t)d->hit;
  o[d->iframes_off] = (uint8_t)d->iframes;
}

void trio_boss_set_hp(Snes *s, int hp) {
  const BossDef *d = def_of(s);
  if (d) s->wram[boss_slot(s) + B_HP] = (uint8_t)(hp * d->hp_scale);
}

bool trio_boss_idle(const Snes *s) {
  const uint32_t B = boss_slot(s);
  const BossDef *d = def_of(s);
  return d && s->wram[B + B_STATE] == d->fight && in_wait(d, s->wram + B);
}

//---------------------------------------------------------------- control

//Between its attacks (while it is held in its wait) the boss is moved by
//the controller with the hero's controls: Left / Right walk, B jumps (held for
//height), Down + B drops through a platform. Gravity, landings, ceilings and
//walls are checked against the arena's blocks in the copy, the moving
//platforms' ladder tops included. Its attacks are its own patterns.
#define GRAVITY 0x40
#define ATK_CANCEL 30
#define ATK_MAX 150
#define FALL_MAX 0x580

static bool wall_at(const Snes *s, int x, int y) { return trio_at_solid(trio_attr(s, x, y)); }

//Ground the feet can stand on at row y (solid, a slope, or a ladder top
//unless dropping through).
static bool floor_at(const Snes *s, int x, int y, bool drop) {
  int a = trio_attr(s, x, y);
  return trio_at_solid(a) || (a >= 1 && a <= 0x0C) || (a == TRIO_AT_LADDER_TOP && !drop);
}

static bool floor_row(const Snes *s, const BossDef *d, int x, int y, bool drop) {
  return floor_at(s, x - d->half_w + 2, y, drop) || floor_at(s, x, y, drop) || floor_at(s, x + d->half_w - 2, y, drop);
}

//The body with its feet at (x, feet) is clear of walls.
static bool body_clear(const Snes *s, const BossDef *d, int x, int feet) {
  for (int y = feet - 4; y > feet - d->height; y -= 12)
    if (wall_at(s, x - d->half_w, y) || wall_at(s, x + d->half_w, y)) return false;
  return !wall_at(s, x - d->half_w, feet - d->height) && !wall_at(s, x + d->half_w, feet - d->height);
}

static void control(Snes *s, const BossDef *d, TrioBossCtl *c, const ArenaStage *st) {
  const ArenaDef *box = st->def;
  uint32_t B = boss_slot(s);
  uint8_t *o = s->wram + B;
  uint16_t pad = c->pad, press = pad & (uint16_t)~c->prev_pad;
  c->prev_pad = pad;
  int x = rd16(s, B + 0x05), feet = (int16_t)rd16(s, B + 0x08) + d->feet;
  if (c->drop_t > 0) c->drop_t--;
  bool drop = c->drop_t > 0;
  //Ground under it (taken over from an attack, or walked off a ledge).
  if (c->ground && !floor_row(s, d, x, feet, drop)) {
    //Down a slope or a step of up to a block.
    int k = 1;
    while (k <= 16 && !floor_row(s, d, x, feet + k, drop)) k++;
    if (k <= 16 && c->walk) feet += k - 1;
    else { c->ground = false; c->vy = 0; }
  } else if (!c->ground && c->vy <= 0 && floor_row(s, d, x, feet, drop) && (feet & 15) == 0) {
    c->ground = true;
  }
  //Jump, or drop through a platform.
  if (press & PAD_B && c->ground) {
    if ((pad & PAD_DOWN) && trio_attr(s, x, feet) == TRIO_AT_LADDER_TOP) {
      c->drop_t = 10;
      drop = true;
      c->vy = -0x100;
    } else {
      c->vy = d->jump_speed;
    }
    c->ground = false;
  }
  if (!(pad & PAD_B) && c->vy > 0x100) c->vy = 0x100;
  //Walk (steered in the air too). Steps up to a block are climbed on the
  //ground; walls stop it.
  if (c->walk) {
    c->sub_x += d->walk_speed;
    int n = c->sub_x >> 8;
    c->sub_x &= 0xFF;
    for (int i = 0; i < n; i++) {
      int nx = x + c->walk;
      if (nx < box->x0 + d->half_w || nx > box->x1 - d->half_w) break;
      int k = 0;
      while (k <= (c->ground ? 16 : 0) && !body_clear(s, d, nx, feet - k)) k++;
      if (k > (c->ground ? 16 : 0)) break;
      x = nx;
      feet -= k;
    }
    c->walk_t++;
    if (c->ground && d->nwalk) o[0x17] = d->walk_frames[(c->walk_t / 8) % d->nwalk];
  } else {
    c->walk_t = 0;
  }
  //Gravity, landing on floors and platforms, ceilings.
  if (!c->ground) {
    c->vy -= GRAVITY;
    if (c->vy < -FALL_MAX) c->vy = -FALL_MAX;
    c->sub_y -= c->vy;
    int dy = c->sub_y >> 8;
    c->sub_y &= 0xFF;
    if (dy > 0) {
      for (int i = 0; i < dy; i++) {
        if (((feet + 1) & 15) == 0 && floor_row(s, d, x, feet + 1, drop)) {
          feet = feet + 1;
          c->ground = true;
          c->vy = 0;
          c->sub_y = 0;
          break;
        }
        feet++;
      }
    } else {
      for (int i = 0; i < -dy; i++) {
        if (wall_at(s, x - d->half_w + 2, feet - d->height - 1) || wall_at(s, x + d->half_w - 2, feet - d->height - 1)) { c->vy = 0; break; }
        feet--;
      }
    }
    if (!c->ground && (d->f_rise || d->f_fall)) o[0x17] = c->vy > 0 ? d->f_rise : d->f_fall;
  }
  wr16(s, B + 0x05, (uint16_t)x);
  wr16(s, B + 0x08, (uint16_t)(feet - d->feet));
}

//Ground somewhere under (x, feet) down to the bottom of the box (not a pit),
//and standing on a floor right at feet.
static bool floor_under(const ArenaStage *st, int x, int feet) {
  for (int y = feet; y < st->def->y1 + 16; y += 16) {
    int a = trio_geo_at(&st->geo, x, y);
    if (a == 0 || a == TRIO_AT_WATER || a == TRIO_AT_WATER_TOP || a == TRIO_AT_LADDER) continue;
    return trio_at_solid(a) || (a >= 1 && a <= 0x0C) || a == TRIO_AT_LADDER_TOP;
  }
  return false;
}

static bool on_floor(const ArenaStage *st, int x, int feet) {
  int a = trio_geo_at(&st->geo, x, feet);
  return trio_at_solid(a) || (a >= 1 && a <= 0x0C) || a == TRIO_AT_LADDER_TOP;
}

void trio_boss_frame(Snes *s, TrioBossCtl *c, int move, const ArenaStage *st) {
  const uint32_t B = boss_slot(s);
  const ArenaDef *box = st->def;
  uint8_t *o = s->wram + B;
  const BossDef *d = def_of(s);
  const TrioGame *g = trio_game(s);
  for (uint32_t a = g->enemy_first + TRIO_OBJ_SIZE; a < g->enemy_end; a += TRIO_OBJ_SIZE) s->wram[a] = 0;
  if (d && installed[d - defs]) put_tiles(s, installed[d - defs]);
  int bx = rd16(s, B + 0x05);
  //Enemies are not held by the camera: keep the boss inside the room, and
  //below its top (rooms open above let a jump land on ledges out of reach).
  if (d && o[B_STATE] == d->fight) {
    int prev_x = c->last_x;
    int lo = box->x0 + 16, hi = box->x1 - 16;
    if (bx < lo || bx > hi) {
      bx = bx < lo ? lo : hi;
      wr16(s, B + 0x05, (uint16_t)bx);
      wr16(s, B + 0x1A, 0);
    }
    int by = (int16_t)rd16(s, B + 0x08), top = box->y0 + 16 - d->feet;
    if (by < top) {
      wr16(s, B + 0x08, (uint16_t)top);
      if ((int16_t)rd16(s, B + 0x1C) > 0) wr16(s, B + 0x1C, 0);
    }
    //An attack that would carry it over a pit (a dash, a long kick) stops
    //at the edge: the boss goes back to its wait there.
    {
      int moved = bx - prev_x;
      int feet = (int16_t)rd16(s, B + 0x08) + d->feet;
      if (moved && !c->holding && installed[d - defs] && on_floor(st, bx, feet) && !floor_under(st, bx + (moved > 0 ? 1 : -1) * (d->half_w + 4), feet)) {
        uint8_t bhp = o[B_HP], inv = o[d->iframes_off], flags = o[B_FLAGS];
        uint16_t y = rd16(s, B + 0x08);
        memcpy(o, installed[d - defs]->obj, TRIO_OBJ_SIZE);
        wr16(s, B + 0x05, (uint16_t)bx);
        wr16(s, B + 0x08, y);
        o[B_HP] = bhp;
        o[d->iframes_off] = inv;
        o[B_FLAGS] = flags;
      }
    }
    //An attack that keeps it pinned there (a jump meant for a taller room)
    //would never end: after half a second it goes back to its wait, and the
    //controller lets it fall.
    if (by <= top && !c->holding) c->top_t++;
    else c->top_t = 0;
    //Any attack that runs on for 2.5 s (stuck against a wall here) ends too,
    //and B cancels one after half a second into a jump (the press is left
    //for the controller, which takes over at once).
    if (!c->holding) c->atk_t++;
    else c->atk_t = 0;
    bool cancel = !c->holding && c->atk_t >= ATK_CANCEL && (c->pad & PAD_B) && !(c->atk_pad & PAD_B);
    c->atk_pad = c->pad;
    if (cancel) c->prev_pad = 0;
    if ((c->top_t > 30 || c->atk_t > ATK_MAX || cancel) && installed[d - defs]) {
      c->atk_t = 0;
      uint16_t x = rd16(s, B + 0x05), y = rd16(s, B + 0x08);
      uint8_t bhp = o[B_HP], inv = o[d->iframes_off], flags = o[B_FLAGS];
      memcpy(o, installed[d - defs]->obj, TRIO_OBJ_SIZE);
      wr16(s, B + 0x05, x);
      wr16(s, B + 0x08, c->top_t > 30 ? (uint16_t)top : y);
      o[B_HP] = bhp;
      o[d->iframes_off] = inv;
      o[B_FLAGS] = flags;
      c->top_t = 0;
    }
    //Bosses ignore walls and ceilings in their attacks: one that ends up
    //inside solid blocks goes back where it was the frame before (a wall),
    //else is lifted out when it is a step (up to a block), else pushed
    //down below them (lifting it further could put it on top of a ceiling
    //or the level's top row, out of the arena).
    by = (int16_t)rd16(s, B + 0x08);
    bool inside = trio_at_solid(trio_attr(s, bx, by + d->feet - 16)) && trio_at_solid(trio_attr(s, bx, by + d->feet - 32));
    //Run into a wall: back where it was the frame before.
    if (inside && prev_x != bx && !trio_at_solid(trio_attr(s, prev_x, by + d->feet - 16)) &&
        !trio_at_solid(trio_attr(s, prev_x, by + d->feet - 32))) {
      bx = prev_x;
      wr16(s, B + 0x05, (uint16_t)bx);
      wr16(s, B + 0x1A, 0);
      inside = false;
    }
    if (inside) {
      if (!trio_at_solid(trio_attr(s, bx, by + d->feet - 48))) {
        wr16(s, B + 0x08, (uint16_t)(by - 16));
      } else {
        int down = 16;
        while (down < 128 && trio_at_solid(trio_attr(s, bx, by + d->feet - 16 + down))) down += 16;
        wr16(s, B + 0x08, (uint16_t)(by + down));
      }
      wr16(s, B + 0x1C, 0);
    }
    c->last_x = rd16(s, B + 0x05);
  }
  int cam_x = bx - 128, cam_y = (int16_t)rd16(s, B + 0x08) - 112;
  stage_clamp(st, &cam_x, &cam_y, 256);
  trio_camera(s, cam_x, cam_y);
  //X, hidden (+$0E bit 0 off: not drawn), where the boss should look: its
  //target, or out on the chosen side. His whole object (and the actions
  //queued with it, up to $0A7B) is put back each frame as it was at the
  //start, so nothing that touches him in this copy can hurt or kill him.
  uint32_t p = g->player;
  if (!c->have_x) { memcpy(c->x_obj, s->wram + p, sizeof(c->x_obj)); c->have_x = true; }
  memcpy(s->wram + p, c->x_obj, sizeof(c->x_obj));
  int px = c->has_target ? c->tx : bx + (c->face_right ? 200 : -200);
  int py = c->has_target ? c->ty : (int16_t)rd16(s, B + 0x08) + (d ? d->feet : 17) - 17;
  //Inside this copy's camera: below it the game takes him for fallen in a
  //pit and restarts the stage (clearing every object, the boss too). The
  //boss still turns and aims the right way.
  if (px < cam_x + 16) px = cam_x + 16;
  if (px > cam_x + 240) px = cam_x + 240;
  if (py < cam_y + 24) py = cam_y + 24;
  if (py > cam_y + 184) py = cam_y + 184;
  wr16(s, p + 0x05, (uint16_t)px);
  wr16(s, p + 0x22, (uint16_t)px);
  wr16(s, p + 0x08, (uint16_t)py);
  wr16(s, p + 0x24, (uint16_t)py);
  s->wram[p + 0x0E] &= (uint8_t)~0x01;
  wr16(s, p + 0x1A, 0);
  wr16(s, p + 0x1C, 0);
  //X never dies. G3's boss meter shows the boss's HP; in the other games
  //The hero's own meter does (the battle HUD draws that copy's hero meter for them).
  int meter = 16;
  if (d && g->id != SNES_GAME_G3) {
    meter = (o[B_HP] + d->hp_scale - 1) / d->hp_scale;
    meter = meter < 1 ? 1 : meter > 16 ? 16 : meter;
  }
  s->wram[p + 0x27] = (uint8_t)meter;
  s->wram[p + 0x6B] = 60;
  if (d && g->id == SNES_GAME_G3) wr16(s, TRIO_BOSS_METER, B);
  if (!d || o[B_STATE] != d->fight) { c->holding = false; return; }
  //A chooser pattern (or a banned move) goes straight back to the wait the
  //boss was taken in.
  if (((d->chooser >= 0 && o[B_PATTERN] == d->chooser) || (d->banned >= 0 && o[B_PATTERN] == d->banned)) && installed[d - defs]) {
    uint16_t x = rd16(s, B + 0x05), y = (int16_t)rd16(s, B + 0x08);
    uint8_t bhp = o[B_HP], inv = o[d->iframes_off], flags = o[B_FLAGS];
    memcpy(o, installed[d - defs]->obj, TRIO_OBJ_SIZE);
    wr16(s, B + 0x05, x);
    wr16(s, B + 0x08, y);
    o[B_HP] = bhp;
    o[d->iframes_off] = inv;
    o[B_FLAGS] = flags;
  }
  if (in_wait(d, o) && !c->holding) {
    memcpy(c->idle, o, TRIO_OBJ_SIZE);
    c->holding = true;
    c->idle_t = 0;
    //The controller takes over where the attack left it: standing if there
    //is ground under its feet, else falling.
    int x = rd16(s, B + 0x05), feet = (int16_t)rd16(s, B + 0x08) + d->feet;
    c->ground = floor_row(s, d, x, feet, false);
    c->vy = 0;
    c->sub_y = 0;
  }
  if (!c->holding) return;
  c->idle_t++;
  //While it waits, the boss is put back to the moment the wait began before
  //every frame, so the AI's wait never runs out and it never picks a move
  //itself. Position, HP, hit state and flags (facing, hit flash) are its own.
  uint16_t x = rd16(s, B + 0x05), y = (int16_t)rd16(s, B + 0x08);
  uint8_t bhp = o[B_HP], hit = o[B_HIT], inv = o[d->iframes_off], flags = o[B_FLAGS];
  memcpy(o, c->idle, TRIO_OBJ_SIZE);
  wr16(s, B + 0x05, x);
  wr16(s, B + 0x08, y);
  //No speed of the wait's own (G1 Raider's armor waits in a backstep): the
  //controller moves it.
  wr16(s, B + 0x1A, 0);
  wr16(s, B + 0x1C, 0);
  o[B_HP] = bhp;
  o[B_HIT] = hit;
  o[d->iframes_off] = inv;
  o[B_FLAGS] = flags;
  //Turn in place. A new facing waits a frame before a move starts, so the
  //parked the hero is already on that side when the AI turns toward him.
  if (c->face_right) o[B_FLAGS] |= 0x40;
  else o[B_FLAGS] &= (uint8_t)~0x40;
  if (c->faced_right != c->face_right) { c->faced_right = c->face_right; c->idle_t = 0; }
  control(s, d, c, st);
  //Attacks start from the ground.
  if (move != TRIO_MOVE_NONE && c->idle_t >= 2 && c->ground) {
    o[B_PATTERN] = (uint8_t)d->pattern[move];
    o[B_STEP] = 0;
    c->holding = false;
  }
}
