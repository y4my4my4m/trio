#ifndef PORT_ENHANCE_H
#define PORT_ENHANCE_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

//Per-frame glue between the settings and the machine: widescreen state,
//sprite limit, and the input helpers (turbo fire, auto-charge).

typedef struct Enhance {
  PpuEnh ppu;              //bound to Snes.ppu.enh
  int wide_cfg;            //columns the ROM patches and streamers use
  int level_on, level_off; //hysteresis counters for "stage running"
  bool in_stage;
  bool stage_entered;      //in_stage went true this frame
  int turbo_t;
  int charge_pulse;        //frames left of the auto-charge release
  bool prev_shoot;
  int hp_max;              //highest health seen this stage
  int lives;               //lives at the last frame, -1 unknown
} Enhance;

//rom: the mutable ROM buffer the Snes reads (widescreen patches it).
void enh_init(Enhance *e, Snes *s, uint8_t *rom, uint32_t rom_size);
//After a reset or state load.
void enh_reset(Enhance *e);
//Applies settings that change the ROM image or PPU width. Call when the
//settings change and once at start.
void enh_apply(Enhance *e, Snes *s, const Settings *cfg);
//Before snes_run_frame: returns the pad value the game should see.
uint16_t enh_input(Enhance *e, const Settings *cfg, uint16_t pad);
//After snes_run_frame: updates the stage detector and the active width.
void enh_after_frame(Enhance *e, Snes *s, const Settings *cfg);
//False when the ROM does not match what widescreen expects.
bool enh_widescreen_supported(void);

#ifdef __cplusplus
}
#endif

#endif
