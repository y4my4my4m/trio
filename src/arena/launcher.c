#include "launcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/battle.h"
#include "arena/sound.h"
#include "arena/ui.h"
#include "game/recomp_rt.h"

//Game select screen and the battle setup page (docs/LAUNCHER.md).

#define W PPU_W
//Power-on to each title menu: START skips the intro (G1 shows its logo
//first and takes a second START), then the menu is up and the machine is
//held there before its attract timer runs out.
static const struct { int start1, start2, hold; } boot[4] = {
  {0, 0, 0}, {900, 1650, 1760}, {900, 0, 1150}, {900, 0, 1150},
};
#define BOOT_FRAMES 1760
#define BOOT_STEP 100            //frames per launcher frame for the games booted in the background

typedef struct Title {
  bool avail, booted;
  int boot_fr;
  Snes *s;
  PpuEnh enh;
  SndChip song;            //a chip as the title's song starts (start_song)
  bool have_song;
  uint32_t pic[UI_H][PPU_W];
  bool have_pic;
} Title;

static Title T[4];
static LauncherGame G[4];
static bool open_;
static int sel_game, cursor, page;   //page 0 main, 1 battle setup
static int psel;                     //battle page cursor
static uint16_t prev_any;
static uint32_t pic[UI_H][PPU_MAXW];

//The screen's sound: the selected game's song and the menu sounds, on a
//chip apart from the held machines' (those stay as the games left them,
//for PLAY).
static ArenaSound snd;
static int snd_game;             //the game whose song is on (0 none)
static int quiet;                //samples of silence since the song was last heard
static bool heard;

static const char *names[4] = {"", "GAME 1", "GAME 2", "GAME 3"};
const char *launcher_game_name(int g) { return g >= 1 && g <= 3 ? names[g] : ""; }

//Layers of each title screen kept for the backdrop: the menu text on BG3 is
//replaced by the launcher's own menu (the G2/G3 numerals are sprites, so
//OBJ stays; the menu's cursor sprite is under the launcher's band).
static const uint8_t logo_layers[4] = {0, 0x1B, 0x1B, 0x1B};
#define BAND_Y 136               //the launcher's menu covers the title from here down

//The song for the game's screen. START fades the opening song out in G2
//and G3 (driver command $F1 two frames later) and their title menus are
//silent; G1's title theme is 14 s in when the machine is held. A copy of
//the held machine runs the game's music routine with the song it had
//loaded ($7E:FFFD: $1F in G1, $25 in G2 and G3), and the chip is kept as
//the song starts. G1's lasts 25 s, G2's 52 s, G3's loops.
static void start_song(Title *t, int game) {
  ArenaSound *a = malloc(sizeof(ArenaSound));
  if (!a) return;
  snd_init(a);
  if (snd_add_song(a, t->s, t->s->wram[trio_games[game].song])) {
    t->song = a->chip[game];
    t->have_song = true;
  }
  free(a);
}

static void boot_step(Title *t, int game, int frames) {
  if (t->booted) return;
  if (!t->s) {
    t->s = malloc(sizeof(Snes));
    snes_init(t->s, G[game].rom, G[game].size);
    snes_reset(t->s);
    rt_install(t->s, G[game].recomp);
    t->boot_fr = 0;
  }
  bool skip = snes_skip_render;
  int end = boot[game].hold, f;
  for (int i = 0; i < frames && (f = t->boot_fr) < end; i++, t->boot_fr++) {
    snes_skip_render = f < end - 2;
    bool st = (f >= boot[game].start1 && f < boot[game].start1 + 2) || (boot[game].start2 && f >= boot[game].start2 && f < boot[game].start2 + 2);
    t->s->pad[0] = st ? PAD_START : 0;
    t->s->pad[1] = 0;
    snes_run_frame(t->s);
    int16_t tmp[2048];
    while (apu_pop_samples(&t->s->apu, tmp, 1024) > 0) {}
  }
  snes_skip_render = skip;
  if (t->boot_fr < end) return;
  t->booted = true;
  start_song(t, game);
  //The backdrop: one frame of a copy drawn with the logo layers only.
  Snes *c = malloc(sizeof(Snes));
  snes_clone(c, t->s);
  memset(&t->enh, 0, sizeof(t->enh));
  t->enh.layer_mask = logo_layers[game];
  c->ppu.enh = &t->enh;
  c->pad[0] = c->pad[1] = 0;
  snes_skip_render = false;
  snes_run_frame(c);
  snes_skip_render = skip;
  int w = c->ppu.out_w ? c->ppu.out_w : PPU_W, off = (w - PPU_W) / 2;
  for (int y = 0; y < UI_H; y++) {
    if (y < BAND_Y) memcpy(t->pic[y], &c->ppu.frame[y][off], sizeof(t->pic[y]));
    else memset(t->pic[y], 0, sizeof(t->pic[y]));
  }
  t->have_pic = true;
  free(c);
  //The title font for the launcher's text (G3's, else G2's).
  if (game == SNES_GAME_G3 || !ui_have_font()) ui_grab_font(t->s);
}

