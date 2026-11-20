#ifndef SNES_CPU_OPS_H
#define SNES_CPU_OPS_H

#include "cpu.h"

//65C816 instruction semantics shared by the interpreter (cpu.c) and the
//recompiled code (src/game/gen). cpu_op_XX executes opcode XX after its
//opcode byte has been fetched; operands are fetched from PB:PC.
//
//Bus access goes through CPU_OPS_READ/WRITE/IDLE, defined by the includer
//before this header; the default uses the Cpu callbacks.

#ifndef CPU_OPS_READ
#define CPU_OPS_READ(c, a) ((c)->read((c)->ctx, (a)))
#define CPU_OPS_WRITE(c, a, v) ((c)->write((c)->ctx, (a), (v)))
#define CPU_OPS_IDLE(c) ((c)->idle((c)->ctx))
#endif
//Opcode and operand fetch from PB:PC. Same bus effect as CPU_OPS_READ.
#ifndef CPU_OPS_FETCH
#define CPU_OPS_FETCH(c, a) CPU_OPS_READ(c, a)
#endif

enum {
  CPU_AM_IMM, CPU_AM_ABS, CPU_AM_ABSX, CPU_AM_ABSY, CPU_AM_DP, CPU_AM_DPX, CPU_AM_DPY,
  CPU_AM_DPI, CPU_AM_DPIX, CPU_AM_DPIY, CPU_AM_DPIL, CPU_AM_DPILY, CPU_AM_ABSL, CPU_AM_ABSLX,
  CPU_AM_SR, CPU_AM_SRIY,
};
enum { CPU_ACC_R, CPU_ACC_W, CPU_ACC_M };
enum { CPU_K_ASL, CPU_K_LSR, CPU_K_ROL, CPU_K_ROR, CPU_K_INC, CPU_K_DEC, CPU_K_TSB, CPU_K_TRB };

//wrap16: second byte of a 16-bit access wraps within the low 16 bits (bank 0).
typedef struct { uint32_t a; bool wrap16; } CpuEa;

static inline uint8_t cpu_rd(Cpu *c, uint32_t a) { return CPU_OPS_READ(c, a & 0xffffff); }
static inline void cpu_wr(Cpu *c, uint32_t a, uint8_t v) { CPU_OPS_WRITE(c, a & 0xffffff, v); }
static inline void cpu_io(Cpu *c) { CPU_OPS_IDLE(c); }

static inline bool cpu_m8(const Cpu *c) { return c->p & CPU_M; }
static inline bool cpu_x8(const Cpu *c) { return c->p & CPU_X; }

static inline uint8_t cpu_f8(Cpu *c) { return CPU_OPS_FETCH(c, (uint32_t)c->pb << 16 | c->pc++); }
static inline uint16_t cpu_f16(Cpu *c) { uint16_t v = cpu_f8(c); return v | cpu_f8(c) << 8; }

static inline void cpu_setf(Cpu *c, uint8_t f, bool on) { if (on) c->p |= f; else c->p &= ~f; }
static inline void cpu_nz(Cpu *c, uint16_t v, bool w) {
  if (w) { cpu_setf(c, CPU_Z, v == 0); cpu_setf(c, CPU_N, v & 0x8000); }
  else { cpu_setf(c, CPU_Z, (v & 0xff) == 0); cpu_setf(c, CPU_N, v & 0x80); }
}

static inline void cpu_fixp(Cpu *c) {
  if (c->e) c->p |= CPU_M | CPU_X;
  if (c->p & CPU_X) { c->x &= 0xff; c->y &= 0xff; }
}
static inline void cpu_efix(Cpu *c) { if (c->e) c->s = 0x100 | (c->s & 0xff); }

static inline void cpu_push(Cpu *c, uint8_t v) {
  cpu_wr(c, c->s, v);
  if (c->e) c->s = 0x100 | ((c->s - 1) & 0xff); else c->s--;
}
static inline uint8_t cpu_pull(Cpu *c) {
  if (c->e) c->s = 0x100 | ((c->s + 1) & 0xff); else c->s++;
  return cpu_rd(c, c->s);
}
//Non-wrapping variants for 65816-only stack instructions; S.h forced to 1 afterwards in emulation.
static inline void cpu_pushn(Cpu *c, uint8_t v) { cpu_wr(c, c->s, v); c->s--; }
static inline uint8_t cpu_pulln(Cpu *c) { c->s++; return cpu_rd(c, c->s); }

//Emulation mode with DL=0 wraps within the direct page.
static inline uint32_t cpu_dpa(const Cpu *c, uint32_t off) {
  if (c->e && !(c->d & 0xff)) return c->d | (off & 0xff);
  return (c->d + off) & 0xffff;
}
static inline uint32_t cpu_dpan(const Cpu *c, uint32_t off) { return (c->d + off) & 0xffff; }

static inline uint32_t cpu_nxt(CpuEa e) {
  return e.wrap16 ? (e.a & 0xff0000) | ((e.a + 1) & 0xffff) : (e.a + 1) & 0xffffff;
}

static inline void cpu_dlio(Cpu *c) { if (c->d & 0xff) cpu_io(c); }

//Extra cycle for indexed reads only on page cross or 16-bit index; writes/RMW always.
static inline void cpu_idxio(Cpu *c, uint16_t base, uint16_t idx, int acc) {
  if (acc != CPU_ACC_R || !cpu_x8(c) || ((base + idx) & 0xff00) != (base & 0xff00)) cpu_io(c);
}

static inline CpuEa cpu_ea(Cpu *c, int mode, int acc) {
  CpuEa e = { 0, false };
  uint32_t o, p;
  switch (mode) {
  case CPU_AM_ABS: e.a = (uint32_t)c->db << 16 | cpu_f16(c); break;
  case CPU_AM_ABSX: case CPU_AM_ABSY: {
    uint16_t w = cpu_f16(c), i = mode == CPU_AM_ABSX ? c->x : c->y;
    cpu_idxio(c, w, i, acc);
    e.a = (((uint32_t)c->db << 16 | w) + i) & 0xffffff;
    break;
  }
  case CPU_AM_DP: o = cpu_f8(c); cpu_dlio(c); e.a = cpu_dpa(c, o); e.wrap16 = true; break;
  case CPU_AM_DPX: case CPU_AM_DPY:
    o = cpu_f8(c); cpu_dlio(c); cpu_io(c);
    e.a = cpu_dpa(c, o + (mode == CPU_AM_DPX ? c->x : c->y)); e.wrap16 = true;
    break;
  case CPU_AM_DPI:
    o = cpu_f8(c); cpu_dlio(c);
    p = cpu_rd(c, cpu_dpa(c, o)); p |= cpu_rd(c, cpu_dpa(c, o + 1)) << 8;
    e.a = (uint32_t)c->db << 16 | p;
    break;
  case CPU_AM_DPIX:
    o = cpu_f8(c); cpu_dlio(c); cpu_io(c);
    o += c->x;
    p = cpu_rd(c, cpu_dpa(c, o)); p |= cpu_rd(c, cpu_dpa(c, o + 1)) << 8;
    e.a = (uint32_t)c->db << 16 | p;
    break;
  case CPU_AM_DPIY:
    o = cpu_f8(c); cpu_dlio(c);
    p = cpu_rd(c, cpu_dpa(c, o)); p |= cpu_rd(c, cpu_dpa(c, o + 1)) << 8;
    cpu_idxio(c, p, c->y, acc);
    e.a = (((uint32_t)c->db << 16 | p) + c->y) & 0xffffff;
    break;
  case CPU_AM_DPIL: case CPU_AM_DPILY:
    o = cpu_f8(c); cpu_dlio(c);
    p = cpu_rd(c, cpu_dpan(c, o)); p |= cpu_rd(c, cpu_dpan(c, o + 1)) << 8;
    p |= (uint32_t)cpu_rd(c, cpu_dpan(c, o + 2)) << 16;
    e.a = (p + (mode == CPU_AM_DPILY ? c->y : 0)) & 0xffffff;
    break;
  case CPU_AM_ABSL: case CPU_AM_ABSLX:
    p = cpu_f16(c); p |= (uint32_t)cpu_f8(c) << 16;
    e.a = (p + (mode == CPU_AM_ABSLX ? c->x : 0)) & 0xffffff;
    break;
  case CPU_AM_SR: o = cpu_f8(c); cpu_io(c); e.a = (c->s + o) & 0xffff; e.wrap16 = true; break;
  case CPU_AM_SRIY:
    o = cpu_f8(c); cpu_io(c);
    p = cpu_rd(c, (c->s + o) & 0xffff); p |= cpu_rd(c, (c->s + o + 1) & 0xffff) << 8;
    cpu_io(c);
    e.a = (((uint32_t)c->db << 16 | p) + c->y) & 0xffffff;
    break;
  }
  return e;
}

