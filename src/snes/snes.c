#include "snes.h"
#include <stdlib.h>
#include <string.h>

//Bus, memory map (LoROM, + Cx4 at $6000-$7FFF when the cartridge has one), CPU-side I/O ($21xx, $40xx-$43xx), DMA/HDMA,
//line scheduler, NMI/IRQ timers, joypad auto-read.

bool snes_skip_render;

static void dma_run(Snes *s, uint8_t mask);
static void hdma_init(Snes *s);
static void hdma_run(Snes *s);

static inline void add_cycles(Snes *s, int n) { snes_add_cycles(s, n); }

static inline int vis_lines(Snes *s) {
  return s->ppu.overscan ? 239 : 224;
}

static void apu_sync(Snes *s) {
  if (s->no_apu) return;
  //SPC700 at 1.024 MHz against 21.477272 MHz master.
  apu_run(&s->apu, s->cycles * 1024000ull / SNES_MASTER_HZ);
}

static inline uint32_t rom_offset(Snes *s, uint8_t bank, uint16_t a) {
  return ((((uint32_t)bank & 0x7F) << 15) | (a & 0x7FFF)) & (s->rom_size - 1);
}

static int access_time(Snes *s, uint32_t addr) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (!(bank & 0x40)) {
    if (a < 0x2000) return 8;
    if (a < 0x4000) return 6;
    if (a < 0x4200) return 12;
    if (a < 0x6000) return 6;
    if (a < 0x8000) return 8;
    return ((bank & 0x80) && (s->memsel & 1)) ? 6 : 8;
  }
  return (bank >= 0xC0 && (s->memsel & 1)) ? 6 : 8;
}

//---------------------------------------------------------------- B bus

static uint8_t bbus_read(Snes *s, uint8_t reg) {
  if (reg >= 0x34 && reg < 0x40) {
    if (reg == 0x37) {
      s->ppu.hcount_latch = (uint16_t)((s->cycles - s->line_start) >> 2);
      s->ppu.vcount_latch = s->vpos;
      s->ppu.counter_latched = true;
      return s->open_bus;
    }
    return ppu_read(&s->ppu, reg, s->open_bus);
  }
  if (reg >= 0x40 && reg < 0x80) {
    apu_sync(s);
    return apu_cpu_read(&s->apu, reg & 3);
  }
  if (reg == 0x80) {
    uint8_t v = s->wram[s->wram_addr];
    s->wram_addr = (s->wram_addr + 1) & 0x1FFFF;
    return v;
  }
  return s->open_bus;
}

static void bbus_write(Snes *s, uint8_t reg, uint8_t v) {
  if (reg < 0x34) { ppu_write(&s->ppu, reg, v); return; }
  if (reg >= 0x40 && reg < 0x80) {
    apu_sync(s);
    apu_cpu_write(&s->apu, reg & 3, v);
    return;
  }
  switch (reg) {
  case 0x80:
    s->wram[s->wram_addr] = v;
    s->wram_addr = (s->wram_addr + 1) & 0x1FFFF;
    break;
  case 0x81: s->wram_addr = (s->wram_addr & 0x1FF00) | v; break;
  case 0x82: s->wram_addr = (s->wram_addr & 0x100FF) | (uint32_t)v << 8; break;
  case 0x83: s->wram_addr = (s->wram_addr & 0x0FFFF) | (uint32_t)(v & 1) << 16; break;
  }
}

//---------------------------------------------------------------- CPU I/O

static uint8_t dma_reg_read(Snes *s, uint16_t a) {
  DmaChannel *c = &s->dma[(a >> 4) & 7];
  switch (a & 0xF) {
  case 0x0: return c->dmap;
  case 0x1: return c->bbad;
  case 0x2: return c->a1t & 0xFF;
  case 0x3: return c->a1t >> 8;
  case 0x4: return c->a1b;
  case 0x5: return c->das & 0xFF;
  case 0x6: return c->das >> 8;
  case 0x7: return c->dasb;
  case 0x8: return c->a2a & 0xFF;
  case 0x9: return c->a2a >> 8;
  case 0xA: return c->ntrl;
  case 0xB: case 0xF: return c->unused;
  }
  return s->open_bus;
}

