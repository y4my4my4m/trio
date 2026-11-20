#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define strcasecmp _stricmp
#define chdir _chdir
//kernel32; prototype instead of <windows.h>, whose macros leak into this file.
__declspec(dllimport) int __stdcall AttachConsole(unsigned long pid);
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#endif
#include "snes/snes.h"
#include "script.h"
#include "game/recomp_rt.h"
#include "game/widescreen.h"
#include "port/enhance.h"
#include "port/overlay.h"
#include "port/rewind.h"
#include "port/saveslots.h"
#include "port/saveslots_ui.h"
#include "port/settings.h"
#include "port/port_api.h"
#include "arena/battle.h"
#include "arena/launcher.h"
#include "arena/ui.h"

//Frontend. Interactive SDL2 window, or --headless for scripted runs.
//Input script format (--input): see script.h.

typedef struct {
  const char *rom_path;
  const char *input_path;
  const char *coverage_path;
  const char *state_path;
  const char *shot_path;
  const char *wav_path;
  int frames;
  int scale;
  bool headless;
  bool interp;
  bool fullscreen;
  int widescreen;          //-1 = from settings
  int scale_set;
} Options;

static Snes g_snes;
static Settings g_cfg;
static Enhance g_enh;
#define SETTINGS_PATH "trio.ini"
//Every game's ROM found at start (index SNES_GAME_*); the active one is
//g_rom, run by g_snes.
static uint8_t *g_roms[4];
static uint32_t g_rom_sizes[4];
static uint8_t *g_rom;
static uint32_t g_rom_size;
static int g_game;
static bool g_recomp;

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
  fclose(f);
  if (size) *size = (size_t)n;
  return buf;
}

static bool write_file(const char *path, const void *data, size_t size) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, size, f) == size;
  fclose(f);
  return ok;
}

static void write_ppm(const char *path, Snes *s) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
  int h = s->ppu.overscan ? PPU_H : 224;
  int w = s->ppu.out_w ? s->ppu.out_w : PPU_W;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint32_t c = s->ppu.frame[y][x];
      uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

static bool save_state_file(const char *path) {
  size_t n = snes_state_size();
  uint8_t *buf = malloc(n);
  snes_state_save(&g_snes, buf);
  bool ok = write_file(path, buf, n);
  if (!ok) fprintf(stderr, "cannot write %s\n", path);
  free(buf);
  return ok;
}

static bool load_state_file(const char *path) {
  size_t n;
  uint8_t *buf = read_file(path, &n);
  if (!buf) return false;
  bool ok = n == snes_state_size() && snes_state_load(&g_snes, buf);
  free(buf);
  if (!ok) fprintf(stderr, "%s: not a state for this build\n", path);
  return ok;
}

//---------------------------------------------------------------- WAV

typedef struct { FILE *f; uint32_t frames; } Wav;

static void wav_open(Wav *w, const char *path) {
  w->f = fopen(path, "wb");
  w->frames = 0;
  if (w->f) { uint8_t z[44] = {0}; fwrite(z, 1, 44, w->f); }
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void wav_close(Wav *w) {
  if (!w->f) return;
  uint8_t h[44];
  uint32_t data = w->frames * 4;
  memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
  put32(h + 24, APU_SAMPLE_RATE); put32(h + 28, APU_SAMPLE_RATE * 4);
  h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
  memcpy(h + 36, "data", 4); put32(h + 40, data);
  fseek(w->f, 0, SEEK_SET);
  fwrite(h, 1, 44, w->f);
  fclose(w->f);
  w->f = NULL;
}

//---------------------------------------------------------------- options

static void usage(void) {
  fprintf(stderr,
    "usage: trio [rom.sfc] [options]   (no ROM: game select screen over the games in rom/)\n"
    "  --headless         no window, no audio device\n"
    "  --frames N         stop after N frames (headless default 600)\n"
    "  --input FILE       input script (script.h)\n"
    "  --state FILE       load a save state before running\n"
    "  --shot FILE        write the last frame as PPM\n"
    "  --wav FILE         write audio as WAV (headless)\n"
    "  --coverage FILE    merge executed PCs into FILE (implies --interp)\n"
    "  --scale N          window scale (overrides settings)\n"
    "  --widescreen N     0 off, 1 16:10, 2 16:9, 3 2:1 (overrides settings)\n"
    "  --fullscreen       start fullscreen\n"
    "  --interp           interpreter only (also TRIO_RECOMP=0)\n");
}

static bool parse(int argc, char **argv, Options *o) {
  memset(o, 0, sizeof(*o));
  o->rom_path = NULL;
  o->scale = 0;
  o->widescreen = -1;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    bool more = i + 1 < argc;
    if (!strcmp(a, "--headless")) o->headless = true;
    else if (!strcmp(a, "--interp")) o->interp = true;
    else if (!strcmp(a, "--fullscreen")) o->fullscreen = true;
    else if (!strcmp(a, "--frames") && more) o->frames = atoi(argv[++i]);
    else if (!strcmp(a, "--input") && more) o->input_path = argv[++i];
    else if (!strcmp(a, "--state") && more) o->state_path = argv[++i];
    else if (!strcmp(a, "--shot") && more) o->shot_path = argv[++i];
    else if (!strcmp(a, "--wav") && more) o->wav_path = argv[++i];
    else if (!strcmp(a, "--coverage") && more) o->coverage_path = argv[++i];
    else if (!strcmp(a, "--scale") && more) o->scale = atoi(argv[++i]);
    else if (!strcmp(a, "--widescreen") && more) o->widescreen = atoi(argv[++i]);
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return false; }
    else if (a[0] != '-') o->rom_path = a;
    else { fprintf(stderr, "unknown option %s\n", a); usage(); return false; }
  }
  if (o->headless && !o->frames) o->frames = 600;
  return true;
}

//---------------------------------------------------------------- coverage

