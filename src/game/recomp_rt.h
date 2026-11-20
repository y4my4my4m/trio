#ifndef GAME_RECOMP_RT_H
#define GAME_RECOMP_RT_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"

#ifdef __cplusplus
extern "C" {
#define RT_NORETURN [[noreturn]]
#else
#define RT_NORETURN _Noreturn
#endif

//Runtime for the recompiled code in src/game/gen (see docs/RECOMP.md).

//ret argument of a hook-entered function: any popped return address ends it.
#define RT_ANY 0xFFFFFFFFu

typedef void RtFn(Snes *s, uint32_t ret, uint32_t ent);

//key = pc24 << 2 | m << 1 | x. Table is sorted by key.
typedef struct {
  uint32_t key;
  RtFn *fn;
  uint32_t ent;
} RtEntry;

//One table per game (Snes.game 1-3), generated with --prefix x1/x2/x3.
//A game whose ROM was not there at build time has an empty table.
extern const RtEntry rt_dispatch_g1[], rt_dispatch_g2[], rt_dispatch_g3[];
extern const uint32_t rt_dispatch_g1_n, rt_dispatch_g2_n, rt_dispatch_g3_n;
//True when the game's code was recompiled into this build.
bool rt_has_game(int game);

//Instructions executed in recompiled code / interpreter steps taken while
//the hook is installed. Interrupt entries and WAI/STP idles are not counted.
extern uint64_t rt_insn_count;
extern uint64_t rt_interp_count;

//Installs or removes rt_hook as s->exec_hook (the table of s->game).
void rt_install(Snes *s, bool on);
bool rt_hook(Snes *s);

//Abandons all recompiled frames; the hook returns and the interpreter
//resumes at PB:PC.
RT_NORETURN void rt_exit(void);

//Runs the recompiled code registered for PB:PC and the current P.M/P.X,
//passing ret; rt_exit when none is registered.
void rt_dyn(Snes *s, uint32_t ret);

//False when the environment sets TRIO_RECOMP=0.
bool rt_enabled_by_env(void);

//Overrides (tools/overrides.txt) run only while true. Default true; false
//when the environment sets TRIO_OVERRIDES=0.
extern bool rt_overrides_on;

//Emulated stack and memory helpers for override code. Bank $7E/$7F WRAM
//address a is a 17-bit offset from $7E:0000.
void rt_push8(Snes *s, uint8_t v);
uint8_t rt_pull8(Snes *s);
//RTS: PC = pulled word + 1.
void rt_rts(Snes *s);
uint16_t rt_wram16(const Snes *s, uint32_t a);
void rt_set_wram16(Snes *s, uint32_t a, uint16_t v);
//Sets N and Z from v (width per P.X for index registers, P.M otherwise).
void rt_set_nz(Snes *s, uint16_t v, bool index);

#ifdef __cplusplus
}
#endif

#endif
