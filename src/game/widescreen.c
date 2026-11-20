#include "game/gen/g3/funcs.h"
#include "game/recomp_rt.h"
#include "game/widescreen.h"

//Widescreen tilemaps (docs/WIDESCREEN.md).
//
//The BG1/BG2 streamers (00:B87B/00:B889) copy the layer camera into
//$1FE4/$1FE8 (current/previous X) at 00:BC9E/00:BCD5 and draw from there:
//a 16 px column at cam (moving left) or cam+$100 (moving right), a row of
//$12 blocks from cam (vertical moves and the full redraw), and single
//blocks inside [cam-16, cam+$110). Shifting the copies left by `w` and
//widening the three constants by 2w (ws_apply_rom) makes all of them cover
//[cam-w, cam+256+w). The game's own camera variables are untouched.

static void shift(Snes *s, uint32_t dst_cur, uint32_t src_cur, uint32_t dst_prev, uint32_t src_prev) {
  int w = ws_active_wide;
  ws_level_frame = s->frame_count;
  //Recomputed from the sources, so a second pass over the same PC is harmless.
  rt_set_wram16(s, dst_cur, (uint16_t)(rt_wram16(s, src_cur) - w));
  rt_set_wram16(s, dst_prev, (uint16_t)(rt_wram16(s, src_prev) - w));
}

//00:BCBC, right after BG1's camera copy. Runs the original instruction.
bool ovr_WideCamBG1(Snes *s) {
  shift(s, 0x1FE4, 0x1E5D, 0x1FE8, 0x1E7A);
  return false;
}

//00:BCF3, right after BG2's camera copy.
bool ovr_WideCamBG2(Snes *s) {
  shift(s, 0x1FE4, 0x1E9D, 0x1FE8, 0x1EBA);
  return false;
}

//00:DE87 JSR $DED3: the spawner's horizontal pass (ws_spawn_columns).
bool ovr_WideSpawnColumns(Snes *s) {
  return ws_spawn_columns(s, 0x1E5D, 0xDE87, 0xDED3);
}