static void coverage_merge(const char *path, const uint8_t *cov) {
  //Text '[E]BBAAAA MX', one line per PC and state; merged with an existing file.
  uint8_t *all = calloc(1 << 24, 1);
  FILE *f = fopen(path, "r");
  if (f) {
    char line[64];
    while (fgets(line, sizeof(line), f)) {
      const char *p = line;
      int e = 0;
      if (*p == 'E') { e = 1; p++; }
      unsigned pc, m, x;
      if (sscanf(p, "%6x %1u%1u", &pc, &m, &x) == 3) all[pc & 0xFFFFFF] |= (uint8_t)(1 << (e * 4 + m * 2 + x));
    }
    fclose(f);
  }
  for (uint32_t i = 0; i < (1u << 24); i++) all[i] |= cov[i];
  f = fopen(path, "w");
  if (!f) { free(all); return; }
  for (uint32_t i = 0; i < (1u << 24); i++)
    for (int st = 0; st < 8; st++)
      if (all[i] >> st & 1) fprintf(f, "%s%06X %d%d\n", st & 4 ? "E" : "", i, st >> 1 & 1, st & 1);
  fclose(f);
  free(all);
}

//---------------------------------------------------------------- ROM

//Returns the game (SNES_GAME_*) the ROM was stored as, 0 on failure.
static int load_rom(const char *path, bool quiet) {
  size_t n;
  uint8_t *d = read_file(path, &n);
  if (!d) { if (!quiet) fprintf(stderr, "cannot read ROM %s\n", path); return 0; }
  uint32_t size;
  uint8_t *rom = snes_rom_image(d, n, &size);
  free(d);
  if (!rom) return 0;
  int g = snes_rom_game(rom, size);
  if (g == SNES_GAME_UNKNOWN) {
    fprintf(stderr, "warning: %s is not a supported ROM (rom/README.md)\n", path);
    g = SNES_GAME_G3;
  }
  free(g_roms[g]);
  g_roms[g] = rom;
  g_rom_sizes[g] = size;
  return g;
}

//Every game with a ROM in dir (snes_rom_find: gN.sfc, or any .sfc / .smc
//file told apart by its header). Returns a game found (0 if none).
static int scan_roms(const char *dir) {
  int found = 0;
  for (int g = 1; g <= 3; g++) {
    uint32_t size;
    uint8_t *rom = snes_rom_find(dir, g, &size);
    if (!rom) continue;
    free(g_roms[g]);
    g_roms[g] = rom;
    g_rom_sizes[g] = size;
    found = g;
  }
  return found;
}

static bool is_dir(const char *path) {
#ifdef _WIN32
  struct _finddata_t fd;
  char pat[600];
  snprintf(pat, sizeof pat, "%s/*.*", path);
  intptr_t h = _findfirst(pat, &fd);
  if (h == -1) return false;
  _findclose(h);
  return true;
#else
  DIR *dp = opendir(path);
  if (dp) closedir(dp);
  return dp != NULL;
#endif
}

//rom/, trio.ini and saves/ are relative to the working directory. Started
//from a directory without rom/ (build/, a file manager), the program moves
//to its own directory or one above it (build/, build/Release/), whichever
//has rom/.
static void find_home(void) {
  if (is_dir("rom")) return;
  char *base = SDL_GetBasePath();
  if (!base) return;
  static const char *up[3] = {"", "../", "../../"};
  for (int k = 0; k < 3; k++) {
    char dir[600], rom[640];
    snprintf(dir, sizeof dir, "%s%s", base, up[k]);
    snprintf(rom, sizeof rom, "%srom", dir);
    if (is_dir(rom) && chdir(dir) == 0) break;
  }
  SDL_free(base);
}

static const char *game_title(int g) {
  static const char *t[4] = {"game 1", "game 1", "game 2", "game 3"};
  return t[g & 3];
}

static const char *save_dir(int g) {
  //G3 keeps the directory it always used.
  static const char *d[4] = {"saves", "saves/g1", "saves/g2", "saves"};
  return d[g & 3];
}

//Makes game g the one g_snes runs: from a held title machine of the game
//select screen, or from power-on (from == NULL).
static void activate_game(int g, const Snes *from) {
  g_game = g;
  g_rom = g_roms[g];
  g_rom_size = g_rom_sizes[g];
  if (from) {
    snes_clone(&g_snes, from);
    launcher_resume(&g_snes);
  } else {
    snes_init(&g_snes, g_rom, g_rom_size);
    snes_reset(&g_snes);
  }
  rt_install(&g_snes, g_recomp);
}

//---------------------------------------------------------------- headless

static int run_headless(const Options *o) {
  int ncmd = 0;
  ScriptCmd *cmds = o->input_path ? script_load(o->input_path, &ncmd) : NULL;
  if (o->input_path && !cmds) return 1;
  Wav wav = {0};
  if (o->wav_path) wav_open(&wav, o->wav_path);
  uint16_t held = 0;
  int held_until = 0, ci = 0;
  int16_t buf[4096];
  clock_t t0 = clock();
  for (int fr = 0; fr < o->frames; fr++) {
    for (; ci < ncmd && cmds[ci].frame <= fr; ci++) {
      ScriptCmd *c = &cmds[ci];
      if (c->kind == CMD_PAD) { held = c->buttons; held_until = fr + c->hold; }
      else if (c->kind == CMD_SHOT) write_ppm(c->arg, &g_snes);
      else if (c->kind == CMD_SAVE) save_state_file(c->arg);
      else if (c->kind == CMD_QUIT) fr = o->frames;
    }
    if (fr >= o->frames) break;
    g_snes.pad[0] = enh_input(&g_enh, &g_cfg, fr < held_until ? held : 0);
    snes_run_frame(&g_snes);
    enh_after_frame(&g_enh, &g_snes, &g_cfg);
    int n;
    while ((n = apu_pop_samples(&g_snes.apu, buf, 2048)) > 0)
      if (wav.f) { fwrite(buf, 4, (size_t)n, wav.f); wav.frames += (uint32_t)n; }
  }
  double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  fprintf(stderr, "%d frames in %.0f ms; recompiled %llu, interpreter %llu\n", o->frames, ms,
          (unsigned long long)rt_insn_count, (unsigned long long)rt_interp_count);
  if (o->shot_path) write_ppm(o->shot_path, &g_snes);
  wav_close(&wav);
  free(cmds);
  return 0;
}

//---------------------------------------------------------------- interactive

static const uint16_t bind_bits[BIND_COUNT] = {
  PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_B, PAD_Y, PAD_A, PAD_X, PAD_L, PAD_R, PAD_START, PAD_SELECT,
};

