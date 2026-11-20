#ifndef SNES_DSP_H
#define SNES_DSP_H

#include <stdint.h>

//S-DSP. State pointer is Apu.dsp_opaque.

void dsp_reset(void *dsp);
uint8_t dsp_read(void *dsp, uint8_t addr);
void dsp_write(void *dsp, uint8_t addr, uint8_t val);
void dsp_step_sample(void *dsp, uint8_t *aram, int16_t out[2]);  //one 32 kHz frame

#endif
