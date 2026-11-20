#include "battle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "abot.h"
#include "arena.h"
#include "sprites.h"
#include "ui.h"
#include "game/battle_hooks.h"
#include "game/recomp_rt.h"

//Everything on screen comes from the games: the stage from a display copy
//of its game (drawn from whole-level tilemaps, so any picture size works),
//the fighters from their copies' sprites, the HUD meters from their copies'
//HUD sprites and the text from the title screen's font.

//The picture is W x H: with BV_NORMAL the frontend's widescreen width by
//224 lines, larger with the other views (more of the arena, same pixels).
#define WIDE TRIO_ARENA_WIDE
static int wide = WIDE;
static int W = PPU_W + 2 * WIDE, H = 224;

static uint32_t pic[BATTLE_MAXH][BATTLE_MAXW];

//---------------------------------------------------------------- text

static UiCanvas C;
enum { FP_TEXT = UI_TEXT, FP_SEL = UI_SEL };
static void text(int x, int y, const char *str, int pal, int scale) { ui_text(&C, x, y, str, pal, scale); }
static void text_c(int cx, int y, const char *str, int pal, int scale) { ui_text_c(&C, cx, y, str, pal, scale); }
static int text_w(const char *str, int scale) { return ui_text_w(str, scale); }
static void dim(int y0, int y1) { ui_dim(&C, y0, y1); }

//---------------------------------------------------------------- roster

//The characters, each in styles: the game it comes from (its look, shots,
//weapons and sounds). HERO, HERO ARMOR (the game's four armor parts and heart
//tanks) and HERO FULL (also G1's Fireball, G2's Uppercut, G3's gold armor)
//are characters of their own. The armor graphics come with the stage load,
//so each style is a boot of its own.
typedef struct Character {
  const char *name;        //the character
  const char *style;
  int game;
  TrioUpgrades up;
  int kind;                //FK_*
  int boss;                //TRIO_BOSS_*
} Character;

static const Character roster[] = {
  //G1's the hero dashes only with the leg part.
  {"HERO", "G1", SNES_GAME_G1, {0x08, 0, 0, false}, FK_X, 0},
  {"HERO", "G2", SNES_GAME_G2, {0, 0, 0, false}, FK_X, 0},
  {"HERO", "G3", SNES_GAME_G3, {0, 0, 0, false}, FK_X, 0},
  {"HERO ARMOR", "G1", SNES_GAME_G1, {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, 0, false}, FK_X, 0},
  {"HERO ARMOR", "G2", SNES_GAME_G2, {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, 0, false}, FK_X, 0},
  {"HERO ARMOR", "G3", SNES_GAME_G3, {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, 0, false}, FK_X, 0},
  {"HERO FULL", "G1", SNES_GAME_G1, {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, 0, true}, FK_X, 0},
  {"HERO FULL", "G2", SNES_GAME_G2, {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, 0, true}, FK_X, 0},
  {"HERO FULL", "G3", SNES_GAME_G3, {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, TRIO_CHIPS_GOLD, false}, FK_X, 0},
  {"BLADE", "G3", SNES_GAME_G3, {0, 0, 0, false}, FK_BLADE, 0},
  {"RAIDER", "G3", SNES_GAME_G3, {0, 0, 0, false}, FK_BOSS, TRIO_BOSS_RAIDER},
  {"RAIDER", "G1", SNES_GAME_G1, {0, 0, 0, false}, FK_BOSS, TRIO_BOSS_G1_RAIDER_ARMOR},
  {"RAIDER", "G1 FOOT", SNES_GAME_G1, {0, 0, 0, false}, FK_BOSS, TRIO_BOSS_G1_RAIDER},
  {"WARLORD", "G3", SNES_GAME_G3, {0, 0, 0, false}, FK_BOSS, TRIO_BOSS_WARLORD},
  {"WARLORD", "G1", SNES_GAME_G1, {0, 0, 0, false}, FK_BOSS, TRIO_BOSS_G1_WARLORD},
  {"WARLORD", "G2", SNES_GAME_G2, {0, 0, 0, false}, FK_BOSS, TRIO_BOSS_G2_WARLORD},
};
#define NCHAR ((int)(sizeof(roster) / sizeof(roster[0])))

int battle_characters(void) { return NCHAR; }
const char *battle_character_name(int c) {
  static char buf[4][32];
  static int k;
  if (c < 0 || c >= NCHAR) return "";
  k = (k + 1) & 3;
  snprintf(buf[k], sizeof buf[k], "%s %s", roster[c].name, roster[c].style);
  return buf[k];
}

const char *battle_character_style(int c) { return c >= 0 && c < NCHAR ? roster[c].style : ""; }
const char *battle_character_group(int c) { return c >= 0 && c < NCHAR ? roster[c].name : ""; }

//The next character (d = 1 / -1) in its first style, or the next style of
//the same character.
int battle_next_character(int c, int d) {
  if (c < 0 || c >= NCHAR) return 0;
  int k = c;
  do k = (k + d + NCHAR) % NCHAR; while (!strcmp(roster[k].name, roster[c].name) && k != c);
  //The first style of that character.
  while (k > 0 && !strcmp(roster[k - 1].name, roster[k].name)) k--;
  return k;
}

int battle_next_style(int c, int d) {
  if (c < 0 || c >= NCHAR) return 0;
  int first = c, last = c;
  while (first > 0 && !strcmp(roster[first - 1].name, roster[c].name)) first--;
  while (last < NCHAR - 1 && !strcmp(roster[last + 1].name, roster[c].name)) last++;
  int n = last - first + 1;
  return first + ((c - first + d) % n + n) % n;
}

