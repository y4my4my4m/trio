#ifndef GAME_RECOMP_GEN_H
#define GAME_RECOMP_GEN_H

//Included by the generated files only. The Cpu is the first member of Snes.
//Bus access uses the inline equivalents of snes_read/snes_write/snes_idle
//from snes.h (same cycles, values, open bus and event order as the
//interpreter's Cpu callbacks).
#define CPU_OPS_READ(c, a) snes_read_inline((Snes *)(c), (a))
#define CPU_OPS_WRITE(c, a, v) snes_write_inline((Snes *)(c), (a), (v))
#define CPU_OPS_IDLE(c) snes_add_cycles((Snes *)(c), 6)
//Recompiled code lies in ROM ($8000-$FFFF of banks $00/$80/$81).
#define CPU_OPS_FETCH(c, a) snes_read_rom((Snes *)(c), (a))

#include "snes/snes.h"
#include "snes/cpu_ops.h"
#include "game/recomp_rt.h"

#if defined(__GNUC__)
#define RT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RT_UNLIKELY(x) (x)
#endif

#define PC24 ((uint32_t)c->pb << 16 | c->pc)

//Interrupt and frame-end check after every instruction, matching the
//interpreter loop (snes_run_frame, cpu_step).
#define POLL() do { \
  rt_insn_count++; \
  if (RT_UNLIKELY(c->nmi_pending | s->frame_done | (c->irq_line & !(c->p & CPU_I)))) rt_exit(); \
} while (0)

//Opcode fetch, execution, poll.
#define OP(h) do { (void)cpu_f8(c); cpu_op_##h(c); POLL(); } while (0)

//After RTS/RTL/RTI: return to the C caller when PB:PC is the continuation it
//expects, otherwise resume in the interpreter at PB:PC.
#define RET() do { if (ret != RT_ANY && ret != PC24) rt_exit(); return; } while (0)

//After an override took over: return to the C caller when PB:PC is its
//continuation, else run the recompiled code at PB:PC.
#define RT_CONTINUE() do { if (ret != RT_ANY && ret == PC24) return; rt_dyn(s, ret); return; } while (0)

#endif