void launcher_open(const LauncherGame games[4], int selected) {
  memcpy(G, games, sizeof(G));
  for (int g = 1; g <= 3; g++) T[g].avail = G[g].rom != NULL;
  if (selected < 1 || selected > 3 || !T[selected].avail) {
    selected = 0;
    for (int g = 3; g >= 1 && !selected; g--) if (T[g].avail) selected = g;
  }
  sel_game = selected;
  snd_game = 0;
  //The font comes from G3 or G2: boot one of them first if the pick is G1.
  if (!ui_have_font()) {
    for (int g = 3; g >= 2; g--) if (T[g].avail) { boot_step(&T[g], g, BOOT_FRAMES); break; }
  }
  if (sel_game) boot_step(&T[sel_game], sel_game, BOOT_FRAMES);
  open_ = true;
  cursor = 0;
  page = 0;
  prev_any = 0xFFFF;
}

bool launcher_is_open(void) { return open_; }
void launcher_close(void) { open_ = false; }
int launcher_game(void) { return sel_game; }
const Snes *launcher_machine(int game) { return game >= 1 && game <= 3 && T[game].booted ? T[game].s : NULL; }

void launcher_resume(Snes *s) {
  //The sound chip's cycle count on the CPU's clock, so apu_sync keeps it
  //running from here.
  s->apu.cycles = s->cycles * 1024000ull / SNES_MASTER_HZ;
}

//---------------------------------------------------------------- sound

//Menu sounds, the games' own (their pause menus' cursor, their title's
//confirm, their menus' close; G1 has no close sound).
enum { SFX_MOVE, SFX_OK, SFX_BACK };
static const uint8_t menu_sfx[4][3] = {{0}, {0x2C, 0x04, 0x2C}, {0x1C, 0x05, 0x1D}, {0x1C, 0x05, 0x1D}};

static void sfx(int kind) {
  if (snd_game && snd_game == sel_game) snd_play(&snd, snd_game, menu_sfx[snd_game][kind], 0);
}

//The selected game's song from its start.
static void play_song(int game) {
  snd_init(&snd);
  snd_game = game;
  quiet = 0;
  heard = false;
  if (!T[game].have_song) return;
  snd.chip[game] = T[game].song;
  snd.music_game = game;
}

static int next_game(int g, int d) {
  for (int k = 0; k < 3; k++) {
    g = (g - 1 + d + 3) % 3 + 1;
    if (T[g].avail) return g;
  }
  return g;
}

//---------------------------------------------------------------- battle page

//Three rows per player (who plays it, the character, its style), then the
//match's rows.
enum { PR_SLOT, PR_CHAR, PR_STYLE, PR_COUNT };
#define PI_PLAYERS (BATTLE_SLOTS * PR_COUNT)
static int prow(int i) { return i < PI_PLAYERS ? i % PR_COUNT : -1; }
static int pnum(int i) { return i / PR_COUNT; }
enum {
  PI_STOCKS = PI_PLAYERS, PI_TIME, PI_STAGE, PI_MUSIC, PI_WEAPONS, PI_WSETS, PI_ITEMS, PI_VIEW, PI_FIGHT, PI_BACK, PI_COUNT
};

static const char *slot_names[BS_COUNT] = {
  "OFF", "PLAYER", "KEYS 1", "KEYS 2", "PAD 1", "PAD 2", "PAD 3", "PAD 4", "CPU LV1", "CPU LV2", "CPU LV3",
};

static int cycle(int v, int n, int d) { return n > 0 ? ((v + d) % n + n) % n : 0; }

static int fighters(const BattleSetup *b) {
  int n = 0;
  for (int i = 0; i < BATTLE_SLOTS; i++) n += b->slot[i] != BS_OFF;
  return n;
}

static bool hidden(int i) {
  const BattleSetup *b = g_port.battle;
  return (prow(i) == PR_CHAR || prow(i) == PR_STYLE) && b->slot[pnum(i)] == BS_OFF;
}

static void move(int d) {
  do psel = cycle(psel, PI_COUNT, d);
  while (hidden(psel));
}