bool battle_character_experimental(int c) { return c >= 0 && c < NCHAR && roster[c].kind == FK_BOSS; }

//---------------------------------------------------------------- arenas, songs

int battle_rooms(void) { return arena_ndefs; }
const char *battle_room_name(int room) { return room >= 0 && room < arena_ndefs ? arena_defs[room].name : ""; }

typedef struct Song { int game, track; const char *name; } Song;
static const Song songs[] = {
  {0, 0, "STAGE"},
  {0, 0, "RANDOM"},
  {1, 0x11, "G1 SONG 11"}, {1, 0x13, "G1 SONG 13"}, {1, 0x12, "G1 SONG 12"},
  {1, 0x14, "G1 SONG 14"}, {1, 0x15, "G1 SONG 15"}, {1, 0x19, "G1 SONG 19"},
  {1, 0x18, "G1 SONG 18"}, {1, 0x16, "G1 SONG 16"}, {1, 0x17, "G1 SONG 17"},
  {1, 0x1A, "G1 SONG 1A"}, {1, 0x1B, "G1 SONG 1B"}, {1, 0x1C, "G1 SONG 1C"}, {1, 0x1D, "G1 SONG 1D"},
  {2, 0x19, "G2 SONG 19"}, {2, 0x12, "G2 SONG 12"}, {2, 0x14, "G2 SONG 14"}, {2, 0x15, "G2 SONG 15"},
  {2, 0x16, "G2 SONG 16"}, {2, 0x13, "G2 SONG 13"}, {2, 0x1F, "G2 SONG 1F"},
  {2, 0x17, "G2 SONG 17"}, {2, 0x18, "G2 SONG 18"}, {2, 0x29, "G2 SONG 29"},
  {2, 0x22, "G2 SONG 22"}, {2, 0x2C, "G2 SONG 2C"},
  {3, 0x12, "G3 SONG 12"}, {3, 0x13, "G3 SONG 13"}, {3, 0x14, "G3 SONG 14"},
  {3, 0x15, "G3 SONG 15"}, {3, 0x16, "G3 SONG 16"}, {3, 0x2B, "G3 SONG 2B"},
  {3, 0x18, "G3 SONG 18"}, {3, 0x1F, "G3 SONG 1F"}, {3, 0x19, "G3 SONG 19"},
  {3, 0x2C, "G3 SONG 2C"}, {3, 0x17, "G3 SONG 17"}, {3, 0x29, "G3 SONG 29"},
};
#define NSONG ((int)(sizeof(songs) / sizeof(songs[0])))

int battle_songs(void) { return NSONG; }
const char *battle_song_name(int i) { return i >= 0 && i < NSONG ? songs[i].name : ""; }

//---------------------------------------------------------------- state

static const uint8_t *roms[4];
static uint32_t rom_sizes[4];
static bool recomp;

//Each character booted from power-on in its game's home stage (with its
//upgrades), once per session; then a copy installed in the current arena.
static Snes home[NCHAR];
static bool home_ok[NCHAR];
static Snes base[NCHAR];
static bool base_ok[NCHAR];
static TrioBossSrc boss_src[TRIO_BOSS_COUNT];

static ArenaStage stage;
static int stage_room = -1;
static ArenaSound snd;

static Arena A;
static ABot bots[ARENA_MAX];
static BattleSetup setup;
static bool live, paused, results;
static int pause_sel, frame_no;
static uint16_t prev_any;
static float cam_x, cam_y;
static ArenaSprites spr[ARENA_MAX][2];   //this frame's and the frame before's
static int spr_cur;

//The plain hero of a game, the first in the roster (Blade and the bosses start
//from him; G1's has the leg part).
static int plain_x(int game) {
  for (int c = 0; c < NCHAR; c++)
    if (roster[c].game == game && roster[c].kind == FK_X) return c;
  return -1;
}

//The same character in game g ("HERO ARMOR" G1 for "HERO ARMOR" G3), or -1.
static int same_tier(int c, int g) {
  for (int k = 0; k < NCHAR; k++)
    if (roster[k].game == g && !strcmp(roster[k].name, roster[c].name)) return k;
  return -1;
}

//X of the game who owns its special (G1's Fireball, G2's Uppercut).
static int special_x(int game) {
  for (int c = 0; c < NCHAR; c++)
    if (roster[c].game == game && roster[c].kind == FK_X && roster[c].up.special) return c;
  return plain_x(game);
}

static bool make_home(int c) {
  if (home_ok[c]) return true;
  const Character *ch = &roster[c];
  if (!roms[ch->game]) return false;
  if (ch->kind != FK_X) {
    int plain = plain_x(ch->game);
    if (plain < 0 || !make_home(plain)) return false;
    snes_clone(&home[c], &home[plain]);
    if (ch->kind == FK_BLADE) trio_make_blade(&home[c]);
    home_ok[c] = true;
    return true;
  }
  if (!trio_boot_stage(&home[c], roms[ch->game], rom_sizes[ch->game], recomp, trio_games[ch->game].home, &ch->up)) return false;
  home_ok[c] = true;
  return true;
}

