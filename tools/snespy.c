#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/snes.h"
#include "game/recomp_rt.h"
#include "game/widescreen.h"

//Research library for tools/sp.py (ctypes): runs machines of any of the
//three games, with breakpoints and WRAM write watches. Not part of the port.

typedef struct Hit { uint32_t pc; uint16_t a, x, y, s, d; uint8_t db, p; uint32_t addr; uint8_t val; uint64_t frame; } Hit;

#define MAXBRK 32
#define MAXHITS 4096

typedef struct Sp {
  Snes s;
  uint8_t *rom;
  uint32_t rom_size;
  bool recomp;
  uint32_t brk[MAXBRK];
  int nbrk;
  uint32_t watch_lo, watch_hi;     //WRAM [lo, hi) write watch, 17-bit offsets
  uint16_t io_lo, io_hi;           //I/O [lo, hi) write watch ($2100-$5FFF of the system banks), recorded as 0x100000 | addr
  Hit hits[MAXHITS];
  int nhits;
  uint8_t *cov;
} Sp;

static Sp *cur;   //for the write callback

static void rec(Sp *p, uint32_t addr, uint8_t v) {
  if (p->nhits >= MAXHITS) return;
  const Cpu *c = &p->s.cpu;
  Hit *h = &p->hits[p->nhits++];
  h->pc = (uint32_t)c->pb << 16 | c->pc;
  h->a = c->a; h->x = c->x; h->y = c->y; h->s = c->s; h->d = c->d; h->db = c->db; h->p = c->p;
  h->addr = addr; h->val = v; h->frame = p->s.frame_count;
}

static uint32_t last_pc;
static bool hook(Snes *s) {
  Sp *p = (Sp *)s;
  const Cpu *c = &s->cpu;
  uint32_t pc = (uint32_t)c->pb << 16 | c->pc;
  last_pc = pc;
  for (int i = 0; i < p->nbrk; i++)
    if (p->brk[i] == pc) rec(p, 0, 0);
  if (p->cov && !c->waiting) p->cov[pc] |= (uint8_t)(1 << ((c->e ? 4 : 0) | ((c->p & CPU_M) ? 2 : 0) | ((c->p & CPU_X) ? 1 : 0)));
  return false;
}

static void wr(void *ctx, uint32_t addr, uint8_t v) {
  Sp *p = (Sp *)ctx;
  uint8_t bank = (uint8_t)(addr >> 16);
  uint32_t w = 0xFFFFFFFF;
  if (!(bank & 0x40) && (addr & 0xFFFF) < 0x2000) w = addr & 0x1FFF;
  else if ((bank & 0xFE) == 0x7E) w = addr & 0x1FFFF;
  uint16_t a16 = (uint16_t)addr;
  if (!(bank & 0x40) && a16 >= p->io_lo && a16 < p->io_hi) {
    rec(p, 0x100000u | a16, v);
    p->hits[p->nhits - 1].pc = last_pc;
  }
  if (w >= p->watch_lo && w < p->watch_hi) {
    uint32_t keep = (uint32_t)p->s.cpu.pb << 16 | p->s.cpu.pc;
    (void)keep;
    rec(p, w, v);
    //PC of the instruction (cpu.pc already advanced past operands): the hook's last PC.
    p->hits[p->nhits - 1].pc = last_pc;
  }
  snes_write(&p->s, addr, v);
}

static void setup(Sp *p) {
  bool w = p->watch_hi > p->watch_lo || p->io_hi > p->io_lo;
  bool dbg = p->nbrk || w || p->cov;
  if (dbg) {
    p->s.exec_hook = hook;
    p->s.cpu.ctx = p;   //Snes is the first member
    p->s.cpu.write = w ? wr : p->s.cpu.write;
  } else {
    rt_install(&p->s, p->recomp);
  }
}

Sp *sp_new(const char *path, int recomp) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc((size_t)n);
  if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
  fclose(f);
  Sp *p = calloc(1, sizeof(Sp));
  p->rom = snes_rom_image(d, (size_t)n, &p->rom_size);
  free(d);
  snes_init(&p->s, p->rom, p->rom_size);
  snes_reset(&p->s);
  p->recomp = recomp != 0;
  setup(p);
  return p;
}

Sp *sp_clone(Sp *src) {
  Sp *p = malloc(sizeof(Sp));
  memcpy(p, src, sizeof(Sp));
  snes_clone(&p->s, &src->s);
  p->nhits = 0;
  setup(p);
  return p;
}

void sp_copy(Sp *dst, Sp *src) {
  snes_clone(&dst->s, &src->s);
  setup(dst);
}

void sp_free(Sp *p) { free(p); }

void sp_run(Sp *p, int pad, int frames) {
  cur = p;
  for (int i = 0; i < frames; i++) {
    p->s.pad[0] = (uint16_t)pad;
    snes_run_frame(&p->s);
  }
}

void sp_run_pads(Sp *p, const uint16_t *pads, int frames) {
  cur = p;
  for (int i = 0; i < frames; i++) {
    p->s.pad[0] = pads[i];
    snes_run_frame(&p->s);
  }
}

