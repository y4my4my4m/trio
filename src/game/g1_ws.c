#include "game/gen/g1/funcs.h"
#include "game/recomp_rt.h"
#include "game/widescreen.h"

//Widescreen for game 1 (docs/WIDESCREEN.md): the G3 streamers and
//spawner, at other addresses. The camera copies go to $1FA3 (X) and $1FA7
//(previous X); the cameras are $1E4D/$1E6A (BG1) and $1E8D/$1EAA (BG2).

static void shift(Snes *s, uint32_t src_cur, uint32_t src_prev) {
  int w = ws_active_wide;
  ws_level_frame = s->frame_count;
  rt_set_wram16(s, 0x1FA3, (uint16_t)(rt_wram16(s, src_cur) - w));
  rt_set_wram16(s, 0x1FA7, (uint16_t)(rt_wram16(s, src_prev) - w));
}

//80:BA87, right after BG1's camera copy (sub_80BA69).
bool ovr_G1WideCamBG1(Snes *s) {
  shift(s, 0x1E4D, 0x1E6A);
  return false;
}

//80:BABE, right after BG2's camera copy.
bool ovr_G1WideCamBG2(Snes *s) {
  shift(s, 0x1E8D, 0x1EAA);
  return false;
}

//80:DCA5 JSR $DCF1: the spawner's horizontal pass (ws_spawn_columns).
bool ovr_G1WideSpawnColumns(Snes *s) {
  return ws_spawn_columns(s, 0x1E4D, 0xDCA5, 0xDCF1);
}