static void dma_reg_write(Snes *s, uint16_t a, uint8_t v) {
  DmaChannel *c = &s->dma[(a >> 4) & 7];
  switch (a & 0xF) {
  case 0x0: c->dmap = v; break;
  case 0x1: c->bbad = v; break;
  case 0x2: c->a1t = (c->a1t & 0xFF00) | v; break;
  case 0x3: c->a1t = (c->a1t & 0x00FF) | v << 8; break;
  case 0x4: c->a1b = v; break;
  case 0x5: c->das = (c->das & 0xFF00) | v; break;
  case 0x6: c->das = (c->das & 0x00FF) | v << 8; break;
  case 0x7: c->dasb = v; break;
  case 0x8: c->a2a = (c->a2a & 0xFF00) | v; break;
  case 0x9: c->a2a = (c->a2a & 0x00FF) | v << 8; break;
  case 0xA: c->ntrl = v; break;
  case 0xB: case 0xF: c->unused = v; break;
  }
}

static uint8_t io_read(Snes *s, uint16_t a) {
  if (a >= 0x2100 && a < 0x2200) return bbus_read(s, a & 0xFF);
  if (a >= 0x4300 && a < 0x4380) return dma_reg_read(s, a);
  switch (a) {
  case 0x4016: case 0x4017: {
    int i = a & 1;
    uint8_t bit = s->joy_shift[i] >> 15;
    if (!s->joy_strobe) s->joy_shift[i] = (uint16_t)(s->joy_shift[i] << 1 | 1);
    return (s->open_bus & 0xFC) | bit | (i ? 0x1C : 0);
  }
  case 0x4210: {
    uint8_t v = (s->nmi_flag ? 0x80 : 0) | (s->open_bus & 0x70) | 0x02;
    s->nmi_flag = false;
    return v;
  }
  case 0x4211: {
    uint8_t v = (s->irq_flag ? 0x80 : 0) | (s->open_bus & 0x7F);
    s->irq_flag = false;
    s->cpu.irq_line = false;
    return v;
  }
  case 0x4212: {
    uint64_t h = s->cycles - s->line_start;
    return (s->in_vblank ? 0x80 : 0) | (h >= SNES_HBLANK_START || h < 4 ? 0x40 : 0) |
           (s->autojoy_busy_lines > 0 ? 1 : 0) | (s->open_bus & 0x3E);
  }
  case 0x4213: return s->wrio;
  case 0x4214: return s->rddiv & 0xFF;
  case 0x4215: return s->rddiv >> 8;
  case 0x4216: return s->rdmpy & 0xFF;
  case 0x4217: return s->rdmpy >> 8;
  case 0x4218: case 0x4219: case 0x421A: case 0x421B:
  case 0x421C: case 0x421D: case 0x421E: case 0x421F: {
    uint16_t j = s->joy_auto[(a - 0x4218) >> 1];
    return (a & 1) ? j >> 8 : j & 0xFF;
  }
  }
  return s->open_bus;
}