void sp_set_pad2(Sp *p, int pad) { p->s.pad[1] = (uint16_t)pad; }
uint8_t *sp_wram(Sp *p) { return p->s.wram; }
uint8_t *sp_vram(Sp *p) { return p->s.ppu.vram; }
uint8_t *sp_oam(Sp *p) { return p->s.ppu.oam; }
uint16_t *sp_cgram(Sp *p) { return p->s.ppu.cgram; }
uint8_t *sp_rom(Sp *p) { return p->rom; }
uint32_t sp_rom_size(Sp *p) { return p->rom_size; }
uint8_t *sp_cx4ram(Sp *p) { return p->s.cx4.ram; }
uint8_t *sp_aram(Sp *p) { return p->s.apu.ram; }
uint8_t *sp_apu_in(Sp *p) { return p->s.apu.port_in; }
uint8_t *sp_apu_out(Sp *p) { return p->s.apu.port_out; }
Snes *sp_snes(Sp *p) { return &p->s; }
uint64_t sp_frame_count(Sp *p) { return p->s.frame_count; }

int sp_cpu(Sp *p, uint32_t *out) {
  const Cpu *c = &p->s.cpu;
  out[0] = (uint32_t)c->pb << 16 | c->pc; out[1] = c->a; out[2] = c->x; out[3] = c->y;
  out[4] = c->s; out[5] = c->d; out[6] = c->db; out[7] = c->p;
  return 8;
}

void sp_break(Sp *p, uint32_t pc) {
  if (p->nbrk < MAXBRK) p->brk[p->nbrk++] = pc;
  setup(p);
}
void sp_watch_io(Sp *p, uint32_t lo, uint32_t hi) { p->io_lo = (uint16_t)lo; p->io_hi = (uint16_t)hi; setup(p); }
void sp_clear_breaks(Sp *p) { p->nbrk = 0; p->watch_lo = p->watch_hi = 0; p->io_lo = p->io_hi = 0; p->s.cpu.write = NULL; snes_clone(&p->s, &p->s); setup(p); }
void sp_watch(Sp *p, uint32_t lo, uint32_t hi) { p->watch_lo = lo; p->watch_hi = hi; setup(p); }
int sp_hits(Sp *p, Hit *out, int max) {
  int n = p->nhits < max ? p->nhits : max;
  memcpy(out, p->hits, (size_t)n * sizeof(Hit));
  p->nhits = 0;
  return n;
}
int sp_hit_size(void) { return (int)sizeof(Hit); }

void sp_coverage(Sp *p, uint8_t *buf) { p->cov = buf; setup(p); }

//Widescreen game logic for w extra columns per side (docs/WIDESCREEN.md):
//the ROM constants, the stage overrides and the Cx4 sprite bounds. The
//picture stays 256 wide.
void sp_wide(Sp *p, int w) {
  ws_active_wide = w;
  ws_apply_rom(p->rom, p->rom_size, w);
  cx4_wide = w;
}

//Picture: PPM of the last frame.
void sp_shot(Sp *p, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return;
  int w = p->s.ppu.out_w ? p->s.ppu.out_w : PPU_W;
  fprintf(f, "P6\n%d %d\n255\n", w, 224);
  for (int y = 0; y < 224; y++)
    for (int x = 0; x < w; x++) {
      uint32_t c = p->s.ppu.frame[y][x];
      fputc((int)(c >> 16 & 255), f); fputc((int)(c >> 8 & 255), f); fputc((int)(c & 255), f);
    }
  fclose(f);
}

int sp_save(Sp *p, const char *path) {
  size_t n = snes_state_size();
  uint8_t *b = malloc(n);
  snes_state_save(&p->s, b);
  FILE *f = fopen(path, "wb");
  if (!f) { free(b); return 0; }
  fwrite(b, 1, n, f);
  fclose(f);
  free(b);
  return 1;
}

int sp_load(Sp *p, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  size_t n = snes_state_size();
  uint8_t *b = malloc(n);
  bool ok = fread(b, 1, n, f) == n && snes_state_load(&p->s, b);
  fclose(f);
  free(b);
  setup(p);
  return ok;
}

//Audio: pops samples (stereo int16) into out; returns frames.
int sp_audio(Sp *p, int16_t *out, int max) { return apu_pop_samples(&p->s.apu, out, max); }

//Runs pb:pc as a JSL on this machine (8-bit A/X/Y, given D and DB) to its RTL.
int sp_call(Sp *p, uint32_t pc24, uint16_t a, uint16_t x, uint16_t y, uint16_t d, uint8_t db, int m16, int x16) {
  Cpu saved = p->s.cpu;
  Cpu *c = &p->s.cpu;
  uint16_t ret = 0xFFF0 - 1;
  p->s.wram[c->s & 0x1FFF] = 0; c->s--;
  p->s.wram[c->s & 0x1FFF] = (uint8_t)(ret >> 8); c->s--;
  p->s.wram[c->s & 0x1FFF] = (uint8_t)ret; c->s--;
  c->e = false; c->pb = (uint8_t)(pc24 >> 16); c->pc = (uint16_t)pc24; c->d = d; c->db = db;
  c->p = (uint8_t)(0x04 | (m16 ? 0 : 0x20) | (x16 ? 0 : 0x10));
  c->a = a; c->x = x; c->y = y;
  c->nmi_pending = false; c->irq_line = false; c->waiting = false;
  int i;
  for (i = 0; i < 200000 && !(c->pb == 0 && c->pc == 0xFFF0); i++) cpu_step(c);
  bool nmi = p->s.cpu.nmi_pending;
  p->s.cpu = saved;
  p->s.cpu.nmi_pending |= nmi;
  return i;
}
