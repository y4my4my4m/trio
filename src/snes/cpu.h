#ifndef SNES_CPU_H
#define SNES_CPU_H

#include <stdint.h>
#include <stdbool.h>

//WDC 65C816 core. Instruction-stepped; all bus traffic and internal
//cycles go through the callbacks so the owner accounts master cycles.

//P register bits.
enum {
  CPU_C = 0x01, CPU_Z = 0x02, CPU_I = 0x04, CPU_D = 0x08,
  CPU_X = 0x10, CPU_M = 0x20, CPU_V = 0x40, CPU_N = 0x80,
};

typedef struct Cpu {
  uint16_t a, x, y, s, d;
  uint8_t db, pb;        //data bank, program bank
  uint16_t pc;
  uint8_t p;             //NVMXDIZC
  bool e;                //emulation mode
  bool waiting;          //WAI executed, cleared by NMI/IRQ
  bool stopped;          //STP executed
  bool nmi_pending;      //edge-latched by owner
  bool irq_line;         //level, asserted by owner

  void *ctx;
  uint8_t (*read)(void *ctx, uint32_t addr);
  void (*write)(void *ctx, uint32_t addr, uint8_t val);
  void (*idle)(void *ctx);  //one internal (IO) cycle
} Cpu;

void cpu_reset(Cpu *c);  //loads PC from $00FFFC, e=1, P=$34, S=$01FF
void cpu_step(Cpu *c);   //one instruction, or one interrupt entry, or one idle if waiting

#endif