static inline uint16_t cpu_rdea(Cpu *c, CpuEa e, bool w) {
  uint16_t v = cpu_rd(c, e.a);
  if (w) v |= cpu_rd(c, cpu_nxt(e)) << 8;
  return v;
}
static inline void cpu_wrea(Cpu *c, CpuEa e, uint16_t v, bool w) {
  cpu_wr(c, e.a, v);
  if (w) cpu_wr(c, cpu_nxt(e), v >> 8);
}

static inline uint16_t cpu_operand(Cpu *c, int mode, bool w) {
  if (mode == CPU_AM_IMM) return w ? cpu_f16(c) : cpu_f8(c);
  return cpu_rdea(c, cpu_ea(c, mode, CPU_ACC_R), w);
}

static inline void cpu_seta(Cpu *c, uint16_t v) {
  if (cpu_m8(c)) c->a = (c->a & 0xff00) | (v & 0xff); else c->a = v;
}
static inline void cpu_setx(Cpu *c, uint16_t *r, uint16_t v) { *r = cpu_x8(c) ? (v & 0xff) : v; }

static inline void cpu_adc(Cpu *c, uint16_t data) {
  int cf = c->p & CPU_C, dec = c->p & CPU_D, r;
  if (cpu_m8(c)) {
    int a = c->a & 0xff, d = data & 0xff;
    if (!dec) r = a + d + cf;
    else {
      r = (a & 0x0f) + (d & 0x0f) + cf;
      if (r > 0x09) r += 0x06;
      cf = r > 0x0f;
      r = (a & 0xf0) + (d & 0xf0) + (cf << 4) + (r & 0x0f);
    }
    cpu_setf(c, CPU_V, ~(a ^ d) & (a ^ r) & 0x80);
    if (dec && r > 0x9f) r += 0x60;
    cpu_setf(c, CPU_C, r > 0xff);
    cpu_seta(c, r); cpu_nz(c, r, false);
  } else {
    int a = c->a, d = data;
    if (!dec) r = a + d + cf;
    else {
      r = (a & 0x000f) + (d & 0x000f) + cf;
      if (r > 0x0009) r += 0x0006;
      cf = r > 0x000f;
      r = (a & 0x00f0) + (d & 0x00f0) + (cf << 4) + (r & 0x000f);
      if (r > 0x009f) r += 0x0060;
      cf = r > 0x00ff;
      r = (a & 0x0f00) + (d & 0x0f00) + (cf << 8) + (r & 0x00ff);
      if (r > 0x09ff) r += 0x0600;
      cf = r > 0x0fff;
      r = (a & 0xf000) + (d & 0xf000) + (cf << 12) + (r & 0x0fff);
    }
    cpu_setf(c, CPU_V, ~(a ^ d) & (a ^ r) & 0x8000);
    if (dec && r > 0x9fff) r += 0x6000;
    cpu_setf(c, CPU_C, r > 0xffff);
    cpu_seta(c, r); cpu_nz(c, r, true);
  }
}

static inline void cpu_sbc(Cpu *c, uint16_t data) {
  int cf = c->p & CPU_C, dec = c->p & CPU_D, r;
  if (cpu_m8(c)) {
    int a = c->a & 0xff, d = ~data & 0xff;
    if (!dec) r = a + d + cf;
    else {
      r = (a & 0x0f) + (d & 0x0f) + cf;
      if (r <= 0x0f) r -= 0x06;
      cf = r > 0x0f;
      r = (a & 0xf0) + (d & 0xf0) + (cf << 4) + (r & 0x0f);
    }
    cpu_setf(c, CPU_V, ~(a ^ d) & (a ^ r) & 0x80);
    if (dec && r <= 0xff) r -= 0x60;
    cpu_setf(c, CPU_C, r > 0xff);
    cpu_seta(c, r & 0xff); cpu_nz(c, r & 0xff, false);
  } else {
    int a = c->a, d = ~data & 0xffff;
    if (!dec) r = a + d + cf;
    else {
      r = (a & 0x000f) + (d & 0x000f) + cf;
      if (r <= 0x000f) r -= 0x0006;
      cf = r > 0x000f;
      r = (a & 0x00f0) + (d & 0x00f0) + (cf << 4) + (r & 0x000f);
      if (r <= 0x00ff) r -= 0x0060;
      cf = r > 0x00ff;
      r = (a & 0x0f00) + (d & 0x0f00) + (cf << 8) + (r & 0x00ff);
      if (r <= 0x0fff) r -= 0x0600;
      cf = r > 0x0fff;
      r = (a & 0xf000) + (d & 0xf000) + (cf << 12) + (r & 0x0fff);
    }
    cpu_setf(c, CPU_V, ~(a ^ d) & (a ^ r) & 0x8000);
    if (dec && r <= 0xffff) r -= 0x6000;
    cpu_setf(c, CPU_C, r > 0xffff);
    cpu_seta(c, r & 0xffff); cpu_nz(c, r & 0xffff, true);
  }
}

static inline void cpu_cmp(Cpu *c, uint16_t reg, uint16_t v, bool w) {
  uint32_t mask = w ? 0xffff : 0xff;
  reg &= mask; v &= mask;
  cpu_setf(c, CPU_C, reg >= v);
  cpu_nz(c, (reg - v) & mask, w);
}

static inline void cpu_bit(Cpu *c, uint16_t v, bool w, bool imm) {
  uint16_t a = w ? c->a : (c->a & 0xff);
  cpu_setf(c, CPU_Z, (a & v) == 0);
  if (imm) return;
  if (w) { cpu_setf(c, CPU_N, v & 0x8000); cpu_setf(c, CPU_V, v & 0x4000); }
  else { cpu_setf(c, CPU_N, v & 0x80); cpu_setf(c, CPU_V, v & 0x40); }
}

static inline uint16_t cpu_alter(Cpu *c, int k, uint16_t v, bool w) {
  uint32_t mask = w ? 0xffff : 0xff, top = w ? 0x8000 : 0x80;
  uint16_t a = c->a & mask;
  v &= mask;
  switch (k) {
  case CPU_K_ASL: cpu_setf(c, CPU_C, v & top); v = (v << 1) & mask; break;
  case CPU_K_LSR: cpu_setf(c, CPU_C, v & 1); v >>= 1; break;
  case CPU_K_ROL: {
    bool ci = c->p & CPU_C;
    cpu_setf(c, CPU_C, v & top); v = ((v << 1) | ci) & mask; break;
  }
  case CPU_K_ROR: {
    bool ci = c->p & CPU_C;
    cpu_setf(c, CPU_C, v & 1); v = (v >> 1) | (ci ? top : 0); break;
  }
  case CPU_K_INC: v = (v + 1) & mask; break;
  case CPU_K_DEC: v = (v - 1) & mask; break;
  case CPU_K_TSB: cpu_setf(c, CPU_Z, (v & a) == 0); return v | a;
  case CPU_K_TRB: cpu_setf(c, CPU_Z, (v & a) == 0); return v & ~a & mask;
  }
  cpu_nz(c, v, w);
  return v;
}