//The character standing in the current arena, ready to be cloned.
static bool make_base(int c) {
  if (base_ok[c]) return true;
  if (!make_home(c)) return false;
  const Character *ch = &roster[c];
  snes_clone(&base[c], &home[c]);
  if (!stage_install(&stage, &base[c])) return false;
  trio_place(&base[c], stage.spawn_x[0], stage.spawn_y[0] - 17, true);
  if (ch->kind == FK_BOSS) {
    TrioBossSrc *src = &boss_src[ch->boss];
    if (!src->ok && !trio_boss_source(src, ch->boss, roms[ch->game], rom_sizes[ch->game], recomp)) return false;
    trio_boss_install(&base[c], src, stage.spawn_x[0], stage.spawn_y[0]);
    if (getenv("BOSS_LOG")) fprintf(stderr, "boss %s installed: kind %d\n", ch->name, trio_boss_kind(&base[c]));
  }
  for (int i = 0; i < 4; i++) {
    if (ch->kind != FK_BOSS) stage_follow(&stage, &base[c]);
    base[c].pad[0] = base[c].pad[1] = 0;
    snes_run_frame(&base[c]);
  }
  if (getenv("BOSS_LOG") && ch->kind == FK_BOSS) fprintf(stderr, "boss %s after 4 frames: kind %d\n", ch->name, trio_boss_kind(&base[c]));
  base_ok[c] = true;
  return true;
}

static bool load_stage(int room) {
  if (room == stage_room && stage.ok) return true;
  if (stage.ok) stage_free(&stage);
  stage_room = -1;
  for (int c = 0; c < NCHAR; c++) base_ok[c] = false;
  const ArenaDef *d = &arena_defs[room];
  if (!roms[d->game] || !stage_boot(&stage, d, roms[d->game], rom_sizes[d->game], recomp)) return false;
  stage_room = room;
  return true;
}

//---------------------------------------------------------------- camera

//The camera frames the biggest group of fighters that fits on the screen
//(people count far more than CPU players), centred on that group. Candidate
//groups are those around each fighter and each pair's midpoint; the group
//framed last frame wins ties, so the view doesn't flip between groups.
static unsigned cam_group;

static void camera_target(int *tx, int *ty) {
  int fx[ARENA_MAX], fy[ARENA_MAX], wt[ARENA_MAX], id[ARENA_MAX], n = 0;
  for (int i = 0; i < ARENA_MAX; i++) {
    const ArenaFighter *f = &A.f[i];
    if (!f->on || f->dead) continue;
    fx[n] = f->p.x;
    fy[n] = f->p.y;
    wt[n] = f->cpu ? 1 : 16;
    id[n++] = i;
  }
  if (!n) { *tx = (int)cam_x + W / 2; *ty = (int)cam_y + H * 3 / 5; return; }
  int hw = W / 2 - 40, hh = H / 2 - 36;
  int best_score = -1;
  unsigned best = 0;
  for (int a = 0; a < n; a++)
    for (int b = a; b < n; b++) {
      int cx = (fx[a] + fx[b]) / 2, cy = (fy[a] + fy[b]) / 2;
      unsigned grp = 0;
      int score = 0;
      for (int k = 0; k < n; k++)
        if (abs(fx[k] - cx) <= hw && abs(fy[k] - cy) <= hh) { grp |= 1u << id[k]; score += wt[k] * 4; }
      if (grp == cam_group) score += 2;
      if (score > best_score) { best_score = score; best = grp; }
    }
  cam_group = best;
  int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
  for (int k = 0; k < n; k++) {
    if (!(best >> id[k] & 1)) continue;
    if (fx[k] < x0) x0 = fx[k];
    if (fx[k] > x1) x1 = fx[k];
    if (fy[k] < y0) y0 = fy[k];
    if (fy[k] > y1) y1 = fy[k];
  }
  *tx = (x0 + x1) / 2;
  *ty = (y0 + y1) / 2 - 8;
}

//An item object's y (+$08) above the feet the arena keeps for it.
#define ITEM_FEET 8

//Picture camera: world position of its top-left pixel. The fighters sit a
//little under the middle (more room above for jumps). The display copy's
//camera is centered on the picture's.
static int pic_x, pic_y;

static void sync_items(void);

static void camera_step(bool snap) {
  sync_items();
  int tx, ty;
  camera_target(&tx, &ty);
  int wx = tx - W / 2, wy = ty - H * 3 / 5;
  stage_clamp_pic(&stage, &wx, &wy, W, H);
  if (snap) { cam_x = (float)wx; cam_y = (float)wy; }
  cam_x += ((float)wx - cam_x) * 0.12f;
  cam_y += ((float)wy - cam_y) * 0.12f;
  pic_x = (int)cam_x;
  pic_y = (int)cam_y;
  stage_view(&stage, pic_x + W / 2 - 128, pic_y + H / 2 - 112, 256, false);
}

//---------------------------------------------------------------- match

