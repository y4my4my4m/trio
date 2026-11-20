#include "cpu_ops.h"

void cpu_reset(Cpu *c) {
  c->e = true;
  c->p = 0x34;
  c->s = 0x01ff;
  c->d = 0;
  c->db = c->pb = 0;
  c->x &= 0xff;
  c->y &= 0xff;
  c->waiting = c->stopped = false;
  c->pc = cpu_rd(c, 0xfffc) | cpu_rd(c, 0xfffd) << 8;
}

void cpu_step(Cpu *c) {
  if (c->stopped) { cpu_io(c); return; }
  if (c->waiting) {
    if (!c->nmi_pending && !c->irq_line) { cpu_io(c); return; }
    c->waiting = false;
  }
  if (c->nmi_pending) {
    c->nmi_pending = false;
    cpu_hw_interrupt(c, 0xffea, 0xfffa);
    return;
  }
  if (c->irq_line && !(c->p & CPU_I)) {
    cpu_hw_interrupt(c, 0xffee, 0xfffe);
    return;
  }
  cpu_exec(c, cpu_f8(c));
}