static void change(int i, int d) {
  BattleSetup *b = g_port.battle;
  if (prow(i) == PR_SLOT) { b->slot[pnum(i)] = cycle(b->slot[pnum(i)], BS_COUNT, d); return; }
  if (prow(i) == PR_CHAR) { b->fighter[pnum(i)] = battle_next_character(b->fighter[pnum(i)], d); return; }
  if (prow(i) == PR_STYLE) { b->fighter[pnum(i)] = battle_next_style(b->fighter[pnum(i)], d); return; }
  switch (i) {
  case PI_STOCKS: b->stocks = cycle(b->stocks - 1, 9, d) + 1; break;
  case PI_STAGE: b->room = cycle(b->room, battle_rooms(), d); break;
  case PI_MUSIC: b->music = cycle(b->music, battle_songs(), d); break;
  case PI_WEAPONS: b->weapons = !b->weapons; break;
  case PI_WSETS: b->weapon_games = cycle(b->weapon_games, 8, d); break;
  case PI_ITEMS: b->items = !b->items; break;
  case PI_VIEW: b->view = cycle(b->view, BV_COUNT, d); break;
  case PI_TIME: b->time = cycle(b->time, BT_COUNT, d); break;
  default: break;
  }
}

static int row_y(int i) {
  //Players in rows 3-14, the match's rows 16-23, START 24, BACK 26.
  if (i < PI_PLAYERS) return (3 + i) * 8;
  if (i == PI_FIGHT) return 24 * 8;
  if (i == PI_BACK) return 26 * 8;
  return (16 + i - PI_STOCKS) * 8;
}

static void draw_page(UiCanvas *c, bool loading) {
  const BattleSetup *b = g_port.battle;
  ui_dim(c, 0, UI_H);
  ui_text_c(c, W / 2, 12, "BATTLE MODE", UI_SEL, 1);
  char buf[40];
  for (int i = 0; i < PI_COUNT; i++) {
    if (hidden(i)) continue;
    int y = row_y(i), pal = i == psel ? UI_SEL : UI_TEXT;
    const char *label = NULL, *val = NULL;
    int lx = 24;
    if (prow(i) == PR_SLOT) {
      snprintf(buf, sizeof buf, "PLAYER %d", pnum(i) + 1);
      label = buf;
      val = slot_names[b->slot[pnum(i)] % BS_COUNT];
    } else if (prow(i) == PR_CHAR) {
      label = "CHARACTER";
      lx = 40;
      val = battle_character_group(b->fighter[pnum(i)]);
    } else if (prow(i) == PR_STYLE) {
      label = "STYLE";
      lx = 40;
      val = battle_character_style(b->fighter[pnum(i)]);
    }
    switch (i) {
    case PI_STOCKS: label = "LIVES"; snprintf(buf, sizeof buf, "%d", b->stocks); val = buf; break;
    case PI_STAGE: label = "STAGE"; val = battle_room_name(b->room); break;
    case PI_MUSIC: label = "MUSIC"; val = battle_song_name(b->music); break;
    case PI_WEAPONS: label = "WEAPONS"; val = b->weapons ? "ALL" : "BLASTER ONLY"; break;
    case PI_WSETS: {
      //Whose weapons every the hero has: his own game's, or these games'.
      static const char *sets[8] = {"OWN GAME", "G1", "G2", "G1 G2", "G3", "G1 G3", "G2 G3", "G1 G2 G3"};
      label = "WEAPON SETS";
      val = sets[b->weapon_games & 7];
      break;
    }
    case PI_ITEMS: label = "ITEMS"; val = b->items ? "ON" : "OFF"; break;
    case PI_TIME: {
      static const char *times[BT_COUNT] = {"OFF", "2 MIN", "3 MIN", "5 MIN"};
      label = "TIME";
      val = times[b->time >= 0 && b->time < BT_COUNT ? b->time : 0];
      break;
    }
    case PI_VIEW: {
      static const char *views[BV_COUNT] = {"NORMAL", "LARGE", "HUGE"};
      label = "VIEW";
      val = views[b->view >= 0 && b->view < BV_COUNT ? b->view : 0];
      break;
    }
    case PI_FIGHT:
      ui_text_c(c, W / 2, y, loading ? "LOADING" : fighters(b) >= 2 ? "START" : "NEED 2 PLAYERS", pal, 1);
      break;
    case PI_BACK: ui_text_c(c, W / 2, y, "BACK", pal, 1); break;
    }
    if (label) ui_text(c, lx, y, label, pal, 1);
    if (val) ui_text(c, W - 24 - ui_text_w(val, 1), y, val, pal, 1);
  }
  //Between START and BACK: a boss is in the match.
  for (int k = 0; k < BATTLE_SLOTS; k++)
    if (b->slot[k] != BS_OFF && battle_character_experimental(b->fighter[k])) {
      ui_text_c(c, W / 2, 25 * 8, "BOSSES ARE EXPERIMENTAL", UI_TEXT, 1);
      break;
    }
}

//---------------------------------------------------------------- main page

enum { MI_PLAY, MI_BATTLE, MI_OPTIONS, MI_QUIT, MI_COUNT };