static uint16_t keyboard_pad(const Uint8 *k, const Settings *c) {
  uint16_t p = 0;
  for (int b = 0; b < BIND_COUNT; b++)
    if ((c->keys[b] && k[c->keys[b]]) || (c->keys2[b] && k[c->keys2[b]])) p |= bind_bits[b];
  return p;
}

//Game controllers: every connected pad drives the player.
#define MAX_PADS 4
static SDL_GameController *g_pads[MAX_PADS];

static void pads_open(int device) {
  if (!SDL_IsGameController(device)) return;
  SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
  for (int i = 0; i < MAX_PADS; i++)
    if (g_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) == id) return;
  for (int i = 0; i < MAX_PADS; i++)
    if (!g_pads[i]) { g_pads[i] = SDL_GameControllerOpen(device); return; }
}

static void pads_close(SDL_JoystickID id) {
  for (int i = 0; i < MAX_PADS; i++)
    if (g_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) == id) {
      SDL_GameControllerClose(g_pads[i]);
      g_pads[i] = NULL;
    }
}

static bool pad_input(SDL_GameController *gc, int v) {
  if (v == PAD_TRIGGER_L) return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
  if (v == PAD_TRIGGER_R) return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
  return v >= 0 && SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)v);
}

static int pad_any_bind(const Settings *c, int v) {
  for (int b = 0; b < BIND_COUNT; b++)
    if (c->pad_map[b] == v) return b;
  return -1;
}

//Pad hotkeys while Back is held (when enabled). Returned as flags; the
//buttons involved are hidden from the game while Back is down.
enum { PH_REWIND = 1, PH_FAST = 2, PH_SAVE = 4, PH_LOAD = 8, PH_MENU = 16 };

typedef struct {
  bool back_down, back_used;
  int select_pulse;        //frames of Select to send after a lone Back tap
  int held_prev;
} PadHot;

static PadHot g_padhot;

//SNES buttons of one pad through the configured mapping and the left stick.
static uint16_t pad_bits(SDL_GameController *gc, const Settings *c) {
  uint16_t p = 0;
  if (!gc) return 0;
  for (int b = 0; b < BIND_COUNT; b++)
    if (pad_input(gc, c->pad_map[b])) p |= bind_bits[b];
  int dz = c->deadzone * 32767 / 100;
  int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
  int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
  if (ax < -dz) p |= PAD_LEFT;
  if (ax > dz) p |= PAD_RIGHT;
  if (ay < -dz) p |= PAD_UP;
  if (ay > dz) p |= PAD_DOWN;
  return p;
}

//held: hotkeys held this frame (rewind, fast); edge: pressed this frame.
static uint16_t pads_read(const Settings *c, int *held, int *edge) {
  uint16_t p = 0;
  int hot = 0;
  bool back = false;
  for (int i = 0; i < MAX_PADS; i++) {
    SDL_GameController *gc = g_pads[i];
    if (!gc) continue;
    p |= pad_bits(gc, c);
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE)) hot |= PH_MENU;
    if (c->pad_hotkeys && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) {
      back = true;
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) hot |= PH_REWIND;
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) hot |= PH_FAST;
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y)) hot |= PH_SAVE;
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X)) hot |= PH_LOAD;
      if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)) hot |= PH_MENU;
    }
  }
  PadHot *h = &g_padhot;
  if (c->pad_hotkeys) {
    //Back is a modifier: the game gets Select only for a lone tap of Back.
    int sel = pad_any_bind(c, SDL_CONTROLLER_BUTTON_BACK);
    uint16_t sel_bit = sel >= 0 ? bind_bits[sel] : 0;
    if (back) {
      if (hot & (PH_REWIND | PH_FAST | PH_SAVE | PH_LOAD | PH_MENU)) h->back_used = true;
      p = 0;  //nothing reaches the game while Back is down
    } else if (h->back_down) {
      if (!h->back_used && sel_bit) h->select_pulse = 3;
      h->back_used = false;
    }
    h->back_down = back;
    if (h->select_pulse > 0) { h->select_pulse--; p |= sel_bit; }
  }
  *held = hot;
  *edge = hot & ~h->held_prev;
  h->held_prev = hot;
  return p;
}

//---------------------------------------------------------------- battle

//Battle mode (docs/BATTLE.md): picked on the game select screen, run here in
//place of the game until the player leaves it.
static BattleSetup g_battle = {{BS_PLAYER, BS_CPU2, BS_OFF, BS_OFF}, 3, 0, true, {0, 0, 0, 0}, 0, true, BV_LARGE, BT_OFF, 0};
static bool g_in_battle, g_in_launcher;
static bool g_battle_release;  //after a match: the game sees no buttons until all are let go

//The setup page as last used, kept in the settings file.
static void battle_setup_load(const Settings *c) {
  if (c->battle_stocks <= 0) return;
  BattleSetup *b = &g_battle;
  for (int i = 0; i < BATTLE_SLOTS && i < 4; i++) {
    if (c->battle_slot[i] >= 0 && c->battle_slot[i] < BS_COUNT) b->slot[i] = c->battle_slot[i];
    b->fighter[i] = c->battle_fighter[i] >= 0 && c->battle_fighter[i] < battle_characters() ? c->battle_fighter[i] : 0;
  }
  b->stocks = c->battle_stocks > 9 ? 9 : c->battle_stocks;
  b->room = c->battle_room >= 0 && c->battle_room < battle_rooms() ? c->battle_room : 0;
  b->music = c->battle_music >= 0 && c->battle_music < battle_songs() ? c->battle_music : 0;
  b->view = c->battle_view >= 0 && c->battle_view < BV_COUNT ? c->battle_view : BV_LARGE;
  b->time = c->battle_time >= 0 && c->battle_time < BT_COUNT ? c->battle_time : BT_OFF;
  b->weapons = c->battle_weapons;
  b->weapon_games = c->battle_weapon_games & 7;
  b->items = c->battle_items;
}

static void battle_setup_save(Settings *c) {
  const BattleSetup *b = &g_battle;
  for (int i = 0; i < BATTLE_SLOTS && i < 4; i++) { c->battle_slot[i] = b->slot[i]; c->battle_fighter[i] = b->fighter[i]; }
  c->battle_stocks = b->stocks;
  c->battle_room = b->room;
  c->battle_music = b->music;
  c->battle_view = b->view;
  c->battle_time = b->time;
  c->battle_weapons = b->weapons;
  c->battle_weapon_games = b->weapon_games;
  c->battle_items = b->items;
  settings_save(c, SETTINGS_PATH);
}

