#include "game/battle_hooks.h"
#include "game/recomp_rt.h"

//Battle mode's changes to the games' own code (docs/BATTLE.md).

BattleBox battle_box;

//The on-screen test every game runs for its shots (and many objects): with
//D = the object, it clears $0E and returns when the object is more than a
//little off the camera, which deletes a shot. In battle mode an object
//anywhere in the arena counts as on screen, so shots fly across all of it:
//execution continues at the test's on-screen branch.
static bool onscreen(Snes *s, uint16_t pass_pc) {
  if (!battle_box.on) return false;
  int x = (int16_t)rt_wram16(s, (uint32_t)s->cpu.d + 0x05), y = (int16_t)rt_wram16(s, (uint32_t)s->cpu.d + 0x08);
  if (x < battle_box.x0 || x >= battle_box.x1 || y < battle_box.y0 || y >= battle_box.y1) return false;
  s->cpu.pc = pass_pc;
  return true;
}

bool ovr_G1OnScreen(Snes *s) { return onscreen(s, 0x80D9); }   //82:80B4, on screen at 82:80D9
bool ovr_G2OnScreen(Snes *s) { return onscreen(s, 0xD87E); }   //00:D859, on screen at 00:D87E
bool ovr_G3OnScreen(Snes *s) { return onscreen(s, 0xD65B); }   //02:D636, on screen at 02:D65B

//In battle mode, execution continues at next_pc: a test of the game's is
//skipped.
static bool battle_skip(Snes *s, uint16_t next_pc) {
  if (!battle_box.on) return false;
  s->cpu.pc = next_pc;
  return true;
}

//The L/R weapon switch does nothing while any of the hero's shots is on screen
//(+35, the shot count, nonzero). In battle mode it switches at once: the
//`LDA $35 / BNE` test is skipped.
bool ovr_G1SwitchShots(Snes *s) { return battle_skip(s, 0x99D2); }   //81:99CE
bool ovr_G2SwitchShots(Snes *s) { return battle_skip(s, 0xBEE0); }   //08:BEDC
bool ovr_G3SwitchShots(Snes *s) { return battle_skip(s, 0xAFD7); }   //04:AFD3

//G1's Fireball and G2's Uppercut need full HP (+$27 compared with $20):
//in battle mode they don't. The check is skipped; the others (the move
//owned, the blaster out, standing) stay.
bool ovr_G1FireballHp(Snes *s) { return battle_skip(s, 0xA0F2); }   //81:A0EC
bool ovr_G2UppercutHp(Snes *s) { return battle_skip(s, 0xC5CB); }  //08:C5C5
