#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/snes.h"
#include "script.h"
#include "game/recomp_rt.h"

//Research tool: run from a state (or reset) with an input script and dump
//WRAM, VRAM and OAM at chosen frames. Script `save` commands write states.
//POKE=aaaa:vv[,...] writes WRAM after the state loads; with
//POKEFRAMES=a-b the writes are repeated every frame in [a, b).
//usage: trio-probe ROM STATE|- SCRIPT|- OUTPREFIX FRAME [FRAME...]

static Snes s;

//BREAK=bbaaaa: when the interpreter reaches that address, print the CPU
//registers and the first 64 bytes of the direct page (recompiled code is
//switched off so every instruction passes through here).
static uint32_t brk_pc;
static int brk_hits;
static bool brk_hook(Snes *m) {
  const Cpu *c = &m->cpu;
  if (((uint32_t)c->pb << 16 | c->pc) == brk_pc && brk_hits < 4) {
    brk_hits++;
    fprintf(stderr, "BREAK %06X a=%04X x=%04X y=%04X d=%04X db=%02X p=%02X s=%04X\n  dp:", brk_pc, c->a, c->x, c->y, c->d, c->db, c->p, c->s);
    for (int i = 0; i < 64; i++) fprintf(stderr, " %02X", m->wram[(c->d + i) & 0x1FFF]);
    fprintf(stderr, "\n");
  }
  return false;
}

static uint8_t *read_all(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long l = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc((size_t)l);
  if (fread(b, 1, (size_t)l, f) != (size_t)l) { free(b); b = NULL; }
  fclose(f);
  *n = (size_t)l;
  return b;
}

static void dump(const char *prefix, int fr) {
  char p[512];
  snprintf(p, sizeof p, "%s_%d.wram", prefix, fr);
  FILE *f = fopen(p, "wb");
  if (f) { fwrite(s.wram, 1, sizeof s.wram, f); fclose(f); }
  snprintf(p, sizeof p, "%s_%d.vram", prefix, fr);
  f = fopen(p, "wb");
  if (f) { fwrite(s.ppu.vram, 1, sizeof s.ppu.vram, f); fclose(f); }
  snprintf(p, sizeof p, "%s_%d.oam", prefix, fr);
  f = fopen(p, "wb");
  if (f) { fwrite(s.ppu.oam, 1, sizeof s.ppu.oam, f); fclose(f); }
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: trio-probe ROM STATE|- SCRIPT|- OUTPREFIX FRAME [FRAME...]\n");
    return 2;
  }
  size_t n;
  uint8_t *raw = read_all(argv[1], &n);
  if (!raw) return 1;
  uint32_t rom_size;
  uint8_t *rom = snes_rom_image(raw, n, &rom_size);
  snes_init(&s, rom, rom_size);
  snes_reset(&s);
  rt_install(&s, true);
  if (getenv("BREAK")) { brk_pc = (uint32_t)strtoul(getenv("BREAK"), NULL, 16); s.exec_hook = brk_hook; }
  if (strcmp(argv[2], "-")) {
    size_t m;
    uint8_t *st = read_all(argv[2], &m);
    if (!st || m != snes_state_size() || !snes_state_load(&s, st)) { fprintf(stderr, "bad state\n"); return 1; }
    if (brk_pc) s.exec_hook = brk_hook;
  }
  //POKE=aaaa:vv[,aaaa:vv...] writes WRAM bytes (17-bit offset) after the state loads.
  if (getenv("POKE")) {
    const char *q = getenv("POKE");
    while (*q) {
      unsigned a, v;
      if (sscanf(q, "%x:%x", &a, &v) != 2) break;
      s.wram[a & 0x1FFFF] = (uint8_t)v;
      q = strchr(q, ',');
      if (!q) break;
      q++;
    }
  }
  int nc = 0;
  ScriptCmd *c = strcmp(argv[3], "-") ? script_load(argv[3], &nc) : NULL;
  int last = 0;
  for (int a = 5; a < argc; a++)
    if (atoi(argv[a]) > last) last = atoi(argv[a]);
  int ci = 0, held_until = 0;
  uint16_t held = 0;
  for (int fr = 0; fr <= last; fr++) {
    for (; ci < nc && c[ci].frame <= fr; ci++) {
      if (c[ci].kind == CMD_PAD) { held = c[ci].buttons; held_until = fr + c[ci].hold; }
      else if (c[ci].kind == CMD_SAVE) {
        size_t z = snes_state_size();
        uint8_t *b = malloc(z);
        snes_state_save(&s, b);
        FILE *f = fopen(c[ci].arg, "wb");
        if (f) { fwrite(b, 1, z, f); fclose(f); }
        free(b);
      }
    }
    for (int a = 5; a < argc; a++)
      if (atoi(argv[a]) == fr) dump(argv[4], fr);
    //POKEFRAMES=a-b re-applies POKE every frame in [a, b).
    if (getenv("POKE") && getenv("POKEFRAMES")) {
      int pa = 0, pb = 0;
      sscanf(getenv("POKEFRAMES"), "%d-%d", &pa, &pb);
      if (fr >= pa && fr < pb) {
        const char *q = getenv("POKE");
        while (*q) {
          unsigned ad, v;
          if (sscanf(q, "%x:%x", &ad, &v) != 2) break;
          s.wram[ad & 0x1FFFF] = (uint8_t)v;
          q = strchr(q, ',');
          if (!q) break;
          q++;
        }
      }
    }
    s.pad[0] = fr < held_until ? held : 0;
    snes_run_frame(&s);
  }
  return 0;
}