static void start_match(void) {
  if (live) arena_free(&A);
  live = false;
  //Sound (sound.h): each game with fighters gives a chip with its music
  //stopped; one game's chip (the stage's or the chosen song's) has the song.
  snd_init(&snd);
  ArenaSlot slots[ARENA_MAX] = {{0}};
  for (int i = 0; i < ARENA_MAX; i++) {
    int t = setup.slot[i];
    if (t == BS_OFF) continue;
    int c = setup.fighter[i] >= 0 && setup.fighter[i] < NCHAR ? setup.fighter[i] : 0;
    if (!make_base(c)) c = 0;
    if (!make_base(c)) continue;
    slots[i].on = true;
    slots[i].cpu = t >= BS_CPU1;
    slots[i].level = t >= BS_CPU1 ? t - BS_CPU1 + 1 : 0;
    slots[i].base = &base[c];
    slots[i].specials = !strcmp(roster[c].name, "HERO FULL");
    if (setup.weapons && roster[c].kind == FK_X) trio_unlock_weapons(&base[c]);
    if (!snd.silent[roster[c].game].on) snd_add(&snd, &home[c], false);
    //X of the same tier in the other games: for the weapons of the games
    //in WEAPON SETS, and HERO FULL's specials (arena.c, hop / sp_start).
    slots[i].pool = setup.weapons && roster[c].kind == FK_X ? (uint8_t)(setup.weapon_games & 7) : 0;
    for (int g = 1; g <= 3 && roster[c].kind == FK_X; g++) {
      bool need = (slots[i].pool >> (g - 1) & 1) || (slots[i].specials && g <= SNES_GAME_G2);
      int c2 = same_tier(c, g);
      if (g == roster[c].game || !need || c2 < 0 || !roms[g] || !make_base(c2)) continue;
      if (setup.weapons) trio_unlock_weapons(&base[c2]);
      slots[i].alt[g] = &base[c2];
      if (!snd.silent[g].on) snd_add(&snd, &home[c2], false);
    }
  }
  int song = setup.music;
  if (song == 1) song = 2 + (int)(time(NULL) % (NSONG - 2));
  bool played = false;
  if (song >= 2 && song < NSONG && roms[songs[song].game]) {
    int c = plain_x(songs[song].game);
    if (c >= 0 && make_home(c)) played = snd_add_song(&snd, &home[c], songs[song].track);
  }
  //The stage's own song: the display copy has been playing it (music -1:
  //none, for tests).
  if (!played) snd_add(&snd, stage.view, setup.music >= 0);
  stage.view->no_apu = true;
  static const int minutes[BT_COUNT] = {0, 2, 3, 5};
  int t = setup.time >= 0 && setup.time < BT_COUNT ? setup.time : BT_OFF;
  ArenaRules rules = {setup.stocks, setup.weapons, setup.items, minutes[t] * 60 * 60};
  arena_init(&A, &stage, &snd, slots, rules);
  //G1's Fireball and G2's Uppercut for HERO FULL of the other games: the hero of
  //those games standing in the arena does them for him (arena.c).
  for (int g = SNES_GAME_G1; g <= SNES_GAME_G2; g++) {
    bool need = false;
    for (int i = 0; i < ARENA_MAX; i++)
      if (slots[i].on && slots[i].specials && roster[setup.fighter[i] >= 0 && setup.fighter[i] < NCHAR ? setup.fighter[i] : 0].game != g) need = true;
    int c = special_x(g);
    if (need && c >= 0 && make_base(c)) {
      arena_set_helper(&A, g, &base[c]);
      if (!snd.silent[g].on) snd_add(&snd, &home[c], false);
    }
  }
  //BATTLE_SEED: the same CPU play every run (tools).
  uint32_t seed = getenv("BATTLE_SEED") ? (uint32_t)atoi(getenv("BATTLE_SEED")) : (uint32_t)time(NULL);
  for (int i = 0; i < ARENA_MAX; i++) abot_init(&bots[i], i, seed);
  A.rng ^= seed;
  live = true;
  paused = results = false;
  memset(spr, 0, sizeof(spr));
  camera_step(true);
  for (int i = 0; i < 8; i++) camera_step(false);
}

bool battle_begin(const BattleSetup *s, const uint8_t *const rom[4], const uint32_t size[4], bool rc, int wide_cols) {
  setup = *s;
  wide = wide_cols < 0 ? 0 : wide_cols > WIDE ? WIDE : wide_cols & ~1;
  static const int sizes[BV_COUNT][2] = {{0, 224}, {512, 288}, {640, 360}};
  int v = setup.view >= 0 && setup.view < BV_COUNT ? setup.view : BV_NORMAL;
  W = sizes[v][0] ? sizes[v][0] : PPU_W + 2 * wide;
  H = sizes[v][1];
  for (int g = 1; g <= 3; g++) {
    if (roms[g] != rom[g])
      for (int c = 0; c < NCHAR; c++)
        if (roster[c].game == g) home_ok[c] = base_ok[c] = false;
    roms[g] = rom[g];
    rom_sizes[g] = size[g];
  }
  if (rc != recomp) {
    for (int c = 0; c < NCHAR; c++) home_ok[c] = base_ok[c] = false;
    stage_room = -1;
  }
  recomp = rc;
  if (setup.room < 0 || setup.room >= arena_ndefs) setup.room = 0;
  snes_skip_render = true;
  if (!load_stage(setup.room)) { snes_skip_render = false; return false; }
  const ArenaDef *d = stage.def;
  //Shots live anywhere in the box (and a screen around it).
  battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
  frame_no = 0;
  prev_any = 0xFFFF;  //the button that started the match is not a press here
  start_match();
  return true;
}

const Arena *battle_arena(void) { return &A; }

void battle_end(void) {
  if (live) arena_free(&A);
  live = false;
  battle_box.on = false;
  snes_skip_render = false;
}

//---------------------------------------------------------------- picture

static int view_dx(void) { return -pic_x; }
static int view_dy(void) { return -pic_y; }

static int hud_objs(const Snes *s) { return s->has_cx4 ? s->cx4.ram[0x626] : 16; }

