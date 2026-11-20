#ifndef SNES_APU_H
#define SNES_APU_H

#include <stdint.h>
#include <stdbool.h>

//S-SMP (SPC700) + S-DSP. SPC700 clock 1.024 MHz; DSP emits one stereo
//sample every 32 SPC cycles (32000 Hz).

#define APU_SAMPLE_RATE 32000
#define APU_RING 8192    //stereo frames, power of two

typedef struct Apu {
  uint8_t ram[0x10000];

  //SPC700 registers.
  uint8_t a, x, y, sp, psw;
  uint16_t pc;
  bool stopped;

  //$F0-$FF state.
  uint8_t control;
  uint8_t dsp_addr;
  uint8_t port_in[4];   //written by the S-CPU, read by SPC at $F4-$F7
  uint8_t port_out[4];  //written by SPC, read by the S-CPU at $2140-$2143
  uint8_t timer_target[3];
  uint8_t timer_stage[3];
  uint8_t timer_counter[3];  //4-bit
  bool timer_enabled[3];
  int timer_div[3];

  uint64_t cycles;       //SPC cycles executed
  int sample_div;        //counts to 32

  uint8_t dsp_opaque[4096];  //S-DSP state, layout private to dsp code

  int16_t ring[APU_RING][2];
  uint32_t ring_w, ring_r;   //free-running, masked by APU_RING-1
} Apu;

void apu_reset(Apu *a);
void apu_run(Apu *a, uint64_t target_cycles);  //run until a->cycles >= target_cycles
uint8_t apu_cpu_read(Apu *a, int port);        //port 0..3
void apu_cpu_write(Apu *a, int port, uint8_t v);
int apu_pop_samples(Apu *a, int16_t *out, int max_frames);  //interleaved L,R

#endif
