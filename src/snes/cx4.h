#ifndef SNES_CX4_H
#define SNES_CX4_H

#include <stdint.h>
#include <stdbool.h>

//Cx4 (HG51B169), high-level emulation.
//
//The chip is visible at $6000-$7FFF of banks $00-$3F/$80-$BF. $6000-$6BFF is
//its data RAM (3 KiB, the game builds OAM and bitmaps there), $7F40-$7FAF are
//the registers. The game drives it through a few entry points:
//  $7F47 write        DMA ROM -> RAM (src $7F40-42, len $7F43-44, dst $7F45-46)
//  $7F4F write        start command (sub-function in $7F4D for command $00)
//  $7F5E read         status, bit 6 = busy (always idle here: commands run
//                     to completion on the write)
//Parameters and results use $7F80-$7FAF. The data ROM of the real chip is not
//needed: the trig tables it holds are computed.

typedef struct Cx4 {
  uint8_t ram[0x2000];     //$6000-$7FFF as one flat block
} Cx4;

//Frontend enhancement (not machine state): OAM pieces are kept up to this
//many pixels beyond each side of the classic 256 px screen (widescreen).
extern int cx4_wide;

void cx4_reset(Cx4 *c);
uint8_t cx4_read(Cx4 *c, uint16_t addr);
//rom/rom_size: LoROM image, wram: 128 KiB WRAM; DMA sources and command data pointers.
void cx4_write(Cx4 *c, uint16_t addr, uint8_t v, const uint8_t *rom, uint32_t rom_size, const uint8_t *wram);

#endif