//A fighter's sprites, back to front. A shot of a weapon he has put away is
//drawn with the copy's tiles and colors from before the switch (old_ppu):
//its sprites are the ones at that shot's place (where it is, or a frame
//ago, when the list was built). Never the ones at the hero's own place.
static void draw_sprites(const ArenaFighter *f, const ArenaSprites *o) {
  const TrioGame *g = trio_game(f->s);
  int w = trio_weapon(f->s), rise = f->kind == FK_BLADE ? TRIO_BLADE_RISE : 0;
  for (int n = o->n - 1; n >= 0; n--) {
    const ArenaSpr *p = &o->s[n];
    const Ppu *ppu = &f->s->ppu;
    bool own = (abs(p->ox - f->p.x) <= 2 && abs(p->oy - (f->p.y - rise)) <= 2) || (abs(p->ox - f->prev.x) <= 2 && abs(p->oy - (f->prev.y - rise)) <= 2);
    for (int k = 0; k < f->nshots && f->have_old && !own; k++) {
      const TrioShot *sh = &f->shots[k];
      int idx = (sh->slot - (int)g->shot_first) / TRIO_OBJ_SIZE;
      if (idx < 0 || idx >= 32 || f->shot_weapon[idx] == w) continue;
      if ((abs(p->ox - sh->x) <= 2 && abs(p->oy - sh->y) <= 2) || (abs(p->ox - (sh->x - sh->vx)) <= 2 && abs(p->oy - (sh->y - sh->vy)) <= 2)) { ppu = f->old_ppu; break; }
    }
    ppu_draw_sprite(ppu, p->x + view_dx(), p->y + view_dy(), p->tile, p->attr, p->big, &pic[0][0], BATTLE_MAXW, W, H);
  }
}

static void draw_fighter(int i) {
  const ArenaFighter *f = &A.f[i];
  if (!f->on || (f->dead && f->stocks <= 0)) return;
  //Spawn protection blinks the whole fighter.
  if (f->spawn_t > 0 && f->p.action != TRIO_ACT_WARP && (frame_no / 3) % 2) return;
  //A defeated boss plays its retreat scene in its copy: it blinks out instead.
  if (f->kind == FK_BOSS && f->dead && (f->dead_t > 30 || (frame_no / 2) % 2)) return;
  //The sprites the copy's PPU shows now were built the frame before. In a
  //special from a helper copy, the helper's the hero is the one shown.
  if (!f->sp_move || !f->sp_go || f->sp_game == trio_game(f->s)->id) {
    if (f->kind == FK_BOSS) spr_draw(f->s, &spr[i][spr_cur ^ 1], &pic[0][0], BATTLE_MAXW, W, H, view_dx(), view_dy());
    else draw_sprites(f, &spr[i][spr_cur ^ 1]);
  }
  //A special done by a helper copy: its hero (in an Uppercut) and its shots.
  for (int g = 1; g <= 3; g++) {
    if (!f->helper[g] || !f->helper_live[g]) continue;
    static ArenaSprites hs;
    spr_collect(f->helper[g], &hs);
    spr_draw(f->helper[g], &hs, &pic[0][0], BATTLE_MAXW, W, H, view_dx(), view_dy());
  }
}

static const char *tag(int i) {
  static char buf[16];
  snprintf(buf, sizeof buf, "%dP", i + 1);
  return buf;
}

//Name over each fighter, or at the screen edge when it is out of view.
static void draw_tag(int i) {
  const ArenaFighter *f = &A.f[i];
  if (!f->on || f->dead) return;
  int x = f->p.x + view_dx(), y = f->p.y + view_dy() - 36;
  int pal = f->cpu ? FP_TEXT : FP_SEL;
  if (y < 30) y = 30;
  if (y > H - 10) y = H - 10;
  if (x < 12) { text(2, y + 16, "<", pal, 1); text(10, y + 16, tag(i), pal, 1); return; }
  if (x >= W - 12) { text(W - 10, y + 16, ">", pal, 1); text(W - 10 - text_w(tag(i), 1), y + 16, tag(i), pal, 1); return; }
  text_c(x, y, tag(i), pal, 1);
}

//Each fighter's own HUD meter (the game's life and weapon bars), side by
//side, with its name and the lives left under it.
static void draw_huds(void) {
  int n = 0;
  for (int i = 0; i < ARENA_MAX; i++) n += A.f[i].on;
  int gap = W / (n > 0 ? n : 1), k = 0;
  for (int i = 0; i < ARENA_MAX; i++) {
    const ArenaFighter *f = &A.f[i];
    if (!f->on) continue;
    int col = k * gap + (gap - 40) / 2;
    k++;
    if (!(f->dead && f->stocks <= 0)) {
      //G3's bosses show the game's boss meter (drawn at x 232) instead of
      //The hero's; G1 / G2 bosses show the hero's meter, which their copy fills with the
      //boss's HP (boss.c).
      if (f->kind == FK_BOSS && trio_game(f->s)->id == SNES_GAME_G3) ppu_draw_objs(&f->s->ppu, TRIO_BOSS_METER_OBJ, hud_objs(f->s), 0, &pic[0][0], BATTLE_MAXW, W, H, col - 232, 0);
      else ppu_draw_objs(&f->s->ppu, 0, hud_objs(f->s), 0, &pic[0][0], BATTLE_MAXW, W, H, col - 16, 0);
    }
    char buf[24];
    snprintf(buf, sizeof buf, "%s%s", tag(i), f->cpu && gap >= 80 ? " CPU" : "");
    text(col + 4, 8, buf, f->cpu ? FP_TEXT : FP_SEL, 1);
    if (f->stocks > 0) snprintf(buf, sizeof buf, "x%d", f->stocks);
    else snprintf(buf, sizeof buf, "OUT");
    text(col + 4, 18, buf, FP_TEXT, 1);
  }
}

//Items are the game's own pickups: item objects in the display copy (the
//stage's game), kept where the arena has them (stage.c) and drawn from its
//sprites. One about to vanish blinks.
static ArenaSprites item_spr;

