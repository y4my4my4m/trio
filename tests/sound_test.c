#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/sound.h"
#include "game/recomp_rt.h"

//Battle mode's sound chips (src/arena/sound.c): a chip taken from a game in
//a stage keeps its music; the stop command silences it; a sound effect sent
//through the handshake is heard; a song started with the game's music
//routine plays. Needs the ROMs in ROMDIR (skipped, exit 77, without any).
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *dir, int g, uint32_t *size) { return snes_rom_find(dir, g, size); }

static double rms(ArenaSound *a, int frames) {
  double sum = 0;
  long cnt = 0;
  int16_t buf[8192];
  for (int f = 0; f < frames; f++) {
    snd_frame(a);
    int n;
    while ((n = snd_mix(a, buf, 2048)) > 0)
      for (int k = 0; k < n * 2; k++) { sum += (double)buf[k] * buf[k]; cnt++; }
  }
  return cnt ? sqrt(sum / cnt) : 0;
}

static Snes s;
static ArenaSound a;

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "rom";
  int found = 0;
  snes_skip_render = true;
  for (int g = 1; g <= 3; g++) {
    uint32_t size;
    uint8_t *rom = load(dir, g, &size);
    if (!rom) continue;
    found++;
    const TrioGame *gm = &trio_games[g];
    CHECK(trio_boot_stage(&s, rom, size, true, gm->home, NULL), "%s boots", gm->name);
    snd_init(&a);
    snd_add(&a, &s, true);
    double music = rms(&a, 60);
    snd_play(&a, g, gm->snd_stop, 0);
    rms(&a, 30);
    double quiet = rms(&a, 60);
    snd_play(&a, g, gm->sfx_death, 0);
    double sfx = rms(&a, 40);
    printf("%s: music %.0f, after stop %.0f, death sound %.0f\n", gm->name, music, quiet, sfx);
    CHECK(music > 500, "%s: the stage's music plays", gm->name);
    CHECK(quiet < music / 4, "%s: command %02X stops it", gm->name, gm->snd_stop);
    CHECK(sfx > quiet * 3 + 100, "%s: sound %02X is heard", gm->name, gm->sfx_death);
    //Another song through the game's routine.
    snd_init(&a);
    bool ok = snd_add_song(&a, &s, g == 1 ? 0x15 : 0x16);
    double song = rms(&a, 90);
    printf("%s: song routine %d, %.0f\n", gm->name, ok, song);
    CHECK(ok && song > 500, "%s: a song plays", gm->name);
    free(rom);
  }
  if (!found) { printf("no ROMs, skipped\n"); return 77; }
  if (!fails) printf("sound: all tests passed\n");
  return fails ? 1 : 0;
}