static inline void cpu_rmw(Cpu *c, int mode, int k) {
  bool w = !cpu_m8(c);
  CpuEa e = cpu_ea(c, mode, CPU_ACC_M);
  uint16_t v = cpu_rdea(c, e, w);
  cpu_io(c);
  v = cpu_alter(c, k, v, w);
  if (w) cpu_wr(c, cpu_nxt(e), v >> 8);
  cpu_wr(c, e.a, v);
}

static inline void cpu_acc_op(Cpu *c, int k) {
  cpu_io(c);
  cpu_seta(c, cpu_alter(c, k, c->a, !cpu_m8(c)));
}

static inline void cpu_idx_op(Cpu *c, uint16_t *r, int d) {
  cpu_io(c);
  cpu_setx(c, r, *r + d);
  cpu_nz(c, *r, !cpu_x8(c));
}

static inline void cpu_xfer(Cpu *c, uint16_t *dst, uint16_t src, bool w) {
  cpu_io(c);
  if (w) *dst = src; else *dst = (*dst & 0xff00) | (src & 0xff);
  cpu_nz(c, *dst, w);
}

static inline void cpu_branch(Cpu *c, bool cond) {
  int8_t off = (int8_t)cpu_f8(c);
  if (!cond) return;
  uint16_t t = c->pc + off;
  cpu_io(c);
  if (c->e && (t & 0xff00) != (c->pc & 0xff00)) cpu_io(c);
  c->pc = t;
}

static inline void cpu_vector(Cpu *c, uint16_t vn, uint16_t ve, uint8_t pushed_p) {
  if (!c->e) cpu_push(c, c->pb);
  cpu_push(c, c->pc >> 8);
  cpu_push(c, c->pc);
  cpu_push(c, pushed_p);
  c->p |= CPU_I;
  c->p &= ~CPU_D;
  c->pb = 0;
  uint16_t v = c->e ? ve : vn;
  c->pc = cpu_rd(c, v) | cpu_rd(c, v + 1) << 8;
}

static inline void cpu_hw_interrupt(Cpu *c, uint16_t vn, uint16_t ve) {
  cpu_io(c); cpu_io(c);
  cpu_vector(c, vn, ve, c->e ? (c->p & ~0x10) : c->p);
}

static inline void cpu_pushw(Cpu *c, uint16_t v, bool w) {
  if (w) cpu_push(c, v >> 8);
  cpu_push(c, v);
}
static inline uint16_t cpu_pullw(Cpu *c, bool w) {
  uint16_t v = cpu_pull(c);
  if (w) v |= cpu_pull(c) << 8;
  return v;
}

static inline void cpu_blockmove(Cpu *c, int dir) {
  uint8_t dst = cpu_f8(c), src = cpu_f8(c);
  c->db = dst;
  cpu_wr(c, (uint32_t)dst << 16 | c->y, cpu_rd(c, (uint32_t)src << 16 | c->x));
  cpu_setx(c, &c->x, c->x + dir);
  cpu_setx(c, &c->y, c->y + dir);
  c->a--;
  cpu_io(c); cpu_io(c);
  if (c->a != 0xffff) c->pc -= 3;
}

static inline void cpu_group1(Cpu *c, uint8_t op, int mode) {
  bool w = !cpu_m8(c);
  int aaa = op >> 5;
  if (aaa == 4) { cpu_wrea(c, cpu_ea(c, mode, CPU_ACC_W), c->a, w); return; }
  uint16_t v = cpu_operand(c, mode, w);
  switch (aaa) {
  case 0: cpu_seta(c, c->a | v); cpu_nz(c, c->a, w); break;
  case 1: cpu_seta(c, c->a & v); cpu_nz(c, c->a, w); break;
  case 2: cpu_seta(c, c->a ^ v); cpu_nz(c, c->a, w); break;
  case 3: cpu_adc(c, v); break;
  case 5: cpu_seta(c, v); cpu_nz(c, c->a, w); break;
  case 6: cpu_cmp(c, c->a, v, w); break;
  case 7: cpu_sbc(c, v); break;
  }
}

static inline void cpu_ldx(Cpu *c, uint16_t *r, int mode) {
  bool w = !cpu_x8(c);
  cpu_setx(c, r, cpu_operand(c, mode, w));
  cpu_nz(c, *r, w);
}
static inline void cpu_stx(Cpu *c, uint16_t v, int mode, bool w) {
  cpu_wrea(c, cpu_ea(c, mode, CPU_ACC_W), v, w);
}
static inline void cpu_cpx(Cpu *c, uint16_t r, int mode) {
  bool w = !cpu_x8(c);
  cpu_cmp(c, r, cpu_operand(c, mode, w), w);
}

#define CPU_OP(h) static inline void cpu_op_##h(Cpu *c)

