#include "sound.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_MASTER 357366ull     //master cycles per frame (NTSC)

void snd_init(ArenaSound *a) {
  memset(a, 0, sizeof(*a));
  for (int g = 0; g < 4; g++) a->volume[g] = 1.0f;
}

static void take(SndChip *c, const Snes *src) {
  c->on = true;
  c->apu = src->apu;
  //The chip runs on its own clock from here.
  c->apu.cycles = 0;
  c->apu.ring_r = c->apu.ring_w;
  c->master = 0;
  c->ack = src->wram[0xFFFE];
  c->nq = 0;
  c->quiet = 0;
}

static void pump(SndChip *c);
static void enqueue(SndChip *c, uint8_t id, uint8_t param);

static void run_frame(SndChip *c) {
  pump(c);
  c->master += FRAME_MASTER;
  apu_run(&c->apu, c->master * 1024000ull / SNES_MASTER_HZ);
}

//Frames for the stop command to go through and the last notes to end.
#define SETTLE_FRAMES 30

void snd_add(ArenaSound *a, const Snes *src, bool music) {
  int g = src->game & 3;
  if (music) {
    take(&a->chip[g], src);
    a->music_game = g;
    return;
  }
  SndChip *t = &a->silent[g];
  take(t, src);
  enqueue(t, trio_games[g].snd_stop, 0);
  for (int i = 0; i < SETTLE_FRAMES; i++) run_frame(t);
  t->apu.ring_r = t->apu.ring_w;
  for (int w = 0; w < 2; w++)
    for (int o = 0; o < SND_OWNERS; o++) a->own[w][o][g].on = false;
  //The game's shared chip too, unless it already plays the music.
  if (!(a->chip[g].on && a->music_game == g)) a->chip[g] = *t;
}

bool snd_add_song(ArenaSound *a, const Snes *src, int track) {
  const TrioGame *gm = trio_game(src);
  Snes *t = malloc(sizeof(Snes));
  snes_clone(t, src);
  t->no_apu = false;
  bool ok = trio_call(t, gm->music, 0, (uint16_t)track, 0, 0, 4000000);
  //The driver starts the song a little after the upload.
  if (ok) {
    for (int i = 0; i < 2; i++) {
      t->pad[0] = t->pad[1] = 0;
      snes_run_frame(t);
    }
    int g = src->game & 3;
    take(&a->chip[g], t);
    a->music_game = g;
  }
  free(t);
  return ok;
}

static void enqueue(SndChip *c, uint8_t id, uint8_t param) {
  if (!c->on) return;
  if (c->nq == SND_QUEUE) {
    memmove(c->q, c->q + 1, sizeof(c->q[0]) * (SND_QUEUE - 1));
    c->nq--;
  }
  c->q[c->nq++] = (TrioSnd){id, param};
  c->quiet = 0;
}

void snd_play(ArenaSound *a, int game, uint8_t id, uint8_t param) { enqueue(&a->chip[game & 3], id, param); }

void snd_play_from(ArenaSound *a, int owner, int which, int game, uint8_t id, uint8_t param) {
  int g = game & 3;
  if (owner < 0 || owner >= SND_OWNERS || !a->silent[g].on) { snd_play(a, g, id, param); return; }
  SndChip *c = &a->own[which == SND_BODY][owner][g];
  if (!c->on) *c = a->silent[g];
  enqueue(c, id, param);
}

//The games' NMI pump: when the driver has acknowledged the last command
//(port 2 = the counter) the next one goes out: parameter on port 1, id on
//port 0; commands ($F0 and up) also clear port 2 and put their argument
//(or 2 for $FE / $FF) on port 3.
static void pump(SndChip *c) {
  if (!c->nq || c->apu.port_out[2] != c->ack) return;
  TrioSnd s = c->q[0];
  memmove(c->q, c->q + 1, sizeof(c->q[0]) * (size_t)(--c->nq));
  c->ack++;
  c->apu.port_in[1] = s.param;
  if (s.id >= 0xF0) {
    c->apu.port_in[2] = 0;
    c->apu.port_in[3] = s.id >= 0xFE ? 2 : s.param;
  }
  c->apu.port_in[0] = s.id;
}

//A fighter's chip that has been silent this long with nothing queued
//stops running until his next sound (each chip is on its own clock).
#define PARK_FRAMES 90
#define PARK_LEVEL 24

void snd_frame(ArenaSound *a) {
  for (int g = 0; g < 4; g++)
    if (a->chip[g].on) run_frame(&a->chip[g]);
  for (int o = 0; o < 2 * SND_OWNERS; o++)
    for (int g = 0; g < 4; g++) {
      SndChip *c = &a->own[o / SND_OWNERS][o % SND_OWNERS][g];
      if (!c->on || (c->quiet >= PARK_FRAMES && !c->nq)) continue;
      uint32_t w0 = c->apu.ring_w;
      run_frame(c);
      bool loud = false;
      for (uint32_t k = w0; k != c->apu.ring_w && !loud; k++) {
        const int16_t *fr = c->apu.ring[k & (APU_RING - 1)];
        loud = fr[0] > PARK_LEVEL || fr[0] < -PARK_LEVEL || fr[1] > PARK_LEVEL || fr[1] < -PARK_LEVEL;
      }
      c->quiet = loud || c->nq ? 0 : c->quiet + 1;
    }
}

int snd_mix(ArenaSound *a, int16_t *out, int max_frames) {
  int n = max_frames;
  for (int g = 0; g < 4; g++)
    if (a->chip[g].on) {
      int av = (int)(a->chip[g].apu.ring_w - a->chip[g].apu.ring_r);
      if (av < n) n = av;
    }
  if (n <= 0) return 0;
  if (n > 4096) n = 4096;
  static int32_t acc[8192];
  static int16_t buf[8192];
  memset(acc, 0, (size_t)n * 2 * sizeof(acc[0]));
  for (int g = 0; g < 4; g++) {
    SndChip *c = &a->chip[g];
    if (!c->on) continue;
    int m = apu_pop_samples(&c->apu, buf, n);
    float v = a->volume[g];
    for (int k = 0; k < m * 2; k++) acc[k] += (int32_t)(buf[k] * v);
  }
  //The fighters' chips follow the shared ones' pace: what they have ready
  //is mixed in (a parked chip has nothing).
  for (int o = 0; o < 2 * SND_OWNERS; o++)
    for (int g = 0; g < 4; g++) {
      SndChip *c = &a->own[o / SND_OWNERS][o % SND_OWNERS][g];
      if (!c->on) continue;
      int m = apu_pop_samples(&c->apu, buf, n);
      float v = a->volume[g];
      for (int k = 0; k < m * 2; k++) acc[k] += (int32_t)(buf[k] * v);
    }
  for (int k = 0; k < n * 2; k++) out[k] = (int16_t)(acc[k] > 32767 ? 32767 : acc[k] < -32768 ? -32768 : acc[k]);
  return n;
}