//Second keyboard player: W A S D move, F jump (B), G shoot (Y), H dash (A),
//J sword (X), Q / E weapon (L / R), Tab pause. These keys are taken away from KEYS 1
//while a KEYS 2 slot is in the match.
static const struct { SDL_Scancode k; uint16_t bit; } keys2_map[] = {
  {SDL_SCANCODE_W, PAD_UP}, {SDL_SCANCODE_S, PAD_DOWN}, {SDL_SCANCODE_A, PAD_LEFT}, {SDL_SCANCODE_D, PAD_RIGHT},
  {SDL_SCANCODE_F, PAD_B}, {SDL_SCANCODE_G, PAD_Y}, {SDL_SCANCODE_H, PAD_A}, {SDL_SCANCODE_J, PAD_X},
  {SDL_SCANCODE_Q, PAD_L}, {SDL_SCANCODE_E, PAD_R}, {SDL_SCANCODE_TAB, PAD_START},
};

static bool is_keys2(int sc) {
  for (size_t i = 0; i < sizeof(keys2_map) / sizeof(keys2_map[0]); i++)
    if ((int)keys2_map[i].k == sc) return true;
  return false;
}

static int slot_device(int t) {
  if (t == BS_KEYS1) return 0;
  if (t == BS_KEYS2) return 1;
  if (t >= BS_PAD1 && t <= BS_PAD4) return 2 + t - BS_PAD1;
  return -1;
}

//Devices 0 KEYS 1 (the configured keys), 1 KEYS 2, 2-5 pads 1-4. A single
//PLAYER slot gets every device no other slot claims. With several, they
//share them out instead of all moving together: the first PLAYER gets
//KEYS 1, the second KEYS 2, and the free connected pads go one each in
//slot order.
static void battle_devices(bool owner[BATTLE_SLOTS][2 + MAX_PADS]) {
  memset(owner, 0, sizeof(bool) * BATTLE_SLOTS * (2 + MAX_PADS));
  bool owned[2 + MAX_PADS] = {0};
  int players = 0;
  for (int i = 0; i < BATTLE_SLOTS; i++) {
    int d = slot_device(g_battle.slot[i]);
    if (d >= 0) { owner[i][d] = true; owned[d] = true; }
    players += g_battle.slot[i] == BS_PLAYER;
  }
  int k = 0, pad = 0;
  for (int i = 0; i < BATTLE_SLOTS; i++) {
    if (g_battle.slot[i] != BS_PLAYER) continue;
    if (players == 1) {
      for (int d = 0; d < 2 + MAX_PADS; d++) owner[i][d] = !owned[d];
      return;
    }
    if (k < 2 && !owned[k]) owner[i][k] = true;
    while (pad < MAX_PADS && (owned[2 + pad] || !g_pads[pad])) pad++;
    if (pad < MAX_PADS) owner[i][2 + pad++] = true;
    k++;
  }
}

static void battle_pads(const Settings *c, const Uint8 *k, uint16_t pads[BATTLE_SLOTS], uint16_t *any) {
  bool owner[BATTLE_SLOTS][2 + MAX_PADS];
  battle_devices(owner);
  bool keys2 = false;
  for (int i = 0; i < BATTLE_SLOTS; i++) keys2 |= owner[i][1] && g_battle.slot[i] != BS_PLAYER;
  //A lone PLAYER owning both keyboards keeps the whole keyboard as KEYS 1.
  for (int i = 0; i < BATTLE_SLOTS; i++) keys2 |= owner[i][1] && !owner[i][0];
  uint16_t dev[2 + MAX_PADS] = {0};
  for (int b = 0; b < BIND_COUNT; b++) {
    int k1 = c->keys[b], k2 = c->keys2[b];
    if ((k1 && k[k1] && !(keys2 && is_keys2(k1))) || (k2 && k[k2] && !(keys2 && is_keys2(k2)))) dev[0] |= bind_bits[b];
  }
  if (keys2)
    for (size_t i = 0; i < sizeof(keys2_map) / sizeof(keys2_map[0]); i++)
      if (k[keys2_map[i].k]) dev[1] |= keys2_map[i].bit;
  for (int i = 0; i < MAX_PADS; i++) dev[2 + i] = pad_bits(g_pads[i], c);
  *any = 0;
  for (int d = 0; d < 2 + MAX_PADS; d++) *any |= dev[d];
  for (int i = 0; i < BATTLE_SLOTS; i++) {
    uint16_t p = 0;
    for (int d = 0; d < 2 + MAX_PADS; d++)
      if (owner[i][d]) p |= dev[d];
    if ((p & (PAD_LEFT | PAD_RIGHT)) == (PAD_LEFT | PAD_RIGHT)) p &= (uint16_t)~(PAD_LEFT | PAD_RIGHT);
    if ((p & (PAD_UP | PAD_DOWN)) == (PAD_UP | PAD_DOWN)) p &= (uint16_t)~(PAD_UP | PAD_DOWN);
    pads[i] = p;
  }
}

static void open_launcher(void);

//Back to the game select screen.
static bool g_window_restore;  //a match resized the window: back to the configured size
static void battle_leave(const Settings *cfg) {
  battle_end();
  g_in_battle = false;
  g_window_restore = true;
  g_battle_release = true;
  g_enh.wide_cfg = -1;
  enh_apply(&g_enh, &g_snes, cfg);
  open_launcher();
}

//The canvas is sized for the configured widescreen width, so the window
//does not jump when widescreen switches between stages and menus; 4:3
//frames are centered in it.
typedef struct { SDL_Rect content; float px_w, px_h; } Layout;