static void io_write(Snes *s, uint16_t a, uint8_t v) {
  if (a >= 0x2100 && a < 0x2200) { bbus_write(s, a & 0xFF, v); return; }
  if (a >= 0x4300 && a < 0x4380) { dma_reg_write(s, a, v); return; }
  switch (a) {
  case 0x4016:
    s->joy_strobe = v & 1;
    if (s->joy_strobe) { s->joy_shift[0] = s->pad[0]; s->joy_shift[1] = s->pad[1]; }
    break;
  case 0x4200: {
    uint8_t old = s->nmitimen;
    s->nmitimen = v;
    if (!(v & 0x30)) { s->irq_flag = false; s->cpu.irq_line = false; }
    if (!(old & 0x80) && (v & 0x80) && s->nmi_flag) s->cpu.nmi_pending = true;
    break;
  }
  case 0x4201:
    if ((s->wrio & 0x80) && !(v & 0x80)) {
      s->ppu.hcount_latch = (uint16_t)((s->cycles - s->line_start) >> 2);
      s->ppu.vcount_latch = s->vpos;
      s->ppu.counter_latched = true;
    }
    s->wrio = v;
    break;
  case 0x4202: s->wrmpya = v; break;
  case 0x4203:
    s->wrmpyb = v;
    s->rdmpy = (uint16_t)(s->wrmpya * v);
    s->rddiv = (uint16_t)(v << 8 | s->wrmpya);
    break;
  case 0x4204: s->wrdiv = (s->wrdiv & 0xFF00) | v; break;
  case 0x4205: s->wrdiv = (s->wrdiv & 0x00FF) | v << 8; break;
  case 0x4206:
    if (v == 0) { s->rddiv = 0xFFFF; s->rdmpy = s->wrdiv; }
    else { s->rddiv = s->wrdiv / v; s->rdmpy = s->wrdiv % v; }
    break;
  case 0x4207: s->htime = (s->htime & 0x100) | v; break;
  case 0x4208: s->htime = (s->htime & 0x0FF) | (v & 1) << 8; break;
  case 0x4209: s->vtime = (s->vtime & 0x100) | v; break;
  case 0x420A: s->vtime = (s->vtime & 0x0FF) | (v & 1) << 8; break;
  case 0x420B: dma_run(s, v); break;
  case 0x420C: s->hdmaen = v; break;
  case 0x420D: s->memsel = v; break;
  }
}

//---------------------------------------------------------------- memory map

static uint8_t bus_read(Snes *s, uint32_t addr) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (!(bank & 0x40)) {
    if (a < 0x2000) return s->wram[a];
    if (a < 0x6000) return io_read(s, a);
    if (a < 0x8000) return s->has_cx4 ? cx4_read(&s->cx4, a) : s->open_bus;
    return s->rom[rom_offset(s, bank, a)];
  }
  if (bank == 0x7E || bank == 0x7F) return s->wram[addr & 0x1FFFF];
  if ((bank & 0x7F) >= 0x70 && a < 0x8000) return s->sram[a & 0x1FFF];
  return s->rom[rom_offset(s, bank, a)];
}

static void bus_write(Snes *s, uint32_t addr, uint8_t v) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (!(bank & 0x40)) {
    if (a < 0x2000) { s->wram[a] = v; return; }
    if (a < 0x6000) { io_write(s, a, v); return; }
    if (a < 0x8000 && s->has_cx4) cx4_write(&s->cx4, a, v, s->rom, s->rom_size, s->wram);
    return;
  }
  if (bank == 0x7E || bank == 0x7F) { s->wram[addr & 0x1FFFF] = v; return; }
  if ((bank & 0x7F) >= 0x70 && a < 0x8000) {
    if (s->sram[a & 0x1FFF] != v) s->sram_dirty = true;
    s->sram[a & 0x1FFF] = v;
  }
}

uint8_t snes_peek(Snes *s, uint32_t addr) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (!(bank & 0x40) && a >= 0x2000 && a < 0x6000) return 0;
  return bus_read(s, addr);
}

uint8_t snes_read(Snes *s, uint32_t addr) {
  add_cycles(s, access_time(s, addr));
  uint8_t v = bus_read(s, addr);
  s->open_bus = v;
  return v;
}

void snes_write(Snes *s, uint32_t addr, uint8_t val) {
  add_cycles(s, access_time(s, addr));
  s->open_bus = val;
  bus_write(s, addr, val);
}

static uint8_t cpu_read_cb(void *ctx, uint32_t addr) { return snes_read(ctx, addr); }
static void cpu_write_cb(void *ctx, uint32_t addr, uint8_t v) { snes_write(ctx, addr, v); }
void snes_idle(Snes *s) { add_cycles(s, 6); }

static void cpu_idle_cb(void *ctx) { add_cycles(ctx, 6); }

//---------------------------------------------------------------- DMA

