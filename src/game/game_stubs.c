#include "game/recomp_rt.h"
#include "game/widescreen.h"
#include "port/port_api.h"

//State shared by the games' overrides, and empty dispatch tables for the
//games whose ROM was not there at build time (they run in the interpreter).

int ws_active_wide;
uint64_t ws_level_frame;

#ifndef TRIO_HAVE_G1
const RtEntry rt_dispatch_g1[1] = {{0, 0, 0}};
const uint32_t rt_dispatch_g1_n = 0;
#endif
#ifndef TRIO_HAVE_G2
const RtEntry rt_dispatch_g2[1] = {{0, 0, 0}};
const uint32_t rt_dispatch_g2_n = 0;
#endif
#ifndef TRIO_HAVE_G3
const RtEntry rt_dispatch_g3[1] = {{0, 0, 0}};
const uint32_t rt_dispatch_g3_n = 0;
#endif