static void sync_items(void) {
  for (int k = 0; k < STAGE_ITEMS; k++) {
    const ArenaItem *it = k < ARENA_ITEMS ? &A.items[k] : NULL;
    int kind = !it ? TRIO_ITEM_NONE : it->kind == IT_LIFE_S ? TRIO_ITEM_LIFE_S : it->kind == IT_LIFE_L ? TRIO_ITEM_LIFE_L
             : it->kind == IT_WEAPON ? TRIO_ITEM_WEAPON_L : it->kind == IT_HEART ? TRIO_ITEM_1UP : TRIO_ITEM_NONE;
    stage.item_kind[k] = kind;
    if (kind) { stage.item_x[k] = it->x; stage.item_y[k] = it->y - ITEM_FEET; }
  }
}

static void draw_items(void) {
  spr_collect_items(stage.view, &item_spr);
  bool blink = false;
  for (int k = 0; k < ARENA_ITEMS; k++)
    if (A.items[k].kind != IT_NONE && A.items[k].t > 60 * 17) blink = true;
  if (blink && (frame_no / 4) % 2) {
    //Only the ones about to vanish blink: draw the others.
    ArenaSprites keep = {.n = 0};
    for (int n = 0; n < item_spr.n; n++) {
      bool old = false;
      for (int k = 0; k < ARENA_ITEMS; k++) {
        const ArenaItem *it = &A.items[k];
        if (it->kind != IT_NONE && it->t > 60 * 17 && abs(item_spr.s[n].x + 8 - it->x) < 16 && abs(item_spr.s[n].y + 8 - it->y) < 24) old = true;
      }
      if (!old) keep.s[keep.n++] = item_spr.s[n];
    }
    spr_draw(stage.view, &keep, &pic[0][0], BATTLE_MAXW, W, H, view_dx(), view_dy());
    return;
  }
  spr_draw(stage.view, &item_spr, &pic[0][0], BATTLE_MAXW, W, H, view_dx(), view_dy());
}

//The arena's own shots (arena.c, ghosts), from their pictures.
static void draw_ghosts(void) {
  for (int k = 0; k < GHOST_MAX; k++) {
    const ArenaGhost *g = &A.ghost[k];
    if (!g->on) continue;
    int x0 = g->x - GHOST_SIZE / 2 + view_dx(), y0 = g->y - GHOST_SIZE / 2 + view_dy();
    for (int y = 0; y < GHOST_SIZE; y++) {
      int Y = y0 + y;
      if (Y < 0 || Y >= H) continue;
      for (int x = 0; x < GHOST_SIZE; x++) {
        int X = x0 + x;
        if (X >= 0 && X < W && g->pic[y][x] != GHOST_CLEAR) pic[Y][X] = g->pic[y][x];
      }
    }
  }
}

//Hit sparks, block flashes, pickups.
static struct { int kind, x, y, t; } sparks[32];
#define POPUP_FRAMES 70          //an extra life says so, rising over the fighter
static void draw_fx(void) {
  for (int k = 0; k < A.nfx; k++)
    for (int j = 0; j < 32; j++)
      if (sparks[j].t <= 0) { sparks[j].kind = A.fx[k].kind; sparks[j].x = A.fx[k].x; sparks[j].y = A.fx[k].y; sparks[j].t = A.fx[k].kind == FX_1UP ? POPUP_FRAMES : 10; break; }
  for (int j = 0; j < 32; j++) {
    if (sparks[j].t <= 0) continue;
    int t = sparks[j].t--;
    if (sparks[j].kind == FX_1UP) {
      if (t > 12 || (t / 2) % 2) text_c(sparks[j].x + view_dx(), sparks[j].y + view_dy() - 28 - (POPUP_FRAMES - t) / 3, "1UP", FP_SEL, 1);
      continue;
    }
    int r = sparks[j].kind == FX_BIG_HIT || sparks[j].kind == FX_KO ? 14 - t : 10 - t * 2 / 3;
    uint32_t c = sparks[j].kind == FX_BLOCK ? 0x80E0FF : sparks[j].kind == FX_ITEM ? 0xFFF080 : 0xFFFFFF;
    int x = sparks[j].x + view_dx(), y = sparks[j].y + view_dy();
    static const int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1}, dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    for (int a = 0; a < 8; a++)
      for (int q = r / 2; q <= r; q++) {
        int X = x + dx[a] * q, Y = y + dy[a] * q;
        if (X >= 0 && X < W && Y >= 0 && Y < H) pic[Y][X] = c;
      }
  }
}

//Moving platforms, drawn with the box's own floor block (a small BG1 map
//of w blocks), or plain when the box has none.
static void draw_movers(void) {
  for (int k = 0; k < A.nmover; k++) {
    const ArenaMover *m = &A.mover[k];
    int w = m->lane.w;
    if (stage.plat_blk >= 0) {
      uint8_t map[8 * 2 * 2 * 2];
      uint16_t t[4];
      trio_block_tiles(stage.view, (uint16_t)stage.plat_blk, t);
      for (int j = 0; j < w && j < 8; j++)
        for (int q = 0; q < 4; q++) {
          uint8_t *e = map + ((size_t)(j * 2 + (q & 1)) * 2 + (q >> 1)) * 2;
          e[0] = (uint8_t)t[q];
          e[1] = (uint8_t)(t[q] >> 8);
        }
      for (int prio = 0; prio < 2; prio++)
        ppu_draw_bg_map(&stage.view->ppu, 0, map, w * 2, 2, pic_x - m->px, pic_y - m->lane.y, prio, &pic[0][0], BATTLE_MAXW, W, H);
    } else {
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < w * 16; x++) {
          int X = m->px + x - pic_x, Y = m->lane.y + y - pic_y;
          if (X >= 0 && X < W && Y >= 0 && Y < H) pic[Y][X] = y == 0 ? 0xE0E0E8 : 0x606070;
        }
    }
  }
}

