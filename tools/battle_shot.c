#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/battle.h"
#include "arena/launcher.h"
#include "game/recomp_rt.h"

//Headless check of the game select screen and battle mode: opens the game
//select screen over the ROMs in ROMDIR, goes to BATTLE MODE with its own
//menu, starts a match of CPU players from the setup page and saves frames.
//usage: trio-battle-shot ROMDIR PREFIX [FIGHTERS [FRAMES [EVERY [ROOM]]]]
//Writes PREFIX-select.ppm, PREFIX-page.ppm and PREFIX-NNNNN.ppm.
//BATTLE_WEAPONS=0: blaster only. BATTLE_CHARS: one digit per slot (character
//index), or comma-separated indexes. BATTLE_WIDE: columns per side of the picture (default 96).
//BATTLE_VIEW: picture size (BV_*). BATTLE_GAMES=1: also saves the select screen of every game (PREFIX-gameN.ppm).

static BattleSetup setup = {{BS_CPU3, BS_CPU2, BS_OFF, BS_OFF}, 2, 0, true, {0, 0, 0, 0}, 0, false, BV_LARGE, BT_OFF, 0};

static void ppm(const char *path, const uint32_t *px, int w, int h, int stride) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint32_t c = px[y * stride + x];
      fputc((int)(c >> 16 & 255), f); fputc((int)(c >> 8 & 255), f); fputc((int)(c & 255), f);
    }
  fclose(f);
}

static uint8_t *load(const char *dir, int g, uint32_t *size) { return snes_rom_find(dir, g, size); }

static void shot_launcher(const char *path) {
  int w, h;
  const uint32_t *px = launcher_picture(&w, &h);
  ppm(path, px, w, h, PPU_MAXW);
}

//Runs the select screen with a press of `pad` (2 frames) then `after` idle frames.
static int press(uint16_t pad, int after) {
  int act = LA_NONE;
  for (int i = 0; i < 2 + after && act == LA_NONE; i++) act = launcher_frame(i < 2 ? pad : 0);
  int16_t audio[4096];
  while (launcher_audio(audio, 2048) > 0) {}
  return act;
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: trio-battle-shot ROMDIR PREFIX [FIGHTERS [FRAMES [EVERY [ROOM]]]]\n"); return 2; }
  const char *prefix = argv[2];
  int fighters = argc > 3 ? atoi(argv[3]) : 2, frames = argc > 4 ? atoi(argv[4]) : 600, every = argc > 5 ? atoi(argv[5]) : 150;
  setup.room = argc > 6 ? atoi(argv[6]) : 0;
  for (int i = 0; i < BATTLE_SLOTS; i++) setup.slot[i] = i < fighters ? (i % 2 ? BS_CPU2 : BS_CPU3) : BS_OFF;
  if (getenv("BATTLE_WEAPONS")) setup.weapons = atoi(getenv("BATTLE_WEAPONS")) != 0;
  const char *chars = getenv("BATTLE_CHARS");
  //One digit per slot, or comma-separated indexes ("10,3").
  if (chars && strchr(chars, ',')) {
    char *e = (char *)chars;
    for (int i = 0; i < BATTLE_SLOTS && *e; i++) { setup.fighter[i] = (int)strtol(e, &e, 10) % battle_characters(); if (*e == ',') e++; }
  } else {
    for (int i = 0; chars && i < BATTLE_SLOTS && chars[i]; i++) setup.fighter[i] = (chars[i] - '0') % battle_characters();
  }
  g_port.battle = &setup;

  bool recomp = rt_enabled_by_env();
  LauncherGame games[4] = {{0}};
  for (int g = 1; g <= 3; g++) {
    games[g].rom = load(argv[1], g, &games[g].size);
    games[g].recomp = recomp;
  }
  launcher_open(games, games[3].rom ? 3 : 0);
  char path[512];
  press(0, 10);
  snprintf(path, sizeof path, "%s-select.ppm", prefix);
  shot_launcher(path);
  if (getenv("BATTLE_GAMES")) {
    //Every game, booted in the background as the screen runs.
    press(0, 120);
    for (int k = 0; k < 3; k++) {
      press(PAD_RIGHT, 20);
      snprintf(path, sizeof path, "%s-game%d.ppm", prefix, launcher_game());
      shot_launcher(path);
    }
  }
  //Down onto BATTLE MODE, open the page; START (B on START) starts.
  press(PAD_DOWN, 4);
  press(PAD_B, 4);
  snprintf(path, sizeof path, "%s-page.ppm", prefix);
  shot_launcher(path);
  int act = press(PAD_B, 10);
  if (act != LA_BATTLE) { fprintf(stderr, "the setup page did not start a match (%d)\n", act); return 1; }
  int wide = getenv("BATTLE_WIDE") ? atoi(getenv("BATTLE_WIDE")) : 96;
  const uint8_t *roms[4] = {0};
  uint32_t sizes[4] = {0};
  for (int g = 1; g <= 3; g++) { roms[g] = games[g].rom; sizes[g] = games[g].size; }
  //BATTLE_MUSIC: song index.
  if (getenv("BATTLE_MUSIC")) setup.music = atoi(getenv("BATTLE_MUSIC"));
  if (getenv("BATTLE_ITEMS")) setup.items = atoi(getenv("BATTLE_ITEMS")) != 0;
  //BATTLE_TIME: time limit (BT_*).
  if (getenv("BATTLE_TIME")) setup.time = atoi(getenv("BATTLE_TIME")) % BT_COUNT;
  //BATTLE_VIEW: 0 NORMAL, 1 LARGE, 2 HUGE.
  if (getenv("BATTLE_VIEW")) setup.view = atoi(getenv("BATTLE_VIEW")) % BV_COUNT;
  if (!battle_begin(&setup, roms, sizes, recomp, wide)) { fprintf(stderr, "battle did not start\n"); return 1; }
  uint16_t none[BATTLE_SLOTS] = {0};
  int16_t audio[4096];
  //BATTLE_WAV: the match's sound.
  FILE *wav = getenv("BATTLE_WAV") ? fopen(getenv("BATTLE_WAV"), "wb") : NULL;
  for (int fr = 1; fr <= frames; fr++) {
    if (!battle_frame(none, 0)) break;
    int n;
    while ((n = battle_audio(audio, 2048)) > 0)
      if (wav) fwrite(audio, 4, (size_t)n, wav);
    if (fr % every == 0) {
      int w, h;
      const uint32_t *px = battle_picture(&w, &h);
      snprintf(path, sizeof path, "%s-%05d.ppm", prefix, fr);
      ppm(path, px, w, h, BATTLE_MAXW);
    }
  }
  if (wav) fclose(wav);
  battle_end();
  return 0;
}
