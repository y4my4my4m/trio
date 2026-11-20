#ifndef SNES_SNES_H
#define SNES_SNES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cpu.h"
#include "ppu.h"
#include "apu.h"
#include "cx4.h"

//NTSC timing in master cycles (21.477272 MHz).
#define SNES_MASTER_HZ 21477272
#define SNES_LINE_CYCLES 1364
#define SNES_LINES 262
#define SNES_HDMA_H 1104
#define SNES_HBLANK_START 1096

//Joypad bits as seen in $4218/$4219 (16-bit, JOY1H:JOY1L).
enum {
  PAD_R = 0x0010, PAD_L = 0x0020, PAD_X = 0x0040, PAD_A = 0x0080,
  PAD_RIGHT = 0x0100, PAD_LEFT = 0x0200, PAD_DOWN = 0x0400, PAD_UP = 0x0800,
  PAD_START = 0x1000, PAD_SELECT = 0x2000, PAD_Y = 0x4000, PAD_B = 0x8000,
};

typedef struct DmaChannel {
  uint8_t dmap, bbad;
  uint16_t a1t;
  uint8_t a1b;
  uint16_t das;
  uint8_t dasb;
  uint16_t a2a;
  uint8_t ntrl;
  uint8_t unused;
  bool hdma_do_transfer;
  bool hdma_terminated;
} DmaChannel;

typedef struct Snes Snes;

//Returns true if it handled execution at the current PC (recompiled code).
typedef bool (*SnesExecHook)(Snes *s);

struct Snes {
  Cpu cpu;
  Ppu ppu;
  Apu apu;
  Cx4 cx4;

  uint8_t wram[0x20000];
  uint8_t sram[0x2000];   //unused by the X games (no battery RAM), kept for the bus map
  const uint8_t *rom;
  uint32_t rom_size;
  bool sram_dirty;

  uint64_t cycles;         //master cycles since power-on
  uint64_t line_start;     //master cycle at H=0 of current line
  uint16_t vpos;
  uint64_t frame_count;
  bool frame_done;

  uint8_t open_bus;
  uint32_t wram_addr;

  uint8_t nmitimen, wrio, memsel;
  uint8_t wrmpya, wrmpyb;
  uint16_t wrdiv, rddiv, rdmpy;
  uint16_t htime, vtime;
  bool nmi_flag;           //$4210 bit 7
  bool irq_flag;           //$4211 bit 7
  bool in_vblank;
  bool hdma_done_line, refresh_done_line, hirq_done_line;
  bool in_events;
  uint8_t hdmaen;
  DmaChannel dma[8];

  uint16_t pad[2];         //current input, set by frontend
  uint16_t joy_auto[4];    //$4218-$421F
  uint16_t joy_shift[2];
  bool joy_strobe;
  int autojoy_busy_lines;

  //Coverage trace: per 24-bit PC, bitmask of (e,m,x) states seen.
  uint8_t *coverage;

  SnesExecHook exec_hook;

  //Which game the ROM is (SNES_GAME_*, from its header) and whether the
  //cartridge has the Cx4 at $6000-$7FFF (G2, G3). Set by snes_init.
  uint8_t game;
  bool has_cx4;
  //When set, the sound chip is not run (battle mode's fighter copies: their
  //sounds are played elsewhere). The S-CPU then reads stale APU ports.
  bool no_apu;
};

enum { SNES_GAME_UNKNOWN, SNES_GAME_G1, SNES_GAME_G2, SNES_GAME_G3 };

//Game of a ROM image (header title), SNES_GAME_UNKNOWN if none of the three.
int snes_rom_game(const uint8_t *rom, uint32_t size);
//ROM file contents to the image the bus reads: a copier header is dropped
//and the image is padded to a power of two the way a LoROM cartridge
//mirrors it (1.5 MiB: the last 512 KiB repeat). Returns a malloc'd buffer.
uint8_t *snes_rom_image(const uint8_t *data, size_t n, uint32_t *out_size);
//The ROM of `game` (SNES_GAME_*) in directory dir as a malloc'd image:
//gN.sfc when it is that game, else any .sfc / .smc file there whose
//header says so. NULL if there is none.
uint8_t *snes_rom_find(const char *dir, int game, uint32_t *out_size);