static Layout compute_layout(SDL_Renderer *ren, const Settings *cfg, int out_w, int h) {
  Layout L;
  int ow, oh;
  SDL_GetRendererOutputSize(ren, &ow, &oh);
  float par = cfg->pixel_aspect ? 8.0f / 7.0f : 1.0f;
  int canvas_w = PPU_W + 2 * (enh_widescreen_supported() ? settings_wide_columns(cfg->widescreen, cfg->pixel_aspect) : 0);
  if (out_w > canvas_w) canvas_w = out_w;
  float sx = (float)ow / (canvas_w * par), sy = (float)oh / h;
  float sc = sx < sy ? sx : sy;
  if (cfg->integer_scale && sc >= 1.0f) sc = (float)(int)sc;
  L.px_w = sc * par;
  L.px_h = sc;
  L.content.w = (int)(out_w * L.px_w + 0.5f);
  L.content.h = (int)(h * L.px_h + 0.5f);
  L.content.x = (ow - L.content.w) / 2;
  L.content.y = (oh - L.content.h) / 2;
  return L;
}

static void apply_window(SDL_Window *win, SDL_Renderer *ren, SDL_Texture *tex, const Settings *cfg, bool resize) {
  SDL_SetWindowFullscreen(win, cfg->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  SDL_RenderSetVSync(ren, cfg->vsync);
  SDL_SetTextureScaleMode(tex, cfg->filter == FILTER_LINEAR ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
  if (resize && !cfg->fullscreen) {
    float par = cfg->pixel_aspect ? 8.0f / 7.0f : 1.0f;
    int cw = PPU_W + 2 * (enh_widescreen_supported() ? settings_wide_columns(cfg->widescreen, cfg->pixel_aspect) : 0);
    SDL_SetWindowSize(win, (int)(cw * par * cfg->scale), 224 * cfg->scale);
  }
}

//A match picture taller than the canvas keeps the window's pixel scale:
//the window grows to show it, as far as the desktop allows.
static void battle_window(SDL_Window *win, const Settings *cfg) {
  int bw, bh, ww, wh;
  battle_picture(&bw, &bh);
  if (cfg->fullscreen || (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) || bh <= 224) return;
  SDL_GetWindowSize(win, &ww, &wh);
  float par = cfg->pixel_aspect ? 8.0f / 7.0f : 1.0f, sc = (float)wh / 224;
  SDL_Rect ub;
  if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win), &ub) == 0) {
    float fit = (float)(ub.h - 48) / bh, fw = (float)ub.w / (bw * par);
    if (fw < fit) fit = fw;
    if (sc > fit) sc = cfg->integer_scale && fit >= 1.0f ? (float)(int)fit : fit;
  }
  SDL_SetWindowSize(win, (int)(bw * par * sc + 0.5f), (int)(bh * sc + 0.5f));
}

static void screenshot_png(void) {
  int h = g_snes.ppu.overscan ? PPU_H : 224, w = g_snes.ppu.out_w ? g_snes.ppu.out_w : PPU_W;
  uint8_t *rgb = malloc((size_t)w * h * 3);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint32_t c = g_snes.ppu.frame[y][x];
      uint8_t *o = rgb + ((size_t)y * w + x) * 3;
      o[0] = (uint8_t)(c >> 16); o[1] = (uint8_t)(c >> 8); o[2] = (uint8_t)c;
    }
  char path[64];
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  strftime(path, sizeof(path), "screenshot-%Y%m%d-%H%M%S.png", tm);
  png_write_rgb(path, rgb, w, h);
  free(rgb);
}

//TRIO_UI_SHOT="n:path.bmp" dumps the composed window (game + UI) at the n-th
//presented frame and quits; TRIO_UI_MENU=1 opens the menu at start.
static bool ui_shot_maybe(SDL_Renderer *ren) {
  static int presented;
  const char *env = getenv("TRIO_UI_SHOT");
  int at;
  char path[512];
  if (!env || sscanf(env, "%d:%511s", &at, path) != 2 || ++presented < at) return false;
  int w, h;
  SDL_GetRendererOutputSize(ren, &w, &h);
  SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
  SDL_SaveBMP(sf, path);
  SDL_FreeSurface(sf);
  return true;
}

//Settings edited in the in-game PORT OPTIONS page (src/game/port_menu.c):
//applied after the frame, since some of them resize the window.
static bool g_game_cfg_dirty;
static void game_settings_changed(void) { g_game_cfg_dirty = true; }

static void on_state_changed(void) {
  enh_reset(&g_enh);
  rw_clear();
  ss_auto_rebase(&g_snes, true);
}

static bool slot_do(int op, int slot, char *status, size_t n, uint32_t *until) {
  bool loaded = false;
  if (slot < 0) snprintf(status, n, "No autosave");
  else if ((loaded = ss_do(op, slot, &g_snes, status, n))) on_state_changed();
  *until = SDL_GetTicks() + 1500;
  return loaded;
}

static SDL_Window *g_win;
static LauncherGame g_lgames[4];

//Starts playing game g (see activate_game) with the port's features bound
//to it: widescreen patches, save slots, rewind history.
static void enter_game(int g, const Snes *from) {
  activate_game(g, from);
  enh_init(&g_enh, &g_snes, g_rom, g_rom_size);
  g_enh.wide_cfg = -1;
  enh_apply(&g_enh, &g_snes, &g_cfg);
  rw_clear();
  ss_init(save_dir(g), g_rom, g_rom_size);
  g_in_launcher = false;
  if (g_win) SDL_SetWindowTitle(g_win, game_title(g));
}

static void open_launcher(void) {
  for (int g = 1; g <= 3; g++) g_lgames[g] = (LauncherGame){g_roms[g], g_rom_sizes[g], g_recomp};
  launcher_open(g_lgames, g_game);
  g_in_launcher = true;
  if (g_win) SDL_SetWindowTitle(g_win, "Trio");
}