//Hazards (stage.h): an ice sheen on the icy floor, streaks in the gust,
//flames (and the embers before them) at the fire jets.
static void blend(int X, int Y, uint32_t c) {
  if (X < 0 || X >= W || Y < 0 || Y >= H) return;
  pic[Y][X] = ((pic[Y][X] >> 1) & 0x7F7F7F) + ((c >> 1) & 0x7F7F7F);
}
static void dot(int X, int Y, uint32_t c) { if (X >= 0 && X < W && Y >= 0 && Y < H) pic[Y][X] = c; }

static void draw_hazards(bool front) {
  for (int k = 0; k < stage.nhaz; k++) {
    const ArenaHazard *h = &stage.haz[k];
    int w = h->x1 - h->x0, hh = h->y1 - h->y0;
    if (h->kind == HZ_ICE && !front) {
      int fy = h->y0 + 16;
      for (int x = h->x0; x < h->x1; x++)
        for (int y = 0; y < 5; y++) blend(x - pic_x, fy + y - pic_y, y < 2 ? 0xE8FCFF : 0x90D8F8);
      for (int s = 0; s < w / 24; s++) {
        int t = (frame_no / 4 + s * 7) % 24;
        if (t < 3) dot(h->x0 + (s * 53 + 11) % w - pic_x, fy + 1 + t - pic_y, 0xFFFFFF);
      }
    } else if (h->kind == HZ_WIND && front) {
      for (int s = 0; s < w * hh / 2400 + 6; s++) {
        int len = 10 + s % 3 * 4;
        int x = h->x0 + (int)(((unsigned)(s * 131) + (unsigned)(frame_no * (3 + s % 3)) * (A.wind_dir > 0 ? 1u : (unsigned)-1)) % (unsigned)w);
        int y = h->y0 + (s * 71 + (frame_no / 6 + s) % 5) % hh;
        for (int i = 0; i < len; i++) blend(x + (A.wind_dir > 0 ? -i : i) - pic_x, y - pic_y, 0xFFFFFF);
      }
    } else if (h->kind == HZ_FIRE && !front) {
      int hgt, ph = arena_fire_phase(&A, k, &hgt);
      int cx = (h->x0 + h->x1) / 2 - pic_x, by = h->y1 - pic_y;
      if (ph == 1) {
        for (int s = 0; s < 6; s++) {
          int t = (frame_no + s * 5) % 20;
          dot(cx - 6 + (s * 5) % 12, by - 1 - t / 2, t < 10 ? 0xFFD040 : 0xF06020);
        }
      } else if (ph == 2) {
        int top = hh * hgt / 256;
        for (int y = 0; y < top; y++) {
          int r = 6 + (y * 3 / (top > 0 ? top : 1)) + ((y * 7 + frame_no * 3) % 5) - 2;
          r = r * (top - y) / (top > 0 ? top : 1) + 3;
          for (int x = -r; x <= r; x++) {
            int e = abs(x) * 256 / (r + 1);
            uint32_t c = e < 90 ? 0xFFF4B0 : e < 170 ? 0xFFB830 : 0xE84818;
            if (((x + y + frame_no) & 7) == 0 && e > 170) continue;
            dot(cx + x, by - y, c);
          }
        }
      }
    }
  }
}

static void compose(void) {
  C = (UiCanvas){&pic[0][0], BATTLE_MAXW, W, H};
  stage_draw(&stage, &pic[0][0], BATTLE_MAXW, W, H, pic_x, pic_y);
  //Outside the box (the level around a boss's room) is darkened.
  if (stage.def->layout == LAYOUT_LEVEL) {
    const ArenaDef *d = stage.def;
    int bx0 = d->x0 - pic_x, bx1 = d->x1 - pic_x;
    if (bx0 > 0) ui_dim_rect(&C, 0, 0, bx0, H, 2);
    if (bx1 < W) ui_dim_rect(&C, bx1, 0, W, H, 2);
  }
  draw_movers();
  draw_hazards(false);
  draw_items();
  for (int i = ARENA_MAX - 1; i >= 0; i--) draw_fighter(i);
  draw_ghosts();
  draw_hazards(true);
  draw_fx();
  for (int i = 0; i < ARENA_MAX; i++) draw_tag(i);
  draw_huds();
  if (A.phase == AP_COUNTDOWN) text_c(W / 2, H / 2 - 16, "READY", FP_SEL, 3);
  else if (A.phase == AP_FIGHT && A.phase_t < 50) text_c(W / 2, H / 2 - 16, "GO", FP_SEL, 3);
  else if (A.phase == AP_GAME) text_c(W / 2, H / 2 - 16, A.time_up ? "TIME" : "GAME", FP_SEL, 3);
  //The clock, under the meters.
  if (A.rules.time_limit && A.phase != AP_OVER) {
    int left = (A.rules.time_limit - A.fight_t + 59) / 60;
    if (left < 0) left = 0;
    char buf[16];
    snprintf(buf, sizeof buf, "%d:%02d", left / 60, left % 60);
    text_c(W / 2, 32, buf, left <= 10 ? FP_SEL : FP_TEXT, 1);
  }
}

