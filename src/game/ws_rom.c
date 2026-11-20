#include "game/widescreen.h"
#include "game/recomp_rt.h"

//Immediate operands widened for widescreen (docs/WIDESCREEN.md). The
//interpreter and the recompiled code both fetch operands from ROM at run
//time, so rewriting them in the loaded image is enough. LoROM addresses.
typedef struct {
  uint32_t addr;           //operand address (after the opcode)
  uint16_t orig;
  int per_w;               //added: per_w * w, or per_w * w / 16 when blocks
  bool blocks;
} Patch;

//game 3.
static const Patch patches_g3[] = {
  //Tilemap streamers (camera copies are shifted by w, see widescreen.c).
  {0x00B8DF, 0x0100, 2, false},  //00:B8DE ADC #$0100  right column = cam + $100
  {0x00BA49, 0x0012, 2, true},   //00:BA48 LDA #$0012  row length, 16 px blocks
  {0x00BB8C, 0x0120, 2, false},  //00:BB8B CMP #$0120  single-block right bound
  //"Near the camera" checks: X - cam + m < r, i.e. X in [cam - m, cam - m + r).
  {0x02D594, 0x0040, 1, false},  //02:D593 ADC #$0040  sub_02D58A
  {0x02D597, 0x0180, 2, false},  //02:D596 CMP #$0180
  {0x02D61B, 0x0060, 1, false},  //02:D61A ADC #$0060  sub_02D611
  {0x02D61E, 0x01C0, 2, false},  //02:D61D CMP #$01C0
  {0x02D640, 0x0020, 1, false},  //02:D63F ADC #$0020  sub_02D636 (on-screen test)
  {0x02D643, 0x0140, 2, false},  //02:D642 CMP #$0140
};

//game 1: the same routines (FastROM, banks $80+), and its OAM builder
//runs on the CPU (80:D780 keeps a piece while x + $10 < $010F).
static const Patch patches_g1[] = {
  {0x80B64A, 0x0100, 2, false},  //80:B649 ADC #$0100  right column = cam + $100
  {0x80B7B4, 0x0012, 2, true},   //80:B7B3 LDA #$0012  row length, 16 px blocks
  {0x80B8F7, 0x0120, 2, false},  //80:B8F6 CMP #$0120  single-block right bound
  {0x828078, 0x0040, 1, false},  //82:8077 ADC #$0040  sub_82806E
  {0x82807B, 0x0180, 2, false},  //82:807A CMP #$0180
  {0x828099, 0x0060, 1, false},  //82:8098 ADC #$0060  sub_82808F
  {0x82809C, 0x01C0, 2, false},  //82:809B CMP #$01C0
  {0x8280BE, 0x0020, 1, false},  //82:80BD ADC #$0020  sub_8280B4 (on-screen test)
  {0x8280C1, 0x0140, 2, false},  //82:80C0 CMP #$0140
  {0x80D7BF, 0x0010, 1, false},  //80:D7BE ADC #$0010  OAM piece cull: kept while x + m < r
  {0x80D7C2, 0x010F, 2, false},  //80:D7C1 CMP #$010F
};

//game 2: the G3 routines at other addresses; sprites are built by the
//Cx4 like G3's (cx4_wide).
static const Patch patches_g2[] = {
  {0x00B40D, 0x0100, 2, false},  //00:B40C ADC #$0100  right column = cam + $100
  {0x00B577, 0x0012, 2, true},   //00:B576 LDA #$0012  row length, 16 px blocks
  {0x00B6BA, 0x0120, 2, false},  //00:B6B9 CMP #$0120  single-block right bound
  {0x00D81D, 0x0040, 1, false},  //00:D81C ADC #$0040  sub_00D813
  {0x00D820, 0x0180, 2, false},  //00:D81F CMP #$0180
  {0x00D83E, 0x0060, 1, false},  //00:D83D ADC #$0060  sub_00D834
  {0x00D841, 0x01C0, 2, false},  //00:D840 CMP #$01C0
  {0x00D863, 0x0020, 1, false},  //00:D862 ADC #$0020  sub_00D859 (on-screen test)
  {0x00D866, 0x0140, 2, false},  //00:D865 CMP #$0140
};

typedef struct { const Patch *p; unsigned n; } Table;

static Table table(const uint8_t *rom, uint32_t size) {
  switch (snes_rom_game(rom, size)) {
  case SNES_GAME_G1: return (Table){patches_g1, sizeof(patches_g1) / sizeof(patches_g1[0])};
  case SNES_GAME_G2: return (Table){patches_g2, sizeof(patches_g2) / sizeof(patches_g2[0])};
  case SNES_GAME_G3: return (Table){patches_g3, sizeof(patches_g3) / sizeof(patches_g3[0])};
  default: return (Table){NULL, 0};
  }
}

static uint32_t off(uint32_t a) { return ((a >> 16 & 0x7F) << 15) | (a & 0x7FFF); }

//Accepts the ROM as loaded or as ws_apply_rom left it (any width): a game
//re-entered from the game select screen, or after a battle that patched
//it for the arena, keeps its widescreen.
static uint16_t patched(const Patch *p, int w) {
  int add = p->per_w * w;
  if (p->blocks) add /= 16;
  return (uint16_t)(p->orig + add);
}

bool ws_check_rom(const uint8_t *rom, uint32_t size) {
  Table t = table(rom, size);
  if (!t.n) return false;
  uint32_t o0 = off(t.p[0].addr);
  if (o0 + 1 >= size) return false;
  int d = (rom[o0] | rom[o0 + 1] << 8) - t.p[0].orig;
  if (d < 0 || d % t.p[0].per_w) return false;
  int w = d / t.p[0].per_w;
  for (unsigned i = 0; i < t.n; i++) {
    uint32_t o = off(t.p[i].addr);
    if (o + 1 >= size || (rom[o] | rom[o + 1] << 8) != patched(&t.p[i], w)) return false;
  }
  return true;
}

void ws_apply_rom(uint8_t *rom, uint32_t size, int w) {
  Table t = table(rom, size);
  for (unsigned i = 0; i < t.n; i++) {
    const Patch *p = &t.p[i];
    uint32_t o = off(p->addr);
    if (o + 1 >= size) continue;
    uint16_t v = patched(p, w);
    rom[o] = (uint8_t)v;
    rom[o + 1] = (uint8_t)(v >> 8);
  }
}

//32 px spawn list column of world X (negative X: columns below 0).
static int column(int x) { return (x + 0x8000) >> 5; }

bool ws_spawn_columns(Snes *s, uint32_t cam, uint16_t jsr, uint16_t scan) {
  int w = ws_active_wide;
  if (!w) return false;
  uint32_t d = s->cpu.d;
  int left = (int16_t)rt_wram16(s, cam), right = left + 0x100;
  int v = (int16_t)rt_wram16(s, d);
  int dir = v >= right ? 1 : -1;
  int classic = dir > 0 ? right : left;
  int next;
  if (v == classic) {
    int k = dir > 0 ? column(classic + w) - column(classic) : column(classic) - column(classic - w);
    if (k <= 0) return false;
    next = classic + dir * 32 * k;
  } else {
    next = v - dir * 32;
  }
  rt_set_wram16(s, d, (uint16_t)next);
  //JSR scan: the classic column returns past the JSR, the others to it.
  uint16_t ret = (uint16_t)(next == classic ? jsr + 2 : jsr - 1);
  rt_push8(s, (uint8_t)(ret >> 8));
  rt_push8(s, (uint8_t)ret);
  s->cpu.pc = scan;
  return true;
}