//When set, scanlines are not rendered (machine state is unaffected). Used by
//the arena, which runs one headless copy of the game per fighter.
extern bool snes_skip_render;

void snes_init(Snes *s, const uint8_t *rom, uint32_t size);
void snes_reset(Snes *s);
void snes_run_frame(Snes *s);

uint8_t snes_read(Snes *s, uint32_t addr);
void snes_write(Snes *s, uint32_t addr, uint8_t val);
uint8_t snes_peek(Snes *s, uint32_t addr);  //no side effects, no cycles
void snes_idle(Snes *s);                    //one CPU internal cycle (6 master)

//Line scheduler: H-IRQ, DRAM refresh, HDMA, line end. Runs events due at s->cycles.
void snes_run_events(Snes *s);

//Advances the master clock by n cycles and runs due line events.
static inline void snes_add_cycles(Snes *s, int n) {
  s->cycles += (uint64_t)n;
  if (s->in_events) return;
  //Exact precondition for snes_run_events doing nothing: no H-IRQ mode, and
  //no refresh, HDMA or line end due yet.
  if (!(s->nmitimen & 0x10)) {
    uint64_t h = s->cycles - s->line_start;
    if (h < 536) return;
    if (s->refresh_done_line && h < SNES_LINE_CYCLES && (h < SNES_HDMA_H || s->hdma_done_line))
      return;
  }
  snes_run_events(s);
}

//snes_read restricted to ROM at $8000-$FFFF of banks $00-$3F/$80-$BF: same
//cycles, value and open bus.
static inline uint8_t snes_read_rom(Snes *s, uint32_t addr) {
  uint8_t bank = (uint8_t)(addr >> 16);
  snes_add_cycles(s, ((bank & 0x80) && (s->memsel & 1)) ? 6 : 8);
  uint8_t v = s->rom[((((uint32_t)bank & 0x7F) << 15) | (addr & 0x7FFF)) & (s->rom_size - 1)];
  s->open_bus = v;
  return v;
}

//snes_read/snes_write with the WRAM cases inline ($0000-$1FFF of banks
//$00-$3F/$80-$BF, banks $7E/$7F: 8 cycles); other addresses call through.
static inline uint8_t snes_read_inline(Snes *s, uint32_t addr) {
  uint8_t bank = (uint8_t)(addr >> 16);
  uint32_t i;
  if (!(bank & 0x40) && (addr & 0xFFFF) < 0x2000) i = addr & 0x1FFF;
  else if ((bank & 0xFE) == 0x7E) i = addr & 0x1FFFF;
  else return snes_read(s, addr);
  snes_add_cycles(s, 8);
  uint8_t v = s->wram[i];
  s->open_bus = v;
  return v;
}

static inline void snes_write_inline(Snes *s, uint32_t addr, uint8_t val) {
  uint8_t bank = (uint8_t)(addr >> 16);
  uint32_t i;
  if (!(bank & 0x40) && (addr & 0xFFFF) < 0x2000) i = addr & 0x1FFF;
  else if ((bank & 0xFE) == 0x7E) i = addr & 0x1FFFF;
  else { snes_write(s, addr, val); return; }
  snes_add_cycles(s, 8);
  s->open_bus = val;
  s->wram[i] = val;
}

//Copies a whole machine (e.g. to run several copies); the CPU callbacks of
//dst are bound to dst.
void snes_clone(Snes *dst, const Snes *src);

//Save state: plain struct image; pointers are re-bound on load.
size_t snes_state_size(void);
void snes_state_save(Snes *s, uint8_t *out);
bool snes_state_load(Snes *s, const uint8_t *in);

#endif