static int run_window(const Options *o) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  Settings *cfg = &g_cfg;
  Settings before;
  g_port.settings = cfg;
  g_port.settings_changed = game_settings_changed;
  g_port.open_menu = overlay_toggle;
  g_port.battle = &g_battle;
  float par = cfg->pixel_aspect ? 8.0f / 7.0f : 1.0f;
  int cw = PPU_W + 2 * (enh_widescreen_supported() ? settings_wide_columns(cfg->widescreen, cfg->pixel_aspect) : 0);
  SDL_Window *win = SDL_CreateWindow(g_in_launcher ? "Trio" : game_title(g_game), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     (int)(cw * par * cfg->scale), 224 * cfg->scale,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                     (cfg->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  g_win = win;
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (cfg->vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
  if (!win || !ren) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                       PPU_MAXW > BATTLE_MAXW ? PPU_MAXW : BATTLE_MAXW, PPU_H > BATTLE_MAXH ? PPU_H : BATTLE_MAXH);
  apply_window(win, ren, tex, cfg, false);
  overlay_init(win, ren);
  before = *cfg;
  if (getenv("TRIO_UI_MENU")) overlay_toggle();

  SDL_AudioSpec want = {0}, have;
  want.freq = APU_SAMPLE_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (audio) SDL_PauseAudioDevice(audio, 0);

  for (int i = 0; i < SDL_NumJoysticks(); i++) pads_open(i);

  //--input in a window: the script's buttons come in as if from a pad (tests
  //with SDL_VIDEODRIVER=dummy and TRIO_UI_SHOT).
  int ncmd = 0, ci = 0, held_until = 0, frame_no = 0;
  uint16_t held = 0;
  ScriptCmd *cmds = o->input_path ? script_load(o->input_path, &ncmd) : NULL;
  bool running = true, paused = false, step_one = false;
  char status[128] = "";
  uint32_t status_until = 0;
  int16_t buf[4096];
  uint64_t next = SDL_GetPerformanceCounter();
  const double freq = (double)SDL_GetPerformanceFrequency();
  const double frame_s = 1.0 / 60.0988;
  uint32_t fps_t0 = SDL_GetTicks();
  int fps_frames = 0;
  float fps = 0;

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) { running = false; continue; }
      if (ev.type == SDL_CONTROLLERDEVICEADDED) pads_open(ev.cdevice.which);
      else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) pads_close(ev.cdevice.which);
      bool menu_key = ev.type == SDL_KEYDOWN && !ev.key.repeat && !overlay_capturing_key() &&
                      (ev.key.keysym.sym == SDLK_F1 || ev.key.keysym.sym == SDLK_ESCAPE);
      if (menu_key) { overlay_toggle(); continue; }
      if (overlay_event(&ev, cfg)) continue;
      if (ev.type != SDL_KEYDOWN || ev.key.repeat) continue;
      SDL_Keycode k = ev.key.keysym.sym;
      SDL_Keymod mod = ev.key.keysym.mod;
      bool fs_key = k == SDLK_F11 || (k == SDLK_RETURN && (mod & KMOD_ALT));
      if ((g_in_battle || g_in_launcher) && !fs_key) continue;  //states, pause and rewind act on the game, not the screens
      if (k == SDLK_p) paused = !paused;
      else if (k == SDLK_o) { paused = true; step_one = true; }
      else if (k == SDLK_F5) slot_do(SS_OP_SAVE, cfg->save_slot, status, sizeof(status), &status_until);
      else if (k == SDLK_F9)
        slot_do(SS_OP_LOAD, (mod & KMOD_SHIFT) ? ss_auto_newest() : cfg->save_slot, status, sizeof(status), &status_until);
      else if (k == SDLK_F6 || k == SDLK_F7) {
        cfg->save_slot = (cfg->save_slot + (k == SDLK_F7 ? 1 : SS_MANUAL)) % (SS_MANUAL + 1);
        char name[16];
        snprintf(status, sizeof(status), "Slot: %s", ss_name(cfg->save_slot, name, sizeof(name)));
        status_until = SDL_GetTicks() + 1500;
        settings_save(cfg, SETTINGS_PATH);
      } else if (k == SDLK_F12) {
        screenshot_png();
        snprintf(status, sizeof(status), "Screenshot saved");
        status_until = SDL_GetTicks() + 1500;
      } else if (k == SDLK_r && (mod & KMOD_CTRL)) { snes_reset(&g_snes); on_state_changed(); }
      else if (k == SDLK_F11 || (k == SDLK_RETURN && (mod & KMOD_ALT))) {
        cfg->fullscreen = !cfg->fullscreen;
        apply_window(win, ren, tex, cfg, false);
        settings_save(cfg, SETTINGS_PATH);
      }
    }

    bool focus = SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    bool menu = overlay_is_open();
    uint16_t pad = 0;
    int pad_held = 0, pad_edge = 0;
    uint16_t padbits = focus ? pads_read(cfg, &pad_held, &pad_edge) : 0;
    for (; ci < ncmd && cmds[ci].frame <= frame_no; ci++)
      if (cmds[ci].kind == CMD_PAD) { held = cmds[ci].buttons; held_until = frame_no + cmds[ci].hold; }
      else if (cmds[ci].kind == CMD_QUIT) running = false;
    if (frame_no < held_until) padbits |= held;
    frame_no++;
    if (cmds) focus = true;
    if (pad_edge & PH_MENU) { overlay_toggle(); menu = overlay_is_open(); }
    bool screen = g_in_battle || g_in_launcher;  //a frontend screen runs instead of the game
    if (!menu && focus && !screen) {
      bool alt = ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT];
      pad = keyboard_pad(ks, cfg);
      if (alt) pad &= (uint16_t)~PAD_START;  //Alt+Enter is fullscreen
      pad |= padbits;
      if (pad_edge & PH_SAVE) slot_do(SS_OP_SAVE, cfg->save_slot, status, sizeof(status), &status_until);
      if (pad_edge & PH_LOAD) slot_do(SS_OP_LOAD, cfg->save_slot, status, sizeof(status), &status_until);
    }
    //The button that left the match must not reach the setup page.
    if (g_battle_release && !screen) {
      if (pad) pad = 0;
      else g_battle_release = false;
    }
    //No opposite directions at once (the game never sees them on hardware).
    if ((pad & (PAD_LEFT | PAD_RIGHT)) == (PAD_LEFT | PAD_RIGHT)) pad &= (uint16_t)~(PAD_LEFT | PAD_RIGHT);
    if ((pad & (PAD_UP | PAD_DOWN)) == (PAD_UP | PAD_DOWN)) pad &= (uint16_t)~(PAD_UP | PAD_DOWN);

    bool fast = focus && !menu && !screen && (ks[SDL_SCANCODE_TAB] || (pad_held & PH_FAST));
    bool rewinding = focus && !menu && !screen && cfg->rewind &&
                     ((ks[SDL_SCANCODE_R] && !(SDL_GetModState() & KMOD_CTRL)) || (pad_held & PH_REWIND));
    bool halted = menu || (paused && !step_one) || (!focus && cfg->pause_unfocused);
    int steps = halted ? 0 : fast ? cfg->ff_speed : 1;
    bool rewound = false;
    if (rewinding && !menu) {
      //Two history entries per presented frame: a bit faster than real time.
      for (int i = 0; i < 2; i++) rewound |= rw_step_back(&g_snes);
      if (rewound) {
        snes_run_frame(&g_snes);
        enh_after_frame(&g_enh, &g_snes, cfg);
        while (apu_pop_samples(&g_snes.apu, buf, 2048) > 0) {}
      }
      steps = 0;
    }
    if (g_in_launcher) {
      uint16_t bpads[BATTLE_SLOTS] = {0}, any = 0;
      if (focus && !menu) {
        battle_pads(cfg, ks, bpads, &any);
        any |= padbits;
        if (ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT]) any &= (uint16_t)~PAD_START;
      }
      for (int i = 0; i < steps && g_in_launcher; i++) {
        int act = launcher_frame(any);
        int n;
        bool mute = cfg->mute_unfocused && !focus;
        while ((n = launcher_audio(buf, 2048)) > 0) {
          if (!audio || mute) continue;
          if (cfg->volume < 100)
            for (int j = 0; j < n * 2; j++) buf[j] = (int16_t)(buf[j] * cfg->volume / 100);
          SDL_QueueAudio(audio, buf, (Uint32)n * 4);
        }
        fps_frames++;
        if (act == LA_PLAY) {
          int g = launcher_game();
          enter_game(g, launcher_machine(g));
          g_battle_release = true;
        } else if (act == LA_BATTLE) {
          int bw = settings_wide_columns(cfg->widescreen, cfg->pixel_aspect);
          battle_setup_save(cfg);
          if (battle_begin(&g_battle, (const uint8_t *const *)g_roms, g_rom_sizes, g_recomp, bw)) {
            g_in_launcher = false;
            g_in_battle = true;
            battle_window(win, cfg);
          } else {
            snprintf(status, sizeof(status), "Battle mode could not start");
            status_until = SDL_GetTicks() + 3000;
          }
        } else if (act == LA_OPTIONS) {
          overlay_toggle();
        } else if (act == LA_QUIT) {
          running = false;
        }
      }
      steps = 0;
    }
    if (g_in_battle) {
      uint16_t bpads[BATTLE_SLOTS] = {0}, any = 0;
      if (focus && !menu) {
        battle_pads(cfg, ks, bpads, &any);
        if (ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT]) any &= (uint16_t)~PAD_START;
      }
      for (int i = 0; i < steps && g_in_battle; i++) {
        if (!battle_frame(bpads, any)) { battle_leave(cfg); break; }
        int n;
        bool mute = cfg->mute_unfocused && !focus;
        while ((n = battle_audio(buf, 2048)) > 0) {
          if (!audio || mute) continue;
          if (cfg->volume < 100)
            for (int j = 0; j < n * 2; j++) buf[j] = (int16_t)(buf[j] * cfg->volume / 100);
          SDL_QueueAudio(audio, buf, (Uint32)n * 4);
        }
        fps_frames++;
      }
      steps = g_in_battle ? steps : 0;
    }
    for (int i = 0; i < steps && !g_in_battle; i++) {
      g_snes.pad[0] = enh_input(&g_enh, cfg, pad);
      snes_run_frame(&g_snes);
      enh_after_frame(&g_enh, &g_snes, cfg);
      if (cfg->rewind) rw_record(&g_snes);
      if (g_enh.stage_entered) ss_auto_stage_entered();
      ss_set_location(g_enh.in_stage ? "In stage" : "Menu");
      int a = ss_auto_tick(&g_snes, cfg->autosave, cfg->autosave_minutes, g_enh.in_stage);
      if (a >= 0) {
        char name[16];
        snprintf(status, sizeof(status), "Autosaved: %s", ss_name(a, name, sizeof(name)));
        status_until = SDL_GetTicks() + 1500;
      }
      int n;
      bool mute = fast || (cfg->mute_unfocused && !focus);
      while ((n = apu_pop_samples(&g_snes.apu, buf, 2048)) > 0) {
        if (!audio || mute) continue;
        if (cfg->volume < 100)
          for (int j = 0; j < n * 2; j++) buf[j] = (int16_t)(buf[j] * cfg->volume / 100);
        SDL_QueueAudio(audio, buf, (Uint32)n * 4);
      }
      fps_frames++;
    }
    step_one = false;

    int h = g_snes.ppu.overscan ? PPU_H : 224;
    int w = g_snes.ppu.out_w ? g_snes.ppu.out_w : PPU_W;
    if (g_window_restore) {
      g_window_restore = false;
      apply_window(win, ren, tex, cfg, !(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED));
    }
    const uint32_t *frame = &g_snes.ppu.frame[0][0];
    int stride = PPU_MAXW;
    if (g_in_battle) { frame = battle_picture(&w, &h); stride = BATTLE_MAXW; }
    else if (g_in_launcher) frame = launcher_picture(&w, &h);
    void *px;
    int pitch;
    if (SDL_LockTexture(tex, NULL, &px, &pitch) == 0) {
      for (int y = 0; y < h; y++) memcpy((uint8_t *)px + y * pitch, frame + (size_t)y * stride, (size_t)w * 4);
      SDL_UnlockTexture(tex);
    }
    Layout L = compute_layout(ren, cfg, w, h);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_Rect src = {0, 0, w, h};
    SDL_RenderCopy(ren, tex, &src, &L.content);
    if (cfg->scanlines > 0 && L.px_h >= 2) {
      //Darken the lower part of every SNES line.
      SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(ren, 0, 0, 0, (Uint8)(cfg->scanlines * 200 / 100));
      int band = (int)(L.px_h / 2);
      for (int y = 0; y < h; y++) {
        SDL_Rect r = {L.content.x, L.content.y + (int)(y * L.px_h + L.px_h) - band, L.content.w, band};
        SDL_RenderFillRect(ren, &r);
      }
    }

    uint32_t now = SDL_GetTicks();
    if (now - fps_t0 >= 1000) { fps = fps_frames * 1000.0f / (float)(now - fps_t0); fps_frames = 0; fps_t0 = now; }
    OverlayInfo info = {0};
    info.fps = fps;
    info.status = now < status_until ? status : (paused && !menu ? "Paused (P)" : "");
    info.widescreen_ok = enh_widescreen_supported() && g_snes.exec_hook;
    info.in_stage = g_enh.in_stage;
    info.rewind_states = rw_count();
    info.rewind_bytes = rw_bytes();
    info.rewinding = rewinding;
    info.can_game_select = !g_in_launcher;
    int act = overlay_frame(cfg, &info);
    if (g_game_cfg_dirty) { act |= OV_SETTINGS | OV_WINDOW; g_game_cfg_dirty = false; }
    SDL_RenderPresent(ren);
    if (ui_shot_maybe(ren)) running = false;

    if (act & (OV_SETTINGS | OV_WINDOW)) {
      //A match keeps its own widescreen patches; battle_leave applies these.
      if (!g_in_battle) enh_apply(&g_enh, &g_snes, cfg);
      rw_set_capacity(cfg->rewind_seconds);
      if (!cfg->rewind && before.rewind) rw_clear();
      bool resize = cfg->scale != before.scale || cfg->widescreen != before.widescreen ||
                    cfg->pixel_aspect != before.pixel_aspect;
      if (act & OV_WINDOW) apply_window(win, ren, tex, cfg, resize);
      settings_save(cfg, SETTINGS_PATH);
    }
    before = *cfg;
    //The game select screen has no game to reset, save or load.
    if (g_in_launcher) act &= ~(OV_RESET | OV_SAVE_STATE | OV_LOAD_STATE);
    if (act & OV_GAME_SELECT) {
      if (g_in_battle) battle_leave(cfg);
      else open_launcher();
    }
    //Anything done to the game from the menu ends a match first.
    if (g_in_battle && (act & (OV_RESET | OV_LOAD_STATE))) { battle_leave(cfg); g_in_launcher = false; }
    if (act & OV_RESET) { snes_reset(&g_snes); on_state_changed(); }
    if (act & OV_SAVE_STATE) slot_do(SS_OP_SAVE, cfg->save_slot, status, sizeof(status), &status_until);
    if (act & OV_LOAD_STATE) slot_do(SS_OP_LOAD, cfg->save_slot, status, sizeof(status), &status_until);
    int op, sl;
    bool took = ssui_take_request(&op, &sl);
    if (took && g_in_launcher) took = false;
    if (took && g_in_battle && op == SS_OP_LOAD) { battle_leave(cfg); g_in_launcher = false; }
    if (took && slot_do(op, sl, status, sizeof(status), &status_until) && overlay_is_open())
      overlay_toggle();
    if (act & OV_QUIT) running = false;

    //Pace on the audio queue while audio plays, else on the clock.
    if (audio && steps == 1 && !(cfg->mute_unfocused && !focus)) {
      while (SDL_GetQueuedAudioSize(audio) > (Uint32)(APU_SAMPLE_RATE / 60 * 4 * 3)) SDL_Delay(1);
    } else if (!fast || steps == 0) {
      next += (uint64_t)(frame_s * freq);
      uint64_t tnow = SDL_GetPerformanceCounter();
      if (next > tnow) SDL_Delay((Uint32)((next - tnow) * 1000 / freq));
      else next = tnow;
    }
  }
  if (g_in_battle) battle_leave(cfg);
  settings_save(cfg, SETTINGS_PATH);
  overlay_shutdown();
  for (int i = 0; i < MAX_PADS; i++)
    if (g_pads[i]) SDL_GameControllerClose(g_pads[i]);
  if (audio) SDL_CloseAudioDevice(audio);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
  //GUI subsystem: no console of its own. Started from a console, output
  //that is not redirected goes to that console.
  if ((_fileno(stdout) < 0 || _fileno(stderr) < 0) && AttachConsole((unsigned long)-1)) {
    if (_fileno(stdout) < 0) freopen("CONOUT$", "w", stdout);
    if (_fileno(stderr) < 0) freopen("CONOUT$", "w", stderr);
  }