static const uint8_t dma_offsets[8][4] = {
  {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
  {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
};
static const uint8_t dma_unit_len[8] = {1, 2, 2, 4, 4, 4, 2, 4};

static bool abus_valid(uint32_t addr) {
  uint8_t bank = addr >> 16;
  uint16_t a = addr;
  if (bank & 0x40) return true;
  if (a >= 0x2100 && a < 0x2200) return false;
  if (a >= 0x4000 && a < 0x4380 && !(a >= 0x4220 && a < 0x4300)) return false;
  return true;
}

static void dma_transfer_byte(Snes *s, bool to_a, uint32_t abus, uint8_t breg) {
  if (to_a) {
    uint8_t v = bbus_read(s, breg);
    if (abus_valid(abus)) bus_write(s, abus, v);
  } else {
    uint8_t v = abus_valid(abus) ? bus_read(s, abus) : s->open_bus;
    bbus_write(s, breg, v);
  }
}

static void dma_run(Snes *s, uint8_t mask) {
  if (!mask) return;
  add_cycles(s, 8);
  for (int i = 0; i < 8; i++) {
    if (!(mask & (1 << i))) continue;
    DmaChannel *c = &s->dma[i];
    add_cycles(s, 8);
    int mode = c->dmap & 7;
    bool to_a = c->dmap & 0x80;
    bool fixed = c->dmap & 0x08;
    int step = (c->dmap & 0x10) ? -1 : 1;
    int n = 0;
    do {
      uint8_t breg = (uint8_t)(c->bbad + dma_offsets[mode][n & 3]);
      dma_transfer_byte(s, to_a, (uint32_t)c->a1b << 16 | c->a1t, breg);
      add_cycles(s, 8);
      if (!fixed) c->a1t = (uint16_t)(c->a1t + step);
      c->das--;
      n++;
    } while (c->das != 0);
  }
}

static void hdma_init(Snes *s) {
  for (int i = 0; i < 8; i++) {
    DmaChannel *c = &s->dma[i];
    c->hdma_do_transfer = false;
    c->hdma_terminated = false;
    if (!(s->hdmaen & (1 << i))) continue;
    c->a2a = c->a1t;
    c->ntrl = bus_read(s, (uint32_t)c->a1b << 16 | c->a2a++);
    add_cycles(s, 8);
    if (c->dmap & 0x40) {
      uint8_t lo = bus_read(s, (uint32_t)c->a1b << 16 | c->a2a++);
      uint8_t hi = bus_read(s, (uint32_t)c->a1b << 16 | c->a2a++);
      c->das = (uint16_t)(hi << 8 | lo);
      add_cycles(s, 16);
    }
    if (c->ntrl == 0) c->hdma_terminated = true;
    else c->hdma_do_transfer = true;
  }
}

static void hdma_run(Snes *s) {
  bool any = false;
  for (int i = 0; i < 8; i++) {
    DmaChannel *c = &s->dma[i];
    if (!(s->hdmaen & (1 << i)) || c->hdma_terminated) continue;
    any = true;
    add_cycles(s, 8);
    int mode = c->dmap & 7;
    bool to_a = c->dmap & 0x80;
    bool indirect = c->dmap & 0x40;
    if (c->hdma_do_transfer) {
      for (int n = 0; n < dma_unit_len[mode]; n++) {
        uint8_t breg = (uint8_t)(c->bbad + dma_offsets[mode][n]);
        uint32_t abus = indirect ? ((uint32_t)c->dasb << 16 | c->das++)
                                 : ((uint32_t)c->a1b << 16 | c->a2a++);
        dma_transfer_byte(s, to_a, abus, breg);
        add_cycles(s, 8);
      }
    }
    c->ntrl--;
    c->hdma_do_transfer = c->ntrl & 0x80;
    if ((c->ntrl & 0x7F) == 0) {
      c->ntrl = bus_read(s, (uint32_t)c->a1b << 16 | c->a2a++);
      add_cycles(s, 8);
      if (indirect) {
        uint8_t lo = bus_read(s, (uint32_t)c->a1b << 16 | c->a2a++);
        uint8_t hi = bus_read(s, (uint32_t)c->a1b << 16 | c->a2a++);
        c->das = (uint16_t)(hi << 8 | lo);
        add_cycles(s, 16);
      }
      if (c->ntrl == 0) c->hdma_terminated = true;
      c->hdma_do_transfer = true;
    }
  }
  if (any) add_cycles(s, 18);
}

//---------------------------------------------------------------- scheduler

static void raise_irq(Snes *s) {
  s->irq_flag = true;
  s->cpu.irq_line = true;
}

static void new_line(Snes *s) {
  s->line_start += SNES_LINE_CYCLES;
  s->vpos++;
  s->hdma_done_line = s->refresh_done_line = s->hirq_done_line = false;
  if (s->autojoy_busy_lines > 0) s->autojoy_busy_lines--;
  if (s->vpos == SNES_LINES) {
    s->vpos = 0;
    s->frame_count++;
    s->in_vblank = false;
    s->nmi_flag = false;
    ppu_frame_start(&s->ppu);
    hdma_init(s);
  }
  int vis = vis_lines(s);
  if (s->vpos >= 1 && s->vpos <= vis && !snes_skip_render) ppu_render_line(&s->ppu, s->vpos);
  if (s->vpos == vis + 1) {
    s->in_vblank = true;
    ppu_vblank_start(&s->ppu);
    s->nmi_flag = true;
    if (s->nmitimen & 0x80) s->cpu.nmi_pending = true;
    if (s->nmitimen & 1) {
      s->joy_auto[0] = s->pad[0];
      s->joy_auto[1] = s->pad[1];
      s->joy_auto[2] = s->joy_auto[3] = 0;
      s->autojoy_busy_lines = 3;
    }
    s->frame_done = true;
  }
  if ((s->nmitimen & 0x30) == 0x20 && s->vpos == s->vtime) raise_irq(s);
}

void snes_run_events(Snes *s) {
  s->in_events = true;
  for (;;) {
    uint64_t h = s->cycles - s->line_start;
    uint8_t irqmode = s->nmitimen & 0x30;
    if (!s->hirq_done_line && (irqmode == 0x10 || (irqmode == 0x30 && s->vpos == s->vtime)) &&
        h >= (uint64_t)s->htime * 4 + 14) {
      s->hirq_done_line = true;
      raise_irq(s);
    }
    if (!s->refresh_done_line && h >= 536) {
      s->refresh_done_line = true;
      s->cycles += 40;
      continue;
    }
    if (!s->hdma_done_line && h >= SNES_HDMA_H) {
      s->hdma_done_line = true;
      if (s->vpos <= vis_lines(s) && s->hdmaen) hdma_run(s);
      continue;
    }
    if (h >= SNES_LINE_CYCLES) { new_line(s); continue; }
    break;
  }
  s->in_events = false;
}

//---------------------------------------------------------------- top level

static void bind(Snes *s) {
  s->cpu.ctx = s;
  s->cpu.read = cpu_read_cb;
  s->cpu.write = cpu_write_cb;
  s->cpu.idle = cpu_idle_cb;
}

int snes_rom_game(const uint8_t *rom, uint32_t size) {
  if (!rom || size < 0x8000) return SNES_GAME_UNKNOWN;
  //The header's checksum word ($7FDE) and its complement ($7FDC).
  uint16_t inv = (uint16_t)(rom[0x7FDC] | rom[0x7FDD] << 8), sum = (uint16_t)(rom[0x7FDE] | rom[0x7FDF] << 8);
  if ((uint16_t)(sum ^ inv) != 0xFFFF) return SNES_GAME_UNKNOWN;
  switch (sum) {
  case 0x55FB: return SNES_GAME_G1;
  case 0x09B7: return SNES_GAME_G2;
  case 0x4055: return SNES_GAME_G3;
  default: return SNES_GAME_UNKNOWN;
  }
}

uint8_t *snes_rom_image(const uint8_t *data, size_t n, uint32_t *out_size) {
  if (n % 0x8000 == 0x200) { data += 0x200; n -= 0x200; }
  uint32_t sz = 0x8000;
  while (sz < n) sz <<= 1;
  uint8_t *img = calloc(sz, 1);
  if (!img) return NULL;
  memcpy(img, data, n);
  //The part above the largest power of two repeats until the end.
  uint32_t base = 0x8000;
  while (base * 2 <= n) base *= 2;
  uint32_t rest = (uint32_t)n - base;
  for (uint32_t i = (uint32_t)n; i < sz; i++) img[i] = rest ? img[base + (i - base) % rest] : img[i - base];
  *out_size = sz;
  return img;
}

void snes_init(Snes *s, const uint8_t *rom, uint32_t size) {
  memset(s, 0, sizeof(*s));
  s->rom = rom;
  s->rom_size = size;
  s->game = (uint8_t)snes_rom_game(rom, size);
  //Cartridge type $F3: Cx4.
  s->has_cx4 = rom && size >= 0x8000 && rom[0x7FD6] == 0xF3;
  memset(s->sram, 0, sizeof(s->sram));
  bind(s);
}

void snes_reset(Snes *s) {
  memset(s->wram, 0, sizeof(s->wram));
  s->cycles = s->line_start = 0;
  s->vpos = 0;
  s->frame_count = 0;
  s->open_bus = 0;
  s->wram_addr = 0;
  s->nmitimen = 0; s->wrio = 0xFF; s->memsel = 0;
  s->wrmpya = s->wrmpyb = 0xFF;
  s->wrdiv = 0xFFFF; s->rddiv = s->rdmpy = 0;
  s->htime = s->vtime = 0x1FF;
  s->nmi_flag = s->irq_flag = s->in_vblank = false;
  s->hdmaen = 0;
  memset(s->dma, 0xFF, sizeof(s->dma));
  for (int i = 0; i < 8; i++) {
    s->dma[i].hdma_do_transfer = false;
    s->dma[i].hdma_terminated = true;
  }
  memset(s->joy_auto, 0, sizeof(s->joy_auto));
  s->hdma_done_line = s->refresh_done_line = s->hirq_done_line = false;
  ppu_reset(&s->ppu);
  apu_reset(&s->apu);
  cx4_reset(&s->cx4);
  bind(s);
  cpu_reset(&s->cpu);
}

static inline void record_coverage(Snes *s) {
  Cpu *c = &s->cpu;
  uint32_t pc = (uint32_t)c->pb << 16 | c->pc;
  int st = (c->e ? 4 : 0) | ((c->p & CPU_M) ? 2 : 0) | ((c->p & CPU_X) ? 1 : 0);
  s->coverage[pc] |= (uint8_t)(1 << st);
}

void snes_run_frame(Snes *s) {
  s->frame_done = false;
  while (!s->frame_done) {
    if (s->coverage && !s->cpu.waiting) record_coverage(s);
    if (s->exec_hook && s->exec_hook(s)) continue;
    cpu_step(&s->cpu);
  }
  apu_sync(s);
}

void snes_clone(Snes *dst, const Snes *src) {
  memcpy(dst, src, sizeof(Snes));
  bind(dst);
}

#define STATE_MAGIC 0x4D583331u

size_t snes_state_size(void) { return sizeof(uint32_t) * 2 + sizeof(Snes); }

void snes_state_save(Snes *s, uint8_t *out) {
  uint32_t hdr[2] = {STATE_MAGIC, (uint32_t)sizeof(Snes)};
  memcpy(out, hdr, sizeof(hdr));
  memcpy(out + sizeof(hdr), s, sizeof(Snes));
}

bool snes_state_load(Snes *s, const uint8_t *in) {
  uint32_t hdr[2];
  memcpy(hdr, in, sizeof(hdr));
  if (hdr[0] != STATE_MAGIC || hdr[1] != sizeof(Snes)) return false;
  const uint8_t *rom = s->rom;
  uint32_t rom_size = s->rom_size;
  uint8_t *cov = s->coverage;
  SnesExecHook hook = s->exec_hook;
  const PpuEnh *enh = s->ppu.enh;
  memcpy(s, in + sizeof(hdr), sizeof(Snes));
  s->rom = rom; s->rom_size = rom_size; s->coverage = cov; s->exec_hook = hook;
  s->ppu.enh = enh;
  bind(s);
  return true;
}