CPU_OP(00) { cpu_f8(c); cpu_vector(c, 0xffe6, 0xfffe, c->p); }
CPU_OP(01) { cpu_group1(c, 0x01, CPU_AM_DPIX); }
CPU_OP(02) { cpu_f8(c); cpu_vector(c, 0xffe4, 0xfff4, c->p); }
CPU_OP(03) { cpu_group1(c, 0x03, CPU_AM_SR); }
CPU_OP(04) { cpu_rmw(c, CPU_AM_DP, CPU_K_TSB); }
CPU_OP(05) { cpu_group1(c, 0x05, CPU_AM_DP); }
CPU_OP(06) { cpu_rmw(c, CPU_AM_DP, CPU_K_ASL); }
CPU_OP(07) { cpu_group1(c, 0x07, CPU_AM_DPIL); }
CPU_OP(08) { cpu_io(c); cpu_push(c, c->p); }
CPU_OP(09) { cpu_group1(c, 0x09, CPU_AM_IMM); }
CPU_OP(0A) { cpu_acc_op(c, CPU_K_ASL); }
CPU_OP(0B) { cpu_io(c); cpu_pushn(c, c->d >> 8); cpu_pushn(c, c->d); cpu_efix(c); }
CPU_OP(0C) { cpu_rmw(c, CPU_AM_ABS, CPU_K_TSB); }
CPU_OP(0D) { cpu_group1(c, 0x0d, CPU_AM_ABS); }
CPU_OP(0E) { cpu_rmw(c, CPU_AM_ABS, CPU_K_ASL); }
CPU_OP(0F) { cpu_group1(c, 0x0f, CPU_AM_ABSL); }
CPU_OP(10) { cpu_branch(c, !(c->p & CPU_N)); }
CPU_OP(11) { cpu_group1(c, 0x11, CPU_AM_DPIY); }
CPU_OP(12) { cpu_group1(c, 0x12, CPU_AM_DPI); }
CPU_OP(13) { cpu_group1(c, 0x13, CPU_AM_SRIY); }
CPU_OP(14) { cpu_rmw(c, CPU_AM_DP, CPU_K_TRB); }
CPU_OP(15) { cpu_group1(c, 0x15, CPU_AM_DPX); }
CPU_OP(16) { cpu_rmw(c, CPU_AM_DPX, CPU_K_ASL); }
CPU_OP(17) { cpu_group1(c, 0x17, CPU_AM_DPILY); }
CPU_OP(18) { cpu_io(c); c->p &= ~CPU_C; }
CPU_OP(19) { cpu_group1(c, 0x19, CPU_AM_ABSY); }
CPU_OP(1A) { cpu_acc_op(c, CPU_K_INC); }
CPU_OP(1B) { cpu_io(c); c->s = c->e ? 0x100 | (c->a & 0xff) : c->a; }
CPU_OP(1C) { cpu_rmw(c, CPU_AM_ABS, CPU_K_TRB); }
CPU_OP(1D) { cpu_group1(c, 0x1d, CPU_AM_ABSX); }
CPU_OP(1E) { cpu_rmw(c, CPU_AM_ABSX, CPU_K_ASL); }
CPU_OP(1F) { cpu_group1(c, 0x1f, CPU_AM_ABSLX); }
CPU_OP(20) {
  uint16_t t;
  t = cpu_f16(c); cpu_io(c);
  c->pc--; cpu_push(c, c->pc >> 8); cpu_push(c, c->pc);
  c->pc = t;
}
CPU_OP(21) { cpu_group1(c, 0x21, CPU_AM_DPIX); }
CPU_OP(22) {
  uint16_t t;
  t = cpu_f16(c);
  cpu_pushn(c, c->pb);
  cpu_io(c);
  uint8_t b = cpu_f8(c);
  c->pc--; cpu_pushn(c, c->pc >> 8); cpu_pushn(c, c->pc);
  c->pb = b; c->pc = t;
  cpu_efix(c);
}
CPU_OP(23) { cpu_group1(c, 0x23, CPU_AM_SR); }
CPU_OP(24) { bool mw = !cpu_m8(c); cpu_bit(c, cpu_operand(c, CPU_AM_DP, mw), mw, false); }
CPU_OP(25) { cpu_group1(c, 0x25, CPU_AM_DP); }
CPU_OP(26) { cpu_rmw(c, CPU_AM_DP, CPU_K_ROL); }
CPU_OP(27) { cpu_group1(c, 0x27, CPU_AM_DPIL); }
CPU_OP(28) { cpu_io(c); cpu_io(c); c->p = cpu_pull(c); cpu_fixp(c); }
CPU_OP(29) { cpu_group1(c, 0x29, CPU_AM_IMM); }
CPU_OP(2A) { cpu_acc_op(c, CPU_K_ROL); }
CPU_OP(2B) {
  cpu_io(c); cpu_io(c);
  c->d = cpu_pulln(c); c->d |= cpu_pulln(c) << 8;
  cpu_nz(c, c->d, true); cpu_efix(c);
}
CPU_OP(2C) { bool mw = !cpu_m8(c); cpu_bit(c, cpu_operand(c, CPU_AM_ABS, mw), mw, false); }
CPU_OP(2D) { cpu_group1(c, 0x2d, CPU_AM_ABS); }
CPU_OP(2E) { cpu_rmw(c, CPU_AM_ABS, CPU_K_ROL); }
CPU_OP(2F) { cpu_group1(c, 0x2f, CPU_AM_ABSL); }
CPU_OP(30) { cpu_branch(c, c->p & CPU_N); }
CPU_OP(31) { cpu_group1(c, 0x31, CPU_AM_DPIY); }
CPU_OP(32) { cpu_group1(c, 0x32, CPU_AM_DPI); }
CPU_OP(33) { cpu_group1(c, 0x33, CPU_AM_SRIY); }
CPU_OP(34) { bool mw = !cpu_m8(c); cpu_bit(c, cpu_operand(c, CPU_AM_DPX, mw), mw, false); }
CPU_OP(35) { cpu_group1(c, 0x35, CPU_AM_DPX); }
CPU_OP(36) { cpu_rmw(c, CPU_AM_DPX, CPU_K_ROL); }
CPU_OP(37) { cpu_group1(c, 0x37, CPU_AM_DPILY); }
CPU_OP(38) { cpu_io(c); c->p |= CPU_C; }
CPU_OP(39) { cpu_group1(c, 0x39, CPU_AM_ABSY); }
CPU_OP(3A) { cpu_acc_op(c, CPU_K_DEC); }
CPU_OP(3B) { cpu_io(c); c->a = c->s; cpu_nz(c, c->a, true); }
CPU_OP(3C) { bool mw = !cpu_m8(c); cpu_bit(c, cpu_operand(c, CPU_AM_ABSX, mw), mw, false); }
CPU_OP(3D) { cpu_group1(c, 0x3d, CPU_AM_ABSX); }
CPU_OP(3E) { cpu_rmw(c, CPU_AM_ABSX, CPU_K_ROL); }
CPU_OP(3F) { cpu_group1(c, 0x3f, CPU_AM_ABSLX); }
CPU_OP(40) {
  cpu_io(c); cpu_io(c);
  c->p = cpu_pull(c); cpu_fixp(c);
  c->pc = cpu_pull(c); c->pc |= cpu_pull(c) << 8;
  if (!c->e) c->pb = cpu_pull(c);
}
CPU_OP(41) { cpu_group1(c, 0x41, CPU_AM_DPIX); }
CPU_OP(42) { cpu_f8(c); }
CPU_OP(43) { cpu_group1(c, 0x43, CPU_AM_SR); }
CPU_OP(44) { cpu_blockmove(c, -1); }
CPU_OP(45) { cpu_group1(c, 0x45, CPU_AM_DP); }
CPU_OP(46) { cpu_rmw(c, CPU_AM_DP, CPU_K_LSR); }
CPU_OP(47) { cpu_group1(c, 0x47, CPU_AM_DPIL); }
CPU_OP(48) { bool mw = !cpu_m8(c); cpu_io(c); cpu_pushw(c, c->a, mw); }
CPU_OP(49) { cpu_group1(c, 0x49, CPU_AM_IMM); }
CPU_OP(4A) { cpu_acc_op(c, CPU_K_LSR); }
CPU_OP(4B) { cpu_io(c); cpu_push(c, c->pb); }
CPU_OP(4C) { c->pc = cpu_f16(c); }
CPU_OP(4D) { cpu_group1(c, 0x4d, CPU_AM_ABS); }
CPU_OP(4E) { cpu_rmw(c, CPU_AM_ABS, CPU_K_LSR); }
CPU_OP(4F) { cpu_group1(c, 0x4f, CPU_AM_ABSL); }
CPU_OP(50) { cpu_branch(c, !(c->p & CPU_V)); }
CPU_OP(51) { cpu_group1(c, 0x51, CPU_AM_DPIY); }
CPU_OP(52) { cpu_group1(c, 0x52, CPU_AM_DPI); }
CPU_OP(53) { cpu_group1(c, 0x53, CPU_AM_SRIY); }
CPU_OP(54) { cpu_blockmove(c, 1); }
CPU_OP(55) { cpu_group1(c, 0x55, CPU_AM_DPX); }
CPU_OP(56) { cpu_rmw(c, CPU_AM_DPX, CPU_K_LSR); }
CPU_OP(57) { cpu_group1(c, 0x57, CPU_AM_DPILY); }
CPU_OP(58) { cpu_io(c); c->p &= ~CPU_I; }
CPU_OP(59) { cpu_group1(c, 0x59, CPU_AM_ABSY); }
CPU_OP(5A) { bool xw = !cpu_x8(c); cpu_io(c); cpu_pushw(c, c->y, xw); }
CPU_OP(5B) { cpu_io(c); c->d = c->a; cpu_nz(c, c->d, true); }
CPU_OP(5C) { uint16_t t; t = cpu_f16(c); c->pb = cpu_f8(c); c->pc = t; }
CPU_OP(5D) { cpu_group1(c, 0x5d, CPU_AM_ABSX); }
CPU_OP(5E) { cpu_rmw(c, CPU_AM_ABSX, CPU_K_LSR); }
CPU_OP(5F) { cpu_group1(c, 0x5f, CPU_AM_ABSLX); }
CPU_OP(60) {
  cpu_io(c); cpu_io(c);
  c->pc = cpu_pull(c); c->pc |= cpu_pull(c) << 8;
  cpu_io(c);
  c->pc++;
}
CPU_OP(61) { cpu_group1(c, 0x61, CPU_AM_DPIX); }
CPU_OP(62) {
  uint16_t t;
  t = cpu_f16(c); cpu_io(c);
  t += c->pc;
  cpu_pushn(c, t >> 8); cpu_pushn(c, t); cpu_efix(c);
}
CPU_OP(63) { cpu_group1(c, 0x63, CPU_AM_SR); }
CPU_OP(64) { bool mw = !cpu_m8(c); cpu_stx(c, 0, CPU_AM_DP, mw); }
CPU_OP(65) { cpu_group1(c, 0x65, CPU_AM_DP); }
CPU_OP(66) { cpu_rmw(c, CPU_AM_DP, CPU_K_ROR); }
CPU_OP(67) { cpu_group1(c, 0x67, CPU_AM_DPIL); }
CPU_OP(68) { bool mw = !cpu_m8(c); cpu_io(c); cpu_io(c); cpu_seta(c, cpu_pullw(c, mw)); cpu_nz(c, c->a, mw); }
CPU_OP(69) { cpu_group1(c, 0x69, CPU_AM_IMM); }
CPU_OP(6A) { cpu_acc_op(c, CPU_K_ROR); }
CPU_OP(6B) {
  cpu_io(c); cpu_io(c);
  c->pc = cpu_pulln(c); c->pc |= cpu_pulln(c) << 8;
  c->pb = cpu_pulln(c);
  c->pc++;
  cpu_efix(c);
}
CPU_OP(6C) {
  uint16_t v = cpu_f16(c);
  c->pc = cpu_rd(c, v) | cpu_rd(c, (uint16_t)(v + 1)) << 8;
}
CPU_OP(6D) { cpu_group1(c, 0x6d, CPU_AM_ABS); }
CPU_OP(6E) { cpu_rmw(c, CPU_AM_ABS, CPU_K_ROR); }
CPU_OP(6F) { cpu_group1(c, 0x6f, CPU_AM_ABSL); }
CPU_OP(70) { cpu_branch(c, c->p & CPU_V); }
CPU_OP(71) { cpu_group1(c, 0x71, CPU_AM_DPIY); }
CPU_OP(72) { cpu_group1(c, 0x72, CPU_AM_DPI); }
CPU_OP(73) { cpu_group1(c, 0x73, CPU_AM_SRIY); }
CPU_OP(74) { bool mw = !cpu_m8(c); cpu_stx(c, 0, CPU_AM_DPX, mw); }
CPU_OP(75) { cpu_group1(c, 0x75, CPU_AM_DPX); }
CPU_OP(76) { cpu_rmw(c, CPU_AM_DPX, CPU_K_ROR); }
CPU_OP(77) { cpu_group1(c, 0x77, CPU_AM_DPILY); }
CPU_OP(78) { cpu_io(c); c->p |= CPU_I; }
CPU_OP(79) { cpu_group1(c, 0x79, CPU_AM_ABSY); }
CPU_OP(7A) { bool xw = !cpu_x8(c); cpu_io(c); cpu_io(c); c->y = cpu_pullw(c, xw); cpu_nz(c, c->y, xw); }
CPU_OP(7B) { cpu_io(c); c->a = c->d; cpu_nz(c, c->a, true); }
CPU_OP(7C) {
  uint16_t v = cpu_f16(c) + c->x;
  uint32_t b = (uint32_t)c->pb << 16;
  cpu_io(c);
  c->pc = cpu_rd(c, b | v) | cpu_rd(c, b | (uint16_t)(v + 1)) << 8;
}
CPU_OP(7D) { cpu_group1(c, 0x7d, CPU_AM_ABSX); }
CPU_OP(7E) { cpu_rmw(c, CPU_AM_ABSX, CPU_K_ROR); }
CPU_OP(7F) { cpu_group1(c, 0x7f, CPU_AM_ABSLX); }
CPU_OP(80) { cpu_branch(c, true); }
CPU_OP(81) { cpu_group1(c, 0x81, CPU_AM_DPIX); }
CPU_OP(82) { uint16_t t; t = cpu_f16(c); cpu_io(c); c->pc += t; }
CPU_OP(83) { cpu_group1(c, 0x83, CPU_AM_SR); }
CPU_OP(84) { bool xw = !cpu_x8(c); cpu_stx(c, c->y, CPU_AM_DP, xw); }
CPU_OP(85) { cpu_group1(c, 0x85, CPU_AM_DP); }
CPU_OP(86) { bool xw = !cpu_x8(c); cpu_stx(c, c->x, CPU_AM_DP, xw); }
CPU_OP(87) { cpu_group1(c, 0x87, CPU_AM_DPIL); }
CPU_OP(88) { cpu_idx_op(c, &c->y, -1); }
CPU_OP(89) { bool mw = !cpu_m8(c); cpu_bit(c, cpu_operand(c, CPU_AM_IMM, mw), mw, true); }
CPU_OP(8A) { bool mw = !cpu_m8(c); cpu_xfer(c, &c->a, c->x, mw); }
CPU_OP(8B) { cpu_io(c); cpu_push(c, c->db); }
CPU_OP(8C) { bool xw = !cpu_x8(c); cpu_stx(c, c->y, CPU_AM_ABS, xw); }
CPU_OP(8D) { cpu_group1(c, 0x8d, CPU_AM_ABS); }
CPU_OP(8E) { bool xw = !cpu_x8(c); cpu_stx(c, c->x, CPU_AM_ABS, xw); }
CPU_OP(8F) { cpu_group1(c, 0x8f, CPU_AM_ABSL); }
CPU_OP(90) { cpu_branch(c, !(c->p & CPU_C)); }
CPU_OP(91) { cpu_group1(c, 0x91, CPU_AM_DPIY); }
CPU_OP(92) { cpu_group1(c, 0x92, CPU_AM_DPI); }
CPU_OP(93) { cpu_group1(c, 0x93, CPU_AM_SRIY); }
CPU_OP(94) { bool xw = !cpu_x8(c); cpu_stx(c, c->y, CPU_AM_DPX, xw); }
CPU_OP(95) { cpu_group1(c, 0x95, CPU_AM_DPX); }
CPU_OP(96) { bool xw = !cpu_x8(c); cpu_stx(c, c->x, CPU_AM_DPY, xw); }
CPU_OP(97) { cpu_group1(c, 0x97, CPU_AM_DPILY); }
CPU_OP(98) { bool mw = !cpu_m8(c); cpu_xfer(c, &c->a, c->y, mw); }
CPU_OP(99) { cpu_group1(c, 0x99, CPU_AM_ABSY); }
CPU_OP(9A) { cpu_io(c); c->s = c->e ? 0x100 | (c->x & 0xff) : c->x; }
CPU_OP(9B) { bool xw = !cpu_x8(c); cpu_io(c); cpu_setx(c, &c->y, c->x); cpu_nz(c, c->y, xw); }
CPU_OP(9C) { bool mw = !cpu_m8(c); cpu_stx(c, 0, CPU_AM_ABS, mw); }
CPU_OP(9D) { cpu_group1(c, 0x9d, CPU_AM_ABSX); }
CPU_OP(9E) { bool mw = !cpu_m8(c); cpu_stx(c, 0, CPU_AM_ABSX, mw); }
CPU_OP(9F) { cpu_group1(c, 0x9f, CPU_AM_ABSLX); }
CPU_OP(A0) { cpu_ldx(c, &c->y, CPU_AM_IMM); }
CPU_OP(A1) { cpu_group1(c, 0xa1, CPU_AM_DPIX); }
CPU_OP(A2) { cpu_ldx(c, &c->x, CPU_AM_IMM); }
CPU_OP(A3) { cpu_group1(c, 0xa3, CPU_AM_SR); }
CPU_OP(A4) { cpu_ldx(c, &c->y, CPU_AM_DP); }
CPU_OP(A5) { cpu_group1(c, 0xa5, CPU_AM_DP); }
CPU_OP(A6) { cpu_ldx(c, &c->x, CPU_AM_DP); }
CPU_OP(A7) { cpu_group1(c, 0xa7, CPU_AM_DPIL); }
CPU_OP(A8) { bool xw = !cpu_x8(c); cpu_io(c); cpu_setx(c, &c->y, c->a); cpu_nz(c, c->y, xw); }
CPU_OP(A9) { cpu_group1(c, 0xa9, CPU_AM_IMM); }
CPU_OP(AA) { bool xw = !cpu_x8(c); cpu_io(c); cpu_setx(c, &c->x, c->a); cpu_nz(c, c->x, xw); }
CPU_OP(AB) { cpu_io(c); cpu_io(c); c->db = cpu_pulln(c); cpu_nz(c, c->db, false); cpu_efix(c); }
CPU_OP(AC) { cpu_ldx(c, &c->y, CPU_AM_ABS); }
CPU_OP(AD) { cpu_group1(c, 0xad, CPU_AM_ABS); }
CPU_OP(AE) { cpu_ldx(c, &c->x, CPU_AM_ABS); }
CPU_OP(AF) { cpu_group1(c, 0xaf, CPU_AM_ABSL); }
CPU_OP(B0) { cpu_branch(c, c->p & CPU_C); }
CPU_OP(B1) { cpu_group1(c, 0xb1, CPU_AM_DPIY); }
CPU_OP(B2) { cpu_group1(c, 0xb2, CPU_AM_DPI); }
CPU_OP(B3) { cpu_group1(c, 0xb3, CPU_AM_SRIY); }
CPU_OP(B4) { cpu_ldx(c, &c->y, CPU_AM_DPX); }
CPU_OP(B5) { cpu_group1(c, 0xb5, CPU_AM_DPX); }
CPU_OP(B6) { cpu_ldx(c, &c->x, CPU_AM_DPY); }
CPU_OP(B7) { cpu_group1(c, 0xb7, CPU_AM_DPILY); }
CPU_OP(B8) { cpu_io(c); c->p &= ~CPU_V; }
CPU_OP(B9) { cpu_group1(c, 0xb9, CPU_AM_ABSY); }
CPU_OP(BA) { bool xw = !cpu_x8(c); cpu_io(c); cpu_setx(c, &c->x, c->s); cpu_nz(c, c->x, xw); }
CPU_OP(BB) { bool xw = !cpu_x8(c); cpu_io(c); cpu_setx(c, &c->x, c->y); cpu_nz(c, c->x, xw); }
CPU_OP(BC) { cpu_ldx(c, &c->y, CPU_AM_ABSX); }
CPU_OP(BD) { cpu_group1(c, 0xbd, CPU_AM_ABSX); }
CPU_OP(BE) { cpu_ldx(c, &c->x, CPU_AM_ABSY); }
CPU_OP(BF) { cpu_group1(c, 0xbf, CPU_AM_ABSLX); }
CPU_OP(C0) { cpu_cpx(c, c->y, CPU_AM_IMM); }
CPU_OP(C1) { cpu_group1(c, 0xc1, CPU_AM_DPIX); }
CPU_OP(C2) { uint16_t t; t = cpu_f8(c); cpu_io(c); c->p &= ~t; cpu_fixp(c); }
CPU_OP(C3) { cpu_group1(c, 0xc3, CPU_AM_SR); }
CPU_OP(C4) { cpu_cpx(c, c->y, CPU_AM_DP); }
CPU_OP(C5) { cpu_group1(c, 0xc5, CPU_AM_DP); }
CPU_OP(C6) { cpu_rmw(c, CPU_AM_DP, CPU_K_DEC); }
CPU_OP(C7) { cpu_group1(c, 0xc7, CPU_AM_DPIL); }
CPU_OP(C8) { cpu_idx_op(c, &c->y, 1); }
CPU_OP(C9) { cpu_group1(c, 0xc9, CPU_AM_IMM); }
CPU_OP(CA) { cpu_idx_op(c, &c->x, -1); }
CPU_OP(CB) { cpu_io(c); cpu_io(c); c->waiting = true; }
CPU_OP(CC) { cpu_cpx(c, c->y, CPU_AM_ABS); }
CPU_OP(CD) { cpu_group1(c, 0xcd, CPU_AM_ABS); }
CPU_OP(CE) { cpu_rmw(c, CPU_AM_ABS, CPU_K_DEC); }
CPU_OP(CF) { cpu_group1(c, 0xcf, CPU_AM_ABSL); }
CPU_OP(D0) { cpu_branch(c, !(c->p & CPU_Z)); }
CPU_OP(D1) { cpu_group1(c, 0xd1, CPU_AM_DPIY); }
CPU_OP(D2) { cpu_group1(c, 0xd2, CPU_AM_DPI); }
CPU_OP(D3) { cpu_group1(c, 0xd3, CPU_AM_SRIY); }
CPU_OP(D4) {
  uint16_t t;
  uint8_t o = cpu_f8(c);
  cpu_dlio(c);
  t = cpu_rd(c, cpu_dpan(c, o)) | cpu_rd(c, cpu_dpan(c, o + 1)) << 8;
  cpu_pushn(c, t >> 8); cpu_pushn(c, t); cpu_efix(c);
}
CPU_OP(D5) { cpu_group1(c, 0xd5, CPU_AM_DPX); }
CPU_OP(D6) { cpu_rmw(c, CPU_AM_DPX, CPU_K_DEC); }
CPU_OP(D7) { cpu_group1(c, 0xd7, CPU_AM_DPILY); }
CPU_OP(D8) { cpu_io(c); c->p &= ~CPU_D; }
CPU_OP(D9) { cpu_group1(c, 0xd9, CPU_AM_ABSY); }
CPU_OP(DA) { bool xw = !cpu_x8(c); cpu_io(c); cpu_pushw(c, c->x, xw); }
CPU_OP(DB) { cpu_io(c); cpu_io(c); c->stopped = true; }
CPU_OP(DC) {
  uint16_t v = cpu_f16(c);
  uint16_t lo = cpu_rd(c, v) | cpu_rd(c, (uint16_t)(v + 1)) << 8;
  c->pb = cpu_rd(c, (uint16_t)(v + 2));
  c->pc = lo;
}
CPU_OP(DD) { cpu_group1(c, 0xdd, CPU_AM_ABSX); }
CPU_OP(DE) { cpu_rmw(c, CPU_AM_ABSX, CPU_K_DEC); }
CPU_OP(DF) { cpu_group1(c, 0xdf, CPU_AM_ABSLX); }
CPU_OP(E0) { cpu_cpx(c, c->x, CPU_AM_IMM); }
CPU_OP(E1) { cpu_group1(c, 0xe1, CPU_AM_DPIX); }
CPU_OP(E2) { uint16_t t; t = cpu_f8(c); cpu_io(c); c->p |= t; cpu_fixp(c); }
CPU_OP(E3) { cpu_group1(c, 0xe3, CPU_AM_SR); }
CPU_OP(E4) { cpu_cpx(c, c->x, CPU_AM_DP); }
CPU_OP(E5) { cpu_group1(c, 0xe5, CPU_AM_DP); }
CPU_OP(E6) { cpu_rmw(c, CPU_AM_DP, CPU_K_INC); }
CPU_OP(E7) { cpu_group1(c, 0xe7, CPU_AM_DPIL); }
CPU_OP(E8) { cpu_idx_op(c, &c->x, 1); }
CPU_OP(E9) { cpu_group1(c, 0xe9, CPU_AM_IMM); }
CPU_OP(EA) { cpu_io(c); }
CPU_OP(EB) {
  cpu_io(c); cpu_io(c);
  c->a = (uint16_t)(c->a >> 8 | c->a << 8);
  cpu_nz(c, c->a, false);
}
CPU_OP(EC) { cpu_cpx(c, c->x, CPU_AM_ABS); }
CPU_OP(ED) { cpu_group1(c, 0xed, CPU_AM_ABS); }
CPU_OP(EE) { cpu_rmw(c, CPU_AM_ABS, CPU_K_INC); }
CPU_OP(EF) { cpu_group1(c, 0xef, CPU_AM_ABSL); }
CPU_OP(F0) { cpu_branch(c, c->p & CPU_Z); }
CPU_OP(F1) { cpu_group1(c, 0xf1, CPU_AM_DPIY); }
CPU_OP(F2) { cpu_group1(c, 0xf2, CPU_AM_DPI); }
CPU_OP(F3) { cpu_group1(c, 0xf3, CPU_AM_SRIY); }
CPU_OP(F4) { uint16_t t; t = cpu_f16(c); cpu_pushn(c, t >> 8); cpu_pushn(c, t); cpu_efix(c); }
CPU_OP(F5) { cpu_group1(c, 0xf5, CPU_AM_DPX); }
CPU_OP(F6) { cpu_rmw(c, CPU_AM_DPX, CPU_K_INC); }
CPU_OP(F7) { cpu_group1(c, 0xf7, CPU_AM_DPILY); }
CPU_OP(F8) { cpu_io(c); c->p |= CPU_D; }
CPU_OP(F9) { cpu_group1(c, 0xf9, CPU_AM_ABSY); }
CPU_OP(FA) { bool xw = !cpu_x8(c); cpu_io(c); cpu_io(c); c->x = cpu_pullw(c, xw); cpu_nz(c, c->x, xw); }
CPU_OP(FB) {
  cpu_io(c);
  bool oc = c->p & CPU_C;
  cpu_setf(c, CPU_C, c->e);
  c->e = oc;
  if (c->e) { c->p |= CPU_M | CPU_X; cpu_efix(c); }
  cpu_fixp(c);
}
CPU_OP(FC) {
  uint16_t v = cpu_f8(c);
  cpu_pushn(c, c->pc >> 8); cpu_pushn(c, c->pc);
  v |= cpu_f8(c) << 8;
  cpu_io(c);
  v += c->x;
  uint32_t b = (uint32_t)c->pb << 16;
  c->pc = cpu_rd(c, b | v) | cpu_rd(c, b | (uint16_t)(v + 1)) << 8;
  cpu_efix(c);
}
CPU_OP(FD) { cpu_group1(c, 0xfd, CPU_AM_ABSX); }
CPU_OP(FE) { cpu_rmw(c, CPU_AM_ABSX, CPU_K_INC); }
CPU_OP(FF) { cpu_group1(c, 0xff, CPU_AM_ABSLX); }

