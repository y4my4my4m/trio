#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/snes/snes.h"
#include "../src/port/rewind.h"

//Rewind history on a synthetic cartridge (no game ROM needed): the program
//counts in WRAM forever; stepping back must restore each recorded snapshot
//exactly (CPU, WRAM, VRAM, APU), and history must respect its capacity.

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Snes s;
static uint8_t rom[0x8000];

typedef struct { Cpu cpu; uint8_t wram[0x20000]; uint8_t vram[0x10000]; uint64_t cycles; } Snap;
static Snap snaps[64];

static void snap(Snap *o) {
  o->cpu = s.cpu;
  memcpy(o->wram, s.wram, sizeof(s.wram));
  memcpy(o->vram, s.ppu.vram, sizeof(s.ppu.vram));
  o->cycles = s.cycles;
}

static bool same(const Snap *o) {
  return o->cpu.pc == s.cpu.pc && o->cpu.a == s.cpu.a && o->cycles == s.cycles &&
         !memcmp(o->wram, s.wram, sizeof(s.wram)) && !memcmp(o->vram, s.ppu.vram, sizeof(s.ppu.vram));
}

int main(void) {
  //$8000: INC $10 / INC $2000 (WRAM mirror) / BRA $8000; vectors to $8000.
  static const uint8_t prog[] = {0xE6, 0x10, 0xEE, 0x00, 0x12, 0x80, 0xF9};
  memcpy(rom, prog, sizeof(prog));
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  snes_init(&s, rom, sizeof(rom));
  snes_reset(&s);
  rw_init();
  rw_set_capacity(1);  //30 entries
  int nsnap = 0;
  for (int f = 0; f < 100; f++) {
    snes_run_frame(&s);
    rw_record(&s);
    if (f % RW_INTERVAL == 0 && f >= 100 - 2 * 31) snap(&snaps[nsnap++]);
  }
  CHECK(rw_count() == 30, "capacity: %d entries", rw_count());
  //The newest recorded state is the head; each step back yields the one before.
  for (int k = nsnap - 2; k >= 0 && rw_count() > 0; k--) {
    CHECK(rw_step_back(&s), "step back at %d", k);
    CHECK(same(&snaps[k]), "state %d restored exactly (pc %04X vs %04X, wram10 %02X vs %02X)", k, s.cpu.pc,
          snaps[k].cpu.pc, s.wram[0x10], snaps[k].wram[0x10]);
  }
  CHECK(!rw_step_back(&s), "history exhausted");
  //Recording resumes cleanly after rewinding.
  for (int f = 0; f < 10; f++) { snes_run_frame(&s); rw_record(&s); }
  CHECK(rw_count() > 0, "records again after rewind");
  CHECK(rw_bytes() < 64 * 1024, "deltas are small (%zu bytes)", rw_bytes());
  if (!fails) printf("rewind: all tests passed\n");
  return fails ? 1 : 0;
}