#endif
  Options o;
  if (!parse(argc, argv, &o)) return 2;
  //A ROM named on the command line is relative to where the user is.
  if (!o.rom_path) find_home();
  settings_load(&g_cfg, SETTINGS_PATH);
  battle_setup_load(&g_cfg);
  if (o.scale > 0) g_cfg.scale = o.scale;
  if (o.fullscreen) g_cfg.fullscreen = true;
  if (o.widescreen >= 0) g_cfg.widescreen = o.widescreen % WIDE_COUNT;
  //Every game in rom/; a ROM named on the command line is started directly
  //(and headless runs default to G3), otherwise the game select screen opens.
  int found = scan_roms("rom");
  int start = 0;
  if (o.rom_path) {
    if (!(start = load_rom(o.rom_path, false))) return 1;
  } else if (o.headless) {
    start = g_roms[SNES_GAME_G3] ? SNES_GAME_G3 : found;
  }
  if (!start && !found) {
    fprintf(stderr, "no ROM: put a supported ROM in rom/ (any file name, .sfc or .smc; rom/README.md)\n");
    if (!o.headless)
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "trio",
        "No supported ROM found.\n\n"
        "Copy your ROM images into the rom folder (any file name, .sfc or .smc) "
        "and start trio again. rom/README.md lists the supported versions.", NULL);
    return 1;
  }
  g_recomp = !o.interp && !o.coverage_path && rt_enabled_by_env();
  activate_game(start ? start : found, NULL);
  uint8_t *cov = NULL;
  if (o.coverage_path) {
    cov = calloc(1 << 24, 1);
    g_snes.coverage = cov;
  }
  enh_init(&g_enh, &g_snes, g_rom, g_rom_size);
  enh_apply(&g_enh, &g_snes, &g_cfg);
  rw_init();
  rw_set_capacity(g_cfg.rewind_seconds);
  if (o.state_path && !load_state_file(o.state_path)) return 1;
  int rc;
  if (o.headless) rc = run_headless(&o);
  else {
    ss_init(save_dir(g_game), g_rom, g_rom_size);
    if (!start) open_launcher();
    rc = run_window(&o);
  }
  if (cov) coverage_merge(o.coverage_path, cov);
  return rc;
}
