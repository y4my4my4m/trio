#include "game/gen/g2/funcs.h"
#include "game/recomp_rt.h"
#include "game/widescreen.h"

//Widescreen for game 2 (docs/WIDESCREEN.md): the G3 streamers and
//spawner, at other addresses. The camera copies go to $1FE0 (X) and $1FE4
//(previous X); the cameras are G3's ($1E5D/$1E7A BG1, $1E9D/$1EBA BG2).

static void shift(Snes *s, uint32_t src_cur, uint32_t src_prev) {
  int w = ws_active_wide;
  ws_level_frame = s->frame_count;
  rt_set_wram16(s, 0x1FE0, (uint16_t)(rt_wram16(s, src_cur) - w));
  rt_set_wram16(s, 0x1FE4, (uint16_t)(rt_wram16(s, src_prev) - w));
}

//00:B7EA, right after BG1's camera copy (sub_00B7CC).
bool ovr_G2WideCamBG1(Snes *s) {
  shift(s, 0x1E5D, 0x1E7A);
  return false;
}

//00:B821, right after BG2's camera copy.
bool ovr_G2WideCamBG2(Snes *s) {
  shift(s, 0x1E9D, 0x1EBA);
  return false;
}

//00:DC9D JSR $DCE9: the spawner's horizontal pass (ws_spawn_columns).
bool ovr_G2WideSpawnColumns(Snes *s) {
  return ws_spawn_columns(s, 0x1E5D, 0xDC9D, 0xDCE9);
}
