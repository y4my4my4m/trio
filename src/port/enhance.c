#include "enhance.h"
#include <string.h>
#include "game/recomp_rt.h"
#include "game/widescreen.h"

//Stage detection: the BG streamers run every frame of stage gameplay (and
//of the attract demo), never on the title, stage select, pause or password
//screens. They also blink on for a frame or two during fades, hence the
//hysteresis before switching the width.
#define STAGE_ON_FRAMES 10
#define STAGE_OFF_FRAMES 4

//WRAM ($7E:xxxx) per game: health is the player object's +$27 (bits 0-6,
//16 at start, more with life upgrades; $A0 while dying), then the lives.
static const struct { uint16_t health, lives; } ram[4] = {
  {0, 0}, {0x0BCF, 0x1F80}, {0x09FF, 0x1FB3}, {0x09FF, 0x1FB4},
};

static uint8_t *g_rom;
static uint32_t g_rom_size;
static bool g_supported;

bool enh_widescreen_supported(void) { return g_supported; }

void enh_init(Enhance *e, Snes *s, uint8_t *rom, uint32_t rom_size) {
  memset(e, 0, sizeof(*e));
  g_rom = rom;
  g_rom_size = rom_size;
  g_supported = ws_check_rom(rom, rom_size);
  e->lives = -1;
  e->ppu.wide_layers = 0x13;     //BG1, BG2, OBJ; BG3 carries the HUD and stays 4:3
  e->ppu.wide_win_center = false;  //extended columns take the window state of the nearest edge
  s->ppu.enh = &e->ppu;
}

void enh_reset(Enhance *e) {
  e->level_on = e->level_off = 0;
  e->in_stage = false;
  e->stage_entered = false;
  e->ppu.wide = 0;
  cx4_wide = 0;
  e->charge_pulse = 0;
  e->hp_max = 0;
  e->lives = -1;
}

void enh_apply(Enhance *e, Snes *s, const Settings *cfg) {
  //Widescreen needs the overrides, which only exist in the recompiled build.
  bool can = g_supported && s->exec_hook && rt_overrides_on;
  int w = can ? settings_wide_columns(cfg->widescreen, cfg->pixel_aspect) : 0;
  if (w != e->wide_cfg) {
    e->wide_cfg = w;
    ws_active_wide = w;
    ws_apply_rom(g_rom, g_rom_size, w);
  }
  e->ppu.no_sprite_limit = cfg->no_sprite_limit;
  if (!w) { e->ppu.wide = 0; cx4_wide = 0; }
}

uint16_t enh_input(Enhance *e, const Settings *cfg, uint16_t pad) {
  bool shoot = pad & PAD_Y;
  bool pressed = shoot && !e->prev_shoot;
  e->prev_shoot = shoot;
  //Both helpers act only in stage gameplay, so menus see the real button.
  if (!e->in_stage) { e->charge_pulse = 0; return pad; }
  if (cfg->auto_charge) {
    //The blaster charges while Y is held and fires on release: hold Y for the
    //game, and turn a press into a short release so the charge goes off.
    if (pressed) e->charge_pulse = 2;
    if (e->charge_pulse > 0) { e->charge_pulse--; return (uint16_t)(pad & ~PAD_Y); }
    return (uint16_t)(pad | PAD_Y);
  }
  if (cfg->turbo_fire && shoot) {
    e->turbo_t++;
    if ((e->turbo_t / cfg->turbo_rate) & 1) pad &= (uint16_t)~PAD_Y;
  } else {
    e->turbo_t = 0;
  }
  return pad;
}

void enh_after_frame(Enhance *e, Snes *s, const Settings *cfg) {
  bool ran = s->frame_count - ws_level_frame <= 1;
  e->stage_entered = false;
  if (ran) { e->level_on++; e->level_off = 0; }
  else { e->level_off++; e->level_on = 0; }
  if (!e->in_stage && e->level_on >= STAGE_ON_FRAMES) { e->in_stage = true; e->stage_entered = true; e->hp_max = 0; }
  else if (e->in_stage && e->level_off >= STAGE_OFF_FRAMES) e->in_stage = false;
  int w = e->in_stage ? e->wide_cfg : 0;
  e->ppu.wide = w;
  cx4_wide = w;
  //The HUD meters are written straight into the OAM slots below the Cx4
  //builder's first free slot ($6626, 16 in stages); everything the builder
  //places is a world object.
  //Assists (they change machine state; off by default).
  uint16_t W_HEALTH = ram[s->game & 3].health, W_LIVES = ram[s->game & 3].lives;
  if (e->in_stage && W_HEALTH) {
    uint8_t hp = s->wram[W_HEALTH];
    int v = hp & 0x7F;
    if (!(hp & 0x80) && v > e->hp_max && v <= 0x40) e->hp_max = v;
    if (cfg->inf_health && !(hp & 0x80) && v > 0 && v < e->hp_max) s->wram[W_HEALTH] = (uint8_t)((hp & 0x80) | e->hp_max);
    int lives = s->wram[W_LIVES];
    if (cfg->inf_lives && e->lives >= 0 && lives < e->lives) s->wram[W_LIVES] = (uint8_t)(lives = e->lives);
    e->lives = lives;
  }
  bool hud = w && cfg->wide_hud;
  //G1 builds its OAM on the CPU from slot 16 on; its meters are in 0-15.
  e->ppu.hud_objs = !hud ? 0 : s->has_cx4 ? s->cx4.ram[0x626] : 16;
  e->ppu.hud_dx = hud ? w : 0;
}