static inline void cpu_exec(Cpu *c, uint8_t op) {
  switch (op) {
  case 0x00: cpu_op_00(c); break;
  case 0x01: cpu_op_01(c); break;
  case 0x02: cpu_op_02(c); break;
  case 0x03: cpu_op_03(c); break;
  case 0x04: cpu_op_04(c); break;
  case 0x05: cpu_op_05(c); break;
  case 0x06: cpu_op_06(c); break;
  case 0x07: cpu_op_07(c); break;
  case 0x08: cpu_op_08(c); break;
  case 0x09: cpu_op_09(c); break;
  case 0x0a: cpu_op_0A(c); break;
  case 0x0b: cpu_op_0B(c); break;
  case 0x0c: cpu_op_0C(c); break;
  case 0x0d: cpu_op_0D(c); break;
  case 0x0e: cpu_op_0E(c); break;
  case 0x0f: cpu_op_0F(c); break;
  case 0x10: cpu_op_10(c); break;
  case 0x11: cpu_op_11(c); break;
  case 0x12: cpu_op_12(c); break;
  case 0x13: cpu_op_13(c); break;
  case 0x14: cpu_op_14(c); break;
  case 0x15: cpu_op_15(c); break;
  case 0x16: cpu_op_16(c); break;
  case 0x17: cpu_op_17(c); break;
  case 0x18: cpu_op_18(c); break;
  case 0x19: cpu_op_19(c); break;
  case 0x1a: cpu_op_1A(c); break;
  case 0x1b: cpu_op_1B(c); break;
  case 0x1c: cpu_op_1C(c); break;
  case 0x1d: cpu_op_1D(c); break;
  case 0x1e: cpu_op_1E(c); break;
  case 0x1f: cpu_op_1F(c); break;
  case 0x20: cpu_op_20(c); break;
  case 0x21: cpu_op_21(c); break;
  case 0x22: cpu_op_22(c); break;
  case 0x23: cpu_op_23(c); break;
  case 0x24: cpu_op_24(c); break;
  case 0x25: cpu_op_25(c); break;
  case 0x26: cpu_op_26(c); break;
  case 0x27: cpu_op_27(c); break;
  case 0x28: cpu_op_28(c); break;
  case 0x29: cpu_op_29(c); break;
  case 0x2a: cpu_op_2A(c); break;
  case 0x2b: cpu_op_2B(c); break;
  case 0x2c: cpu_op_2C(c); break;
  case 0x2d: cpu_op_2D(c); break;
  case 0x2e: cpu_op_2E(c); break;
  case 0x2f: cpu_op_2F(c); break;
  case 0x30: cpu_op_30(c); break;
  case 0x31: cpu_op_31(c); break;
  case 0x32: cpu_op_32(c); break;
  case 0x33: cpu_op_33(c); break;
  case 0x34: cpu_op_34(c); break;
  case 0x35: cpu_op_35(c); break;
  case 0x36: cpu_op_36(c); break;
  case 0x37: cpu_op_37(c); break;
  case 0x38: cpu_op_38(c); break;
  case 0x39: cpu_op_39(c); break;
  case 0x3a: cpu_op_3A(c); break;
  case 0x3b: cpu_op_3B(c); break;
  case 0x3c: cpu_op_3C(c); break;
  case 0x3d: cpu_op_3D(c); break;
  case 0x3e: cpu_op_3E(c); break;
  case 0x3f: cpu_op_3F(c); break;
  case 0x40: cpu_op_40(c); break;
  case 0x41: cpu_op_41(c); break;
  case 0x42: cpu_op_42(c); break;
  case 0x43: cpu_op_43(c); break;
  case 0x44: cpu_op_44(c); break;
  case 0x45: cpu_op_45(c); break;
  case 0x46: cpu_op_46(c); break;
  case 0x47: cpu_op_47(c); break;
  case 0x48: cpu_op_48(c); break;
  case 0x49: cpu_op_49(c); break;
  case 0x4a: cpu_op_4A(c); break;
  case 0x4b: cpu_op_4B(c); break;
  case 0x4c: cpu_op_4C(c); break;
  case 0x4d: cpu_op_4D(c); break;
  case 0x4e: cpu_op_4E(c); break;
  case 0x4f: cpu_op_4F(c); break;
  case 0x50: cpu_op_50(c); break;
  case 0x51: cpu_op_51(c); break;
  case 0x52: cpu_op_52(c); break;
  case 0x53: cpu_op_53(c); break;
  case 0x54: cpu_op_54(c); break;
  case 0x55: cpu_op_55(c); break;
  case 0x56: cpu_op_56(c); break;
  case 0x57: cpu_op_57(c); break;
  case 0x58: cpu_op_58(c); break;
  case 0x59: cpu_op_59(c); break;
  case 0x5a: cpu_op_5A(c); break;
  case 0x5b: cpu_op_5B(c); break;
  case 0x5c: cpu_op_5C(c); break;
  case 0x5d: cpu_op_5D(c); break;
  case 0x5e: cpu_op_5E(c); break;
  case 0x5f: cpu_op_5F(c); break;
  case 0x60: cpu_op_60(c); break;
  case 0x61: cpu_op_61(c); break;
  case 0x62: cpu_op_62(c); break;
  case 0x63: cpu_op_63(c); break;
  case 0x64: cpu_op_64(c); break;
  case 0x65: cpu_op_65(c); break;
  case 0x66: cpu_op_66(c); break;
  case 0x67: cpu_op_67(c); break;
  case 0x68: cpu_op_68(c); break;
  case 0x69: cpu_op_69(c); break;
  case 0x6a: cpu_op_6A(c); break;
  case 0x6b: cpu_op_6B(c); break;
  case 0x6c: cpu_op_6C(c); break;
  case 0x6d: cpu_op_6D(c); break;
  case 0x6e: cpu_op_6E(c); break;
  case 0x6f: cpu_op_6F(c); break;
  case 0x70: cpu_op_70(c); break;
  case 0x71: cpu_op_71(c); break;
  case 0x72: cpu_op_72(c); break;
  case 0x73: cpu_op_73(c); break;
  case 0x74: cpu_op_74(c); break;
  case 0x75: cpu_op_75(c); break;
  case 0x76: cpu_op_76(c); break;
  case 0x77: cpu_op_77(c); break;
  case 0x78: cpu_op_78(c); break;
  case 0x79: cpu_op_79(c); break;
  case 0x7a: cpu_op_7A(c); break;
  case 0x7b: cpu_op_7B(c); break;
  case 0x7c: cpu_op_7C(c); break;
  case 0x7d: cpu_op_7D(c); break;
  case 0x7e: cpu_op_7E(c); break;
  case 0x7f: cpu_op_7F(c); break;
  case 0x80: cpu_op_80(c); break;
  case 0x81: cpu_op_81(c); break;
  case 0x82: cpu_op_82(c); break;
  case 0x83: cpu_op_83(c); break;
  case 0x84: cpu_op_84(c); break;
  case 0x85: cpu_op_85(c); break;
  case 0x86: cpu_op_86(c); break;
  case 0x87: cpu_op_87(c); break;
  case 0x88: cpu_op_88(c); break;
  case 0x89: cpu_op_89(c); break;
  case 0x8a: cpu_op_8A(c); break;
  case 0x8b: cpu_op_8B(c); break;
  case 0x8c: cpu_op_8C(c); break;
  case 0x8d: cpu_op_8D(c); break;
  case 0x8e: cpu_op_8E(c); break;
  case 0x8f: cpu_op_8F(c); break;
  case 0x90: cpu_op_90(c); break;
  case 0x91: cpu_op_91(c); break;
  case 0x92: cpu_op_92(c); break;
  case 0x93: cpu_op_93(c); break;
  case 0x94: cpu_op_94(c); break;
  case 0x95: cpu_op_95(c); break;
  case 0x96: cpu_op_96(c); break;
  case 0x97: cpu_op_97(c); break;
  case 0x98: cpu_op_98(c); break;
  case 0x99: cpu_op_99(c); break;
  case 0x9a: cpu_op_9A(c); break;
  case 0x9b: cpu_op_9B(c); break;
  case 0x9c: cpu_op_9C(c); break;
  case 0x9d: cpu_op_9D(c); break;
  case 0x9e: cpu_op_9E(c); break;
  case 0x9f: cpu_op_9F(c); break;
  case 0xa0: cpu_op_A0(c); break;
  case 0xa1: cpu_op_A1(c); break;
  case 0xa2: cpu_op_A2(c); break;
  case 0xa3: cpu_op_A3(c); break;
  case 0xa4: cpu_op_A4(c); break;
  case 0xa5: cpu_op_A5(c); break;
  case 0xa6: cpu_op_A6(c); break;
  case 0xa7: cpu_op_A7(c); break;
  case 0xa8: cpu_op_A8(c); break;
  case 0xa9: cpu_op_A9(c); break;
  case 0xaa: cpu_op_AA(c); break;
  case 0xab: cpu_op_AB(c); break;
  case 0xac: cpu_op_AC(c); break;
  case 0xad: cpu_op_AD(c); break;
  case 0xae: cpu_op_AE(c); break;
  case 0xaf: cpu_op_AF(c); break;
  case 0xb0: cpu_op_B0(c); break;
  case 0xb1: cpu_op_B1(c); break;
  case 0xb2: cpu_op_B2(c); break;
  case 0xb3: cpu_op_B3(c); break;
  case 0xb4: cpu_op_B4(c); break;
  case 0xb5: cpu_op_B5(c); break;
  case 0xb6: cpu_op_B6(c); break;
  case 0xb7: cpu_op_B7(c); break;
  case 0xb8: cpu_op_B8(c); break;
  case 0xb9: cpu_op_B9(c); break;
  case 0xba: cpu_op_BA(c); break;
  case 0xbb: cpu_op_BB(c); break;
  case 0xbc: cpu_op_BC(c); break;
  case 0xbd: cpu_op_BD(c); break;
  case 0xbe: cpu_op_BE(c); break;
  case 0xbf: cpu_op_BF(c); break;
  case 0xc0: cpu_op_C0(c); break;
  case 0xc1: cpu_op_C1(c); break;
  case 0xc2: cpu_op_C2(c); break;
  case 0xc3: cpu_op_C3(c); break;
  case 0xc4: cpu_op_C4(c); break;
  case 0xc5: cpu_op_C5(c); break;
  case 0xc6: cpu_op_C6(c); break;
  case 0xc7: cpu_op_C7(c); break;
  case 0xc8: cpu_op_C8(c); break;
  case 0xc9: cpu_op_C9(c); break;
  case 0xca: cpu_op_CA(c); break;
  case 0xcb: cpu_op_CB(c); break;
  case 0xcc: cpu_op_CC(c); break;
  case 0xcd: cpu_op_CD(c); break;
  case 0xce: cpu_op_CE(c); break;
  case 0xcf: cpu_op_CF(c); break;
  case 0xd0: cpu_op_D0(c); break;
  case 0xd1: cpu_op_D1(c); break;
  case 0xd2: cpu_op_D2(c); break;
  case 0xd3: cpu_op_D3(c); break;
  case 0xd4: cpu_op_D4(c); break;
  case 0xd5: cpu_op_D5(c); break;
  case 0xd6: cpu_op_D6(c); break;
  case 0xd7: cpu_op_D7(c); break;
  case 0xd8: cpu_op_D8(c); break;
  case 0xd9: cpu_op_D9(c); break;
  case 0xda: cpu_op_DA(c); break;
  case 0xdb: cpu_op_DB(c); break;
  case 0xdc: cpu_op_DC(c); break;
  case 0xdd: cpu_op_DD(c); break;
  case 0xde: cpu_op_DE(c); break;
  case 0xdf: cpu_op_DF(c); break;
  case 0xe0: cpu_op_E0(c); break;
  case 0xe1: cpu_op_E1(c); break;
  case 0xe2: cpu_op_E2(c); break;
  case 0xe3: cpu_op_E3(c); break;
  case 0xe4: cpu_op_E4(c); break;
  case 0xe5: cpu_op_E5(c); break;
  case 0xe6: cpu_op_E6(c); break;
  case 0xe7: cpu_op_E7(c); break;
  case 0xe8: cpu_op_E8(c); break;
  case 0xe9: cpu_op_E9(c); break;
  case 0xea: cpu_op_EA(c); break;
  case 0xeb: cpu_op_EB(c); break;
  case 0xec: cpu_op_EC(c); break;
  case 0xed: cpu_op_ED(c); break;
  case 0xee: cpu_op_EE(c); break;
  case 0xef: cpu_op_EF(c); break;
  case 0xf0: cpu_op_F0(c); break;
  case 0xf1: cpu_op_F1(c); break;
  case 0xf2: cpu_op_F2(c); break;
  case 0xf3: cpu_op_F3(c); break;
  case 0xf4: cpu_op_F4(c); break;
  case 0xf5: cpu_op_F5(c); break;
  case 0xf6: cpu_op_F6(c); break;
  case 0xf7: cpu_op_F7(c); break;
  case 0xf8: cpu_op_F8(c); break;
  case 0xf9: cpu_op_F9(c); break;
  case 0xfa: cpu_op_FA(c); break;
  case 0xfb: cpu_op_FB(c); break;
  case 0xfc: cpu_op_FC(c); break;
  case 0xfd: cpu_op_FD(c); break;
  case 0xfe: cpu_op_FE(c); break;
  case 0xff: cpu_op_FF(c); break;
  }
}

#undef CPU_OP

#endif