static void draw_main(UiCanvas *c) {
  char buf[40];
  snprintf(buf, sizeof buf, "%s", names[sel_game]);
  int tw = ui_text_w(buf, 1);
  ui_text_c(c, W / 2, 146, buf, UI_SEL, 1);
  int ng = 0;
  for (int g = 1; g <= 3; g++) ng += T[g].avail;
  if (ng > 1) {
    ui_text(c, W / 2 - tw / 2 - 20, 146, "<", UI_SEL, 1);
    ui_text(c, W / 2 + tw / 2 + 12, 146, ">", UI_SEL, 1);
  }
  static const char *items[MI_COUNT] = {"PLAY", "BATTLE MODE", "OPTIONS", "QUIT"};
  for (int i = 0; i < MI_COUNT; i++) ui_text_c(c, W / 2, 166 + i * 12, items[i], i == cursor ? UI_SEL : UI_TEXT, 1);
}

static int loading;

int launcher_frame(uint16_t any) {
  if (!open_) return LA_NONE;
  uint16_t pressed = any & (uint16_t)~prev_any;
  prev_any = any;
  //Games not booted yet come up in the background.
  for (int g = 1; g <= 3; g++)
    if (T[g].avail && !T[g].booted && g != sel_game) { boot_step(&T[g], g, BOOT_STEP); break; }
  if (sel_game && !T[sel_game].booted) boot_step(&T[sel_game], sel_game, BOOT_FRAMES);
  //The selected game's song, from its start when the game changes and
  //again once it has ended (1.5 s of silence).
  if (sel_game && T[sel_game].booted && (snd_game != sel_game || (heard && quiet > APU_SAMPLE_RATE * 3 / 2))) play_song(sel_game);
  if (snd_game) snd_frame(&snd);
  bool up = pressed & PAD_UP, down = pressed & PAD_DOWN, left = pressed & PAD_LEFT, right = pressed & PAD_RIGHT;
  bool ok = pressed & (PAD_B | PAD_A | PAD_START), back = pressed & (PAD_Y | PAD_SELECT);
  int act = LA_NONE;
  if (loading > 0) {
    if (--loading == 0) act = LA_BATTLE;
  } else if (page == 0) {
    if (up) { cursor = cycle(cursor, MI_COUNT, -1); sfx(SFX_MOVE); }
    else if (down) { cursor = cycle(cursor, MI_COUNT, 1); sfx(SFX_MOVE); }
    else if (left || right) sel_game = next_game(sel_game, right ? 1 : -1);
    else if (ok) {
      if (cursor != MI_PLAY && cursor != MI_QUIT) sfx(SFX_OK);
      if (cursor == MI_PLAY && sel_game && T[sel_game].booted) act = LA_PLAY;
      else if (cursor == MI_BATTLE && g_port.battle) { page = 1; psel = PI_FIGHT; }
      else if (cursor == MI_OPTIONS) act = LA_OPTIONS;
      else if (cursor == MI_QUIT) act = LA_QUIT;
    }
  } else {
    BattleSetup *b = g_port.battle;
    if (up) { move(-1); sfx(SFX_MOVE); }
    else if (down) { move(1); sfx(SFX_MOVE); }
    else if (left || right) { change(psel, right ? 1 : -1); sfx(SFX_MOVE); }
    else if (back || (ok && psel == PI_BACK)) { page = 0; sfx(SFX_BACK); }
    else if (ok && (psel == PI_FIGHT || (pressed & PAD_START))) {
      if (fighters(b) >= 2) { psel = PI_FIGHT; loading = 2; sfx(SFX_OK); }
      else sfx(SFX_BACK);
    } else if (ok) { change(psel, 1); sfx(SFX_MOVE); }
  }
  //Picture.
  UiCanvas c = {&pic[0][0], PPU_MAXW, W, UI_H};
  const Title *t = &T[sel_game];
  for (int y = 0; y < UI_H; y++) {
    if (sel_game && t->have_pic) memcpy(pic[y], t->pic[y], sizeof(t->pic[y]));
    else memset(pic[y], 0, W * 4);
  }
  if (page == 0) draw_main(&c);
  else draw_page(&c, loading > 0);
  return act;
}

const uint32_t *launcher_picture(int *w, int *h) {
  *w = W;
  *h = UI_H;
  return &pic[0][0];
}

#define QUIET_LEVEL 24

int launcher_audio(int16_t *out, int max_frames) {
  if (!open_ || !snd_game) return 0;
  int n = snd_mix(&snd, out, max_frames);
  for (int k = 0; k < n * 2; k++) {
    if (out[k] > QUIET_LEVEL || out[k] < -QUIET_LEVEL) { heard = true; quiet = 0; }
    else quiet += k & 1;
  }
  return n;
}