static void draw_pause(void) {
  dim(0, H);
  int top = (H - 224) / 2;
  text_c(W / 2, top + 64, "PAUSE", FP_SEL, 2);
  static const char *items[3] = {"CONTINUE", "RESTART", "EXIT"};
  for (int i = 0; i < 3; i++) text_c(W / 2, top + 104 + i * 16, items[i], i == pause_sel ? FP_SEL : FP_TEXT, 1);
  //The controls of the first person's character.
  for (int i = 0; i < ARENA_MAX; i++) {
    const ArenaFighter *f = &A.f[i];
    if (!f->on || f->cpu) continue;
    int c = setup.fighter[i] >= 0 && setup.fighter[i] < NCHAR ? setup.fighter[i] : 0;
    const char *l1 = "B JUMP  Y SHOOT  A DASH", *l2 = "L R WEAPON";
    if (f->kind == FK_BLADE) l2 = "X SWORD";
    else if (f->kind == FK_BOSS) { l1 = "B JUMP  DOWN B DROP"; l2 = "X Y A ATTACKS"; }
    else if (roster[c].up.special && roster[c].game == SNES_GAME_G1) l2 = "L R WEAPON  DOWN FWD Y FIREBALL";
    else if (roster[c].up.special && roster[c].game == SNES_GAME_G2) l2 = "L R WEAPON  FWD DOWN FWD Y UPPERCUT";
    char buf[48];
    snprintf(buf, sizeof buf, "%s %s %s", tag(i), roster[c].name, roster[c].style);
    text_c(W / 2, top + 162, buf, FP_TEXT, 1);
    text_c(W / 2, top + 176, l1, FP_TEXT, 1);
    text_c(W / 2, top + 188, l2, FP_TEXT, 1);
    break;
  }
}

static void draw_results(void) {
  dim(0, H);
  char buf[64];
  int w = A.winner;
  if (w >= 0) snprintf(buf, sizeof buf, "%s WINS", tag(w));
  else snprintf(buf, sizeof buf, "DRAW");
  int top = (H - 224) / 2;
  text_c(W / 2, top + 40, buf, FP_SEL, 3);
  int y = top + 88;
  for (int i = 0; i < ARENA_MAX; i++) {
    const ArenaFighter *f = &A.f[i];
    if (!f->on) continue;
    snprintf(buf, sizeof buf, "%-3s %-12s KO %d FALLS %d DMG %d", tag(i), battle_character_name(setup.fighter[i]), f->kos, f->falls, f->dealt);
    text_c(W / 2, y, buf, i == w ? FP_SEL : FP_TEXT, 1);
    y += 14;
  }
  static const char *items[2] = {"REMATCH", "EXIT"};
  for (int i = 0; i < 2; i++) text_c(W / 2, top + 168 + i * 16, items[i], i == pause_sel ? FP_SEL : FP_TEXT, 1);
}

//---------------------------------------------------------------- frame

//Menu sounds of the game playing the music (as on the game select screen).
enum { SFX_MOVE, SFX_OK, SFX_BACK };
static void menu_sfx(int kind) {
  static const uint8_t ids[4][3] = {{0}, {0x2C, 0x04, 0x2C}, {0x1C, 0x05, 0x1D}, {0x1C, 0x05, 0x1D}};
  int g = snd.music_game;
  if (g >= 1 && g <= 3) snd_play(&snd, g, ids[g][kind], 0);
}

bool battle_frame(const uint16_t pads[BATTLE_SLOTS], uint16_t any) {
  if (!live) return false;
  frame_no++;
  uint16_t pressed = any & (uint16_t)~prev_any;
  prev_any = any;
  bool ok = pressed & (PAD_B | PAD_A | PAD_START), up = pressed & PAD_UP, down = pressed & PAD_DOWN;
  if (results) {
    if (up || down) { pause_sel ^= 1; menu_sfx(SFX_MOVE); }
    snd_frame(&snd);
    if (ok) {
      if (pause_sel == 1) return false;
      start_match();
      return true;
    }
  } else if (paused) {
    if (up) { pause_sel = (pause_sel + 2) % 3; menu_sfx(SFX_MOVE); }
    if (down) { pause_sel = (pause_sel + 1) % 3; menu_sfx(SFX_MOVE); }
    if (ok && pause_sel == 0) menu_sfx(SFX_BACK);
    //The music plays on in the pause menu, as in the games'.
    snd_frame(&snd);
    if (ok) {
      if (pause_sel == 0) paused = false;
      else if (pause_sel == 1) { start_match(); return true; }
      else return false;
    }
  } else if (pressed & PAD_START) {
    paused = true;
    pause_sel = 0;
    menu_sfx(SFX_OK);
    snd_frame(&snd);
  } else {
    uint16_t in[ARENA_MAX] = {0};
    for (int i = 0; i < ARENA_MAX; i++) {
      if (!A.f[i].on) continue;
      in[i] = A.f[i].cpu ? abot_think(&bots[i], &A, i) : (uint16_t)(pads[i] & (uint16_t)~PAD_START);
    }
    arena_step(&A, in);
    snd_frame(&snd);
    spr_cur ^= 1;
    for (int i = 0; i < ARENA_MAX; i++)
      if (A.f[i].on && A.f[i].s) spr_collect(A.f[i].s, &spr[i][spr_cur]);
    if (A.phase == AP_OVER) { results = true; pause_sel = 0; }
    camera_step(false);
  }
  compose();
  if (paused) draw_pause();
  if (results) draw_results();
  return true;
}

const uint32_t *battle_picture(int *w, int *h) {
  *w = W;
  *h = H;
  return &pic[0][0];
}

int battle_audio(int16_t *out, int max_frames) {
  if (!live) return 0;
  return snd_mix(&snd, out, max_frames);
}
