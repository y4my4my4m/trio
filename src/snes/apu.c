#include "apu.h"
#include "dsp.h"

#include <string.h>

enum { F_C = 0x01, F_Z = 0x02, F_I = 0x04, F_H = 0x08, F_B = 0x10, F_P = 0x20,
       F_V = 0x40, F_N = 0x80 };

//The chip's 64-byte boot ROM ($FFC0-$FFFF while CONTROL bit 7 is set) is
//not in this file: its transfer protocol is run in C (ipl_step). The PC
//moves through the addresses of the protocol's steps in the ROM, so a saved
//state taken during a transfer continues.
enum {
  IPL_RESET = 0xFFC0, IPL_CLEAR = 0xFFC5, IPL_AA = 0xFFC9, IPL_BB = 0xFFCC, IPL_WAIT = 0xFFCF, IPL_FIRST = 0xFFD6,
  IPL_COUNT = 0xFFDA, IPL_BYTE = 0xFFDE, IPL_STORE = 0xFFE0, IPL_AHEAD = 0xFFEB, IPL_ADDR = 0xFFEF, IPL_CMD = 0xFFF3,
  IPL_ECHO = 0xFFF5,
};

//Base cycles per opcode; taken branches add 2.
static const uint8_t cycle_table[256] = {
  2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8,  //0
  2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,  //1
  2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4,  //2
  2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,  //3
  2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6,  //4
  2,8,4,5,4,5,5,6,5,5,4,5,2,2,4,3,  //5
  2,8,4,5,3,4,3,6,2,6,4,4,5,4,5,5,  //6
  2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,  //7
  2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5,  //8
  2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5, //9
  3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4,  //A
  2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,  //B
  3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9,  //C
  2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,  //D
  2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3,  //E
  2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,3,  //F
};

//---------------------------------------------------------------- I/O

static uint8_t io_read(Apu *a, uint16_t addr) {
  switch (addr) {
  case 0xF2: return a->dsp_addr;
  case 0xF3: return dsp_read(a->dsp_opaque, a->dsp_addr & 0x7F);
  case 0xF4: case 0xF5: case 0xF6: case 0xF7: return a->port_in[addr - 0xF4];
  case 0xF8: case 0xF9: return a->ram[addr];
  case 0xFD: case 0xFE: case 0xFF: {
    int i = addr - 0xFD;
    uint8_t v = a->timer_counter[i] & 0x0F;
    a->timer_counter[i] = 0;
    return v;
  }
  default: return 0;  //$F0, $F1, $FA-$FC are write-only
  }
}

static void io_write(Apu *a, uint16_t addr, uint8_t v) {
  switch (addr) {
  case 0xF1:
    for (int i = 0; i < 3; i++) {
      bool en = (v >> i) & 1;
      if (en && !a->timer_enabled[i]) {
        a->timer_stage[i] = 0;
        a->timer_counter[i] = 0;
      }
      a->timer_enabled[i] = en;
    }
    if (v & 0x10) a->port_in[0] = a->port_in[1] = 0;
    if (v & 0x20) a->port_in[2] = a->port_in[3] = 0;
    a->control = v;
    break;
  case 0xF2: a->dsp_addr = v; break;
  case 0xF3: if (a->dsp_addr < 0x80) dsp_write(a->dsp_opaque, a->dsp_addr, v); break;
  case 0xF4: case 0xF5: case 0xF6: case 0xF7: a->port_out[addr - 0xF4] = v; break;
  case 0xFA: case 0xFB: case 0xFC: a->timer_target[addr - 0xFA] = v; break;
  default: break;  //$F0 TEST, $F8/$F9 RAM, $FD-$FF read-only
  }
}

//A program reading $FFC0-$FFFF with the boot ROM mapped gets the RAM
//beneath it (the three games' drivers never read it).
static uint8_t rd(Apu *a, uint16_t addr) {
  if ((addr & 0xFFF0) == 0x00F0) return io_read(a, addr);
  return a->ram[addr];
}

//RAM beneath $F0-$FF and the IPL region always receives the write.
static void wr(Apu *a, uint16_t addr, uint8_t v) {
  if ((addr & 0xFFF0) == 0x00F0) io_write(a, addr, v);
  a->ram[addr] = v;
}

//---------------------------------------------------------------- time

static void tick(Apu *a, int n) {
  static const int period[3] = { 128, 128, 16 };
  a->cycles += (uint64_t)n;
  for (int i = 0; i < 3; i++) {
    a->timer_div[i] += n;
    while (a->timer_div[i] >= period[i]) {
      a->timer_div[i] -= period[i];
      if (!a->timer_enabled[i]) continue;
      //Target 0 matches after 256 ticks via uint8 wrap.
      if (++a->timer_stage[i] == a->timer_target[i]) {
        a->timer_stage[i] = 0;
        a->timer_counter[i] = (a->timer_counter[i] + 1) & 0x0F;
      }
    }
  }
  a->sample_div += n;
  while (a->sample_div >= 32) {
    a->sample_div -= 32;
    int16_t s[2];
    dsp_step_sample(a->dsp_opaque, a->ram, s);
    if (a->ring_w - a->ring_r >= APU_RING) a->ring_r++;
    int16_t *d = a->ring[a->ring_w & (APU_RING - 1)];
    d[0] = s[0];
    d[1] = s[1];
    a->ring_w++;
  }
}

//---------------------------------------------------------------- SPC700

static inline uint8_t imm(Apu *a) { return rd(a, a->pc++); }

static inline uint16_t imm16(Apu *a) {
  uint16_t lo = imm(a);
  return (uint16_t)(lo | imm(a) << 8);
}

static inline uint16_t dp(Apu *a, uint8_t o) {
  return (uint16_t)((a->psw & F_P ? 0x100 : 0) | o);
}

//16-bit read of a direct-page word; high byte wraps within the page.
static inline uint16_t rd16dp(Apu *a, uint8_t o) {
  uint16_t lo = rd(a, dp(a, o));
  return (uint16_t)(lo | rd(a, dp(a, (uint8_t)(o + 1))) << 8);
}

static inline void wr16dp(Apu *a, uint8_t o, uint16_t v) {
  wr(a, dp(a, o), (uint8_t)v);
  wr(a, dp(a, (uint8_t)(o + 1)), (uint8_t)(v >> 8));
}

static inline uint16_t rd16(Apu *a, uint16_t addr) {
  uint16_t lo = rd(a, addr);
  return (uint16_t)(lo | rd(a, (uint16_t)(addr + 1)) << 8);
}

static inline void push(Apu *a, uint8_t v) { wr(a, (uint16_t)(0x100 | a->sp--), v); }
static inline uint8_t pop(Apu *a) { return rd(a, (uint16_t)(0x100 | ++a->sp)); }

static inline void push16(Apu *a, uint16_t v) {
  push(a, (uint8_t)(v >> 8));
  push(a, (uint8_t)v);
}

static inline uint16_t pop16(Apu *a) {
  uint16_t lo = pop(a);
  return (uint16_t)(lo | pop(a) << 8);
}

static inline void setf(Apu *a, uint8_t f, int on) {
  a->psw = on ? (a->psw | f) : (a->psw & ~f);
}

static inline uint8_t nz(Apu *a, uint8_t v) {
  a->psw = (uint8_t)((a->psw & ~(F_N | F_Z)) | (v & 0x80) | (v ? 0 : F_Z));
  return v;
}

static inline void nz16(Apu *a, uint16_t v) {
  a->psw = (uint8_t)((a->psw & ~(F_N | F_Z)) | ((v >> 8) & 0x80) | (v ? 0 : F_Z));
}

static inline uint16_t ya(Apu *a) { return (uint16_t)(a->y << 8 | a->a); }
static inline void set_ya(Apu *a, uint16_t v) { a->a = (uint8_t)v; a->y = (uint8_t)(v >> 8); }

static uint8_t adc(Apu *a, uint8_t x, uint8_t y) {
  int r = x + y + (a->psw & F_C);
  setf(a, F_C, r > 0xFF);
  setf(a, F_H, (x ^ y ^ r) & 0x10);
  setf(a, F_V, ~(x ^ y) & (x ^ r) & 0x80);
  return nz(a, (uint8_t)r);
}

static uint8_t sbc(Apu *a, uint8_t x, uint8_t y) { return adc(a, x, (uint8_t)~y); }

static void cmp(Apu *a, uint8_t x, uint8_t y) {
  int r = x - y;
  setf(a, F_C, r >= 0);
  nz(a, (uint8_t)r);
}

//kind: 0 OR, 1 AND, 2 EOR, 3 CMP, 4 ADC, 5 SBC. Returns the value to store.
static uint8_t alu(Apu *a, int kind, uint8_t x, uint8_t y) {
  switch (kind) {
  case 0: return nz(a, x | y);
  case 1: return nz(a, x & y);
  case 2: return nz(a, x ^ y);
  case 3: cmp(a, x, y); return x;
  case 4: return adc(a, x, y);
  default: return sbc(a, x, y);
  }
}

//kind: 0 ASL, 1 ROL, 2 LSR, 3 ROR, 4 DEC, 5 INC.
static uint8_t rmw(Apu *a, int kind, uint8_t v) {
  int c = a->psw & F_C;
  switch (kind) {
  case 0: setf(a, F_C, v & 0x80); v = (uint8_t)(v << 1); break;
  case 1: setf(a, F_C, v & 0x80); v = (uint8_t)(v << 1 | c); break;
  case 2: setf(a, F_C, v & 1); v >>= 1; break;
  case 3: setf(a, F_C, v & 1); v = (uint8_t)(v >> 1 | c << 7); break;
  case 4: v--; break;
  default: v++; break;
  }
  return nz(a, v);
}

//mem.bit operand: bits 0-12 address, bits 13-15 bit number.
static inline int membit(Apu *a, uint16_t *addr, uint8_t *val) {
  uint16_t w = imm16(a);
  *addr = w & 0x1FFF;
  *val = rd(a, *addr);
  return w >> 13;
}

static inline int branch(Apu *a, int cond) {
  int8_t rel = (int8_t)imm(a);
  if (!cond) return 0;
  a->pc = (uint16_t)(a->pc + rel);
  return 2;
}

//The boot program, one step of its protocol per call; returns the SPC
//cycles the step takes on the chip. Every step that reads or writes a port
//does so first, and the cycle counts are the chip's: the S-CPU's side of a
//transfer races the chip (G2 writes the count, then the data byte, a few
//cycles apart), so a step that looked at two ports at once, or took 4
//cycles more at reset, left G2's driver corrupt.
//  RESET  SP = X = $EF, A = 0
//  CLEAR  $01..X cleared, downward
//  AA BB  $AA / $BB on ports 0 / 1: ready
//  WAIT   for $CC on port 0
//  ADDR   the address on ports 2 / 3 is kept at $00 / $01
//  CMD    port 0 and port 1 (0: run, else data follows) are read
//  ECHO   port 0's value is echoed; then to BLADE, or a jump to the address
//  BLADE   waits for port 0 = 0: the count (Y) of the block's first byte
//  COUNT  port 0 = the count: the byte is there (BYTE, STORE: read from
//         port 1, count echoed, stored at (address) + Y, Y up by one; past
//         a page the address's high byte goes up). A port 0 value ahead of
//         the count (AHEAD checks it again) starts the next block (ADDR).
static int ipl_step(Apu *a) {
  uint16_t pc = a->pc;
  if (pc < IPL_CLEAR) {
    a->sp = a->x = 0xEF;
    a->a = nz(a, 0);
    a->pc = IPL_CLEAR;
    return 6;
  }
  if (pc < IPL_AA) {
    int n = a->x ? a->x : 256;
    while (a->x) wr(a, a->x--, a->a);
    nz(a, 0);
    a->pc = IPL_AA;
    return n * 10 - 2;
  }
  if (pc < IPL_BB) { wr(a, 0xF4, 0xAA); a->pc = IPL_BB; return 5; }
  if (pc < IPL_WAIT) { wr(a, 0xF5, 0xBB); a->pc = IPL_WAIT; return 5; }
  if (pc < IPL_FIRST) {
    if (a->port_in[0] != 0xCC) return 9;
    a->pc = IPL_ADDR;
    return 11;
  }
  if (pc < IPL_COUNT) {
    a->y = nz(a, a->port_in[0]);
    if (a->y) return 7;
    a->pc = IPL_COUNT;
    return 5;
  }
  if (pc < IPL_BYTE) {
    uint8_t d = (uint8_t)(a->y - a->port_in[0]);
    if (!d) { a->pc = IPL_BYTE; return 5; }
    if (!(d & 0x80)) return 11;
    a->pc = IPL_AHEAD;
    return 9;
  }
  if (pc < IPL_STORE) {
    a->a = a->port_in[1];
    a->pc = IPL_STORE;
    return 3;
  }
  if (pc < IPL_AHEAD) {
    wr(a, 0xF4, a->y);
    wr(a, (uint16_t)((a->ram[0] | a->ram[1] << 8) + a->y), a->a);
    a->pc = IPL_COUNT;
    if (++a->y) return 17;
    wr(a, 1, (uint8_t)(a->ram[1] + 1));
    if (!(a->ram[1] & 0x80)) return 23;
    a->pc = IPL_AHEAD;
    return 21;
  }
  if (pc < IPL_ADDR) {
    if (!((uint8_t)(a->y - a->port_in[0]) & 0x80)) { a->pc = IPL_COUNT; return 7; }
    a->pc = IPL_ADDR;
    return 5;
  }
  if (pc < IPL_CMD) {
    wr(a, 0, a->port_in[2]);
    wr(a, 1, a->port_in[3]);
    a->pc = IPL_CMD;
    return 10;
  }
  if (pc < IPL_ECHO) {
    a->a = a->port_in[0];
    a->y = a->port_in[1];
    a->pc = IPL_ECHO;
    return 5;
  }
  wr(a, 0xF4, a->a);
  a->a = a->x = nz(a, a->y);
  if (a->x) { a->pc = IPL_FIRST; return 12; }
  a->pc = (uint16_t)(a->ram[0] | a->ram[1] << 8);
  return 16;
}

static int step(Apu *a) {
  if (a->pc >= IPL_RESET && (a->control & 0x80)) return ipl_step(a);
  uint8_t op = imm(a);
  int cyc = cycle_table[op];
  int lo = op & 0x0F, hi = op >> 4;

  //ALU group: rows 0-B, columns 4-9.
  if (hi < 0xC && lo >= 4 && lo <= 9) {
    int kind = op >> 5;
    bool odd = hi & 1;
    uint16_t addr;
    uint8_t m;
    if (!odd) {
      switch (lo) {
      case 4: m = rd(a, dp(a, imm(a))); break;
      case 5: m = rd(a, imm16(a)); break;
      case 6: m = rd(a, dp(a, a->x)); break;
      case 7: m = rd(a, rd16dp(a, (uint8_t)(imm(a) + a->x))); break;
      case 8: m = imm(a); break;
      default: {
        uint8_t s = rd(a, dp(a, imm(a)));
        addr = dp(a, imm(a));
        uint8_t r = alu(a, kind, rd(a, addr), s);
        if (kind != 3) wr(a, addr, r);
        return cyc;
      }
      }
    } else {
      switch (lo) {
      case 4: m = rd(a, dp(a, (uint8_t)(imm(a) + a->x))); break;
      case 5: m = rd(a, (uint16_t)(imm16(a) + a->x)); break;
      case 6: m = rd(a, (uint16_t)(imm16(a) + a->y)); break;
      case 7: m = rd(a, (uint16_t)(rd16dp(a, imm(a)) + a->y)); break;
      case 8: {
        uint8_t s = imm(a);
        addr = dp(a, imm(a));
        uint8_t r = alu(a, kind, rd(a, addr), s);
        if (kind != 3) wr(a, addr, r);
        return cyc;
      }
      default: {
        uint8_t s = rd(a, dp(a, a->y));
        addr = dp(a, a->x);
        uint8_t r = alu(a, kind, rd(a, addr), s);
        if (kind != 3) wr(a, addr, r);
        return cyc;
      }
      }
    }
    a->a = alu(a, kind, a->a, m);
    return cyc;
  }

  //Shift/INC/DEC group: rows 0-B, columns B-C.
  if (hi < 0xC && (lo == 0xB || lo == 0xC)) {
    int kind = op >> 5;
    uint16_t addr;
    if (hi & 1) {
      if (lo == 0xC) { a->a = rmw(a, kind, a->a); return cyc; }
      addr = dp(a, (uint8_t)(imm(a) + a->x));
    } else {
      addr = lo == 0xB ? dp(a, imm(a)) : imm16(a);
    }
    wr(a, addr, rmw(a, kind, rd(a, addr)));
    return cyc;
  }

  switch (lo) {
  case 0x1:  //TCALL n
    push16(a, a->pc);
    a->pc = rd16(a, (uint16_t)(0xFFDE - hi * 2));
    return cyc;
  case 0x2: {  //SET1/CLR1 dp.bit
    uint16_t addr = dp(a, imm(a));
    uint8_t v = rd(a, addr), bit = (uint8_t)(1 << (op >> 5));
    wr(a, addr, hi & 1 ? (uint8_t)(v & ~bit) : (uint8_t)(v | bit));
    return cyc;
  }
  case 0x3: {  //BBS/BBC dp.bit,rel
    uint8_t v = rd(a, dp(a, imm(a)));
    int set = (v >> (op >> 5)) & 1;
    return cyc + branch(a, (hi & 1) ? !set : set);
  }
  case 0x0:
    if (hi & 1) {  //conditional branches
      static const uint8_t flag[4] = { F_N, F_V, F_C, F_Z };
      int set = (a->psw & flag[op >> 6]) != 0;
      return cyc + branch(a, set == ((op >> 5) & 1));
    }
    break;
  default: break;
  }

  uint16_t addr, w;
  uint8_t v;
  int bit;

  switch (op) {
  case 0x00: break;                               //NOP
  case 0x20: a->psw &= ~F_P; break;               //CLRP
  case 0x40: a->psw |= F_P; break;                //SETP
  case 0x60: a->psw &= ~F_C; break;               //CLRC
  case 0x80: a->psw |= F_C; break;                //SETC
  case 0xA0: a->psw |= F_I; break;                //EI
  case 0xC0: a->psw &= ~F_I; break;               //DI
  case 0xE0: a->psw &= ~(F_V | F_H); break;       //CLRV
  case 0xED: a->psw ^= F_C; break;                //NOTC

  //Moves to memory.
  case 0xC4: wr(a, dp(a, imm(a)), a->a); break;
  case 0xC5: wr(a, imm16(a), a->a); break;
  case 0xC6: wr(a, dp(a, a->x), a->a); break;
  case 0xC7: wr(a, rd16dp(a, (uint8_t)(imm(a) + a->x)), a->a); break;
  case 0xD4: wr(a, dp(a, (uint8_t)(imm(a) + a->x)), a->a); break;
  case 0xD5: wr(a, (uint16_t)(imm16(a) + a->x), a->a); break;
  case 0xD6: wr(a, (uint16_t)(imm16(a) + a->y), a->a); break;
  case 0xD7: wr(a, (uint16_t)(rd16dp(a, imm(a)) + a->y), a->a); break;
  case 0xC9: wr(a, imm16(a), a->x); break;
  case 0xD8: wr(a, dp(a, imm(a)), a->x); break;
  case 0xD9: wr(a, dp(a, (uint8_t)(imm(a) + a->y)), a->x); break;
  case 0xCB: wr(a, dp(a, imm(a)), a->y); break;
  case 0xCC: wr(a, imm16(a), a->y); break;
  case 0xDB: wr(a, dp(a, (uint8_t)(imm(a) + a->x)), a->y); break;
  case 0xAF: wr(a, dp(a, a->x++), a->a); break;   //MOV (X)+,A
  case 0xFA: v = rd(a, dp(a, imm(a))); wr(a, dp(a, imm(a)), v); break;
  case 0x8F: v = imm(a); wr(a, dp(a, imm(a)), v); break;

  //Loads.
  case 0xE4: a->a = nz(a, rd(a, dp(a, imm(a)))); break;
  case 0xE5: a->a = nz(a, rd(a, imm16(a))); break;
  case 0xE6: a->a = nz(a, rd(a, dp(a, a->x))); break;
  case 0xE7: a->a = nz(a, rd(a, rd16dp(a, (uint8_t)(imm(a) + a->x)))); break;
  case 0xE8: a->a = nz(a, imm(a)); break;
  case 0xF4: a->a = nz(a, rd(a, dp(a, (uint8_t)(imm(a) + a->x)))); break;
  case 0xF5: a->a = nz(a, rd(a, (uint16_t)(imm16(a) + a->x))); break;
  case 0xF6: a->a = nz(a, rd(a, (uint16_t)(imm16(a) + a->y))); break;
  case 0xF7: a->a = nz(a, rd(a, (uint16_t)(rd16dp(a, imm(a)) + a->y))); break;
  case 0xBF: a->a = nz(a, rd(a, dp(a, a->x++))); break;  //MOV A,(X)+
  case 0xCD: a->x = nz(a, imm(a)); break;
  case 0xE9: a->x = nz(a, rd(a, imm16(a))); break;
  case 0xF8: a->x = nz(a, rd(a, dp(a, imm(a)))); break;
  case 0xF9: a->x = nz(a, rd(a, dp(a, (uint8_t)(imm(a) + a->y)))); break;
  case 0x8D: a->y = nz(a, imm(a)); break;
  case 0xEB: a->y = nz(a, rd(a, dp(a, imm(a)))); break;
  case 0xEC: a->y = nz(a, rd(a, imm16(a))); break;
  case 0xFB: a->y = nz(a, rd(a, dp(a, (uint8_t)(imm(a) + a->x)))); break;

  //Register transfers.
  case 0x5D: a->x = nz(a, a->a); break;
  case 0x7D: a->a = nz(a, a->x); break;
  case 0xDD: a->a = nz(a, a->y); break;
  case 0xFD: a->y = nz(a, a->a); break;
  case 0x9D: a->x = nz(a, a->sp); break;
  case 0xBD: a->sp = a->x; break;

  //Index compares, INC/DEC.
  case 0xC8: cmp(a, a->x, imm(a)); break;
  case 0xAD: cmp(a, a->y, imm(a)); break;
  case 0x1E: cmp(a, a->x, rd(a, imm16(a))); break;
  case 0x3E: cmp(a, a->x, rd(a, dp(a, imm(a)))); break;
  case 0x5E: cmp(a, a->y, rd(a, imm16(a))); break;
  case 0x7E: cmp(a, a->y, rd(a, dp(a, imm(a)))); break;
  case 0x1D: a->x = nz(a, (uint8_t)(a->x - 1)); break;
  case 0x3D: a->x = nz(a, (uint8_t)(a->x + 1)); break;
  case 0xDC: a->y = nz(a, (uint8_t)(a->y - 1)); break;
  case 0xFC: a->y = nz(a, (uint8_t)(a->y + 1)); break;

  //Stack.
  case 0x0D: push(a, a->psw); break;
  case 0x2D: push(a, a->a); break;
  case 0x4D: push(a, a->x); break;
  case 0x6D: push(a, a->y); break;
  case 0x8E: a->psw = pop(a); break;
  case 0xAE: a->a = pop(a); break;
  case 0xCE: a->x = pop(a); break;
  case 0xEE: a->y = pop(a); break;

  //16-bit.
  case 0x1A: v = imm(a); w = (uint16_t)(rd16dp(a, v) - 1); wr16dp(a, v, w); nz16(a, w); break;
  case 0x3A: v = imm(a); w = (uint16_t)(rd16dp(a, v) + 1); wr16dp(a, v, w); nz16(a, w); break;
  case 0x5A: {  //CMPW YA,dp
    w = rd16dp(a, imm(a));
    int r = ya(a) - w;
    setf(a, F_C, r >= 0);
    nz16(a, (uint16_t)r);
    break;
  }
  case 0x7A: {  //ADDW YA,dp; H = carry into bit 12
    w = rd16dp(a, imm(a));
    uint16_t x = ya(a);
    int r = x + w;
    setf(a, F_C, r > 0xFFFF);
    setf(a, F_H, (x ^ w ^ r) & 0x1000);
    setf(a, F_V, ~(x ^ w) & (x ^ r) & 0x8000);
    set_ya(a, (uint16_t)r);
    nz16(a, (uint16_t)r);
    break;
  }
  case 0x9A: {  //SUBW YA,dp; H = no borrow into bit 12
    w = rd16dp(a, imm(a));
    uint16_t x = ya(a);
    int r = x - w;
    setf(a, F_C, r >= 0);
    setf(a, F_H, !((x ^ w ^ r) & 0x1000));
    setf(a, F_V, (x ^ w) & (x ^ r) & 0x8000);
    set_ya(a, (uint16_t)r);
    nz16(a, (uint16_t)r);
    break;
  }
  case 0xBA: w = rd16dp(a, imm(a)); set_ya(a, w); nz16(a, w); break;
  case 0xDA: wr16dp(a, imm(a), ya(a)); break;

  //Bit ops.
  case 0x0A: bit = membit(a, &addr, &v); if ((v >> bit) & 1) a->psw |= F_C; break;
  case 0x2A: bit = membit(a, &addr, &v); if (!((v >> bit) & 1)) a->psw |= F_C; break;
  case 0x4A: bit = membit(a, &addr, &v); if (!((v >> bit) & 1)) a->psw &= ~F_C; break;
  case 0x6A: bit = membit(a, &addr, &v); if ((v >> bit) & 1) a->psw &= ~F_C; break;
  case 0x8A: bit = membit(a, &addr, &v); if ((v >> bit) & 1) a->psw ^= F_C; break;
  case 0xAA: bit = membit(a, &addr, &v); setf(a, F_C, (v >> bit) & 1); break;
  case 0xCA:
    bit = membit(a, &addr, &v);
    v = (a->psw & F_C) ? (uint8_t)(v | 1 << bit) : (uint8_t)(v & ~(1 << bit));
    wr(a, addr, v);
    break;
  case 0xEA: bit = membit(a, &addr, &v); wr(a, addr, (uint8_t)(v ^ 1 << bit)); break;
  case 0x0E: case 0x4E:  //TSET1/TCLR1 !abs; NZ from A-mem
    addr = imm16(a);
    v = rd(a, addr);
    nz(a, (uint8_t)(a->a - v));
    wr(a, addr, op == 0x0E ? (uint8_t)(v | a->a) : (uint8_t)(v & ~a->a));
    break;

  //Compare-and-branch.
  case 0x2E: v = rd(a, dp(a, imm(a))); cyc += branch(a, a->a != v); break;
  case 0xDE: v = rd(a, dp(a, (uint8_t)(imm(a) + a->x))); cyc += branch(a, a->a != v); break;
  case 0x6E:
    addr = dp(a, imm(a));
    v = (uint8_t)(rd(a, addr) - 1);
    wr(a, addr, v);
    cyc += branch(a, v != 0);
    break;
  case 0xFE: a->y--; cyc += branch(a, a->y != 0); break;

  //Flow.
  case 0x2F: cyc += branch(a, 1) - 2; break;  //BRA; base count includes the jump
  case 0x5F: a->pc = imm16(a); break;
  case 0x1F: a->pc = rd16(a, (uint16_t)(imm16(a) + a->x)); break;
  case 0x3F: w = imm16(a); push16(a, a->pc); a->pc = w; break;
  case 0x4F: v = imm(a); push16(a, a->pc); a->pc = (uint16_t)(0xFF00 | v); break;
  case 0x6F: a->pc = pop16(a); break;
  case 0x7F: a->psw = pop(a); a->pc = pop16(a); break;
  case 0x0F:  //BRK
    push16(a, a->pc);
    push(a, a->psw);
    a->psw = (uint8_t)((a->psw | F_B) & ~F_I);
    a->pc = rd16(a, 0xFFDE);
    break;

  //Arithmetic.
  case 0x9F: a->a = nz(a, (uint8_t)(a->a >> 4 | a->a << 4)); break;  //XCN
  case 0xCF: {  //MUL YA; NZ from Y
    uint16_t r = (uint16_t)(a->y * a->a);
    set_ya(a, r);
    nz(a, a->y);
    break;
  }
  case 0x9E: {  //DIV YA,X; out-of-range quotient per bsnes model
    unsigned yav = ya(a), x = a->x;
    setf(a, F_V, a->y >= x);
    setf(a, F_H, (a->y & 15) >= (x & 15));
    if (a->y < (x << 1)) {
      a->a = (uint8_t)(yav / x);
      a->y = (uint8_t)(yav % x);
    } else {
      a->a = (uint8_t)(255 - (yav - (x << 9)) / (256 - x));
      a->y = (uint8_t)(x + (yav - (x << 9)) % (256 - x));
    }
    nz(a, a->a);
    break;
  }
  case 0xDF:  //DAA
    if ((a->psw & F_C) || a->a > 0x99) { a->a += 0x60; a->psw |= F_C; }
    if ((a->psw & F_H) || (a->a & 15) > 9) a->a += 6;
    nz(a, a->a);
    break;
  case 0xBE:  //DAS
    if (!(a->psw & F_C) || a->a > 0x99) { a->a -= 0x60; a->psw &= ~F_C; }
    if (!(a->psw & F_H) || (a->a & 15) > 9) a->a -= 6;
    nz(a, a->a);
    break;

  case 0xEF: case 0xFF:  //SLEEP, STOP
    a->stopped = true;
    break;

  default: break;
  }
  return cyc;
}

//---------------------------------------------------------------- API

void apu_reset(Apu *a) {
  memset(a->ram, 0, sizeof a->ram);
  a->a = a->x = a->y = 0;
  a->psw = 0;
  a->sp = 0xEF;
  a->stopped = false;
  a->control = 0xB0;
  a->dsp_addr = 0;
  memset(a->port_in, 0, sizeof a->port_in);
  memset(a->port_out, 0, sizeof a->port_out);
  for (int i = 0; i < 3; i++) {
    a->timer_target[i] = 0;
    a->timer_stage[i] = 0;
    a->timer_counter[i] = 0;
    a->timer_enabled[i] = false;
    a->timer_div[i] = 0;
  }
  a->cycles = 0;
  a->sample_div = 0;
  dsp_reset(a->dsp_opaque);
  memset(a->ring, 0, sizeof a->ring);
  a->ring_w = a->ring_r = 0;
  a->pc = IPL_RESET;
}

void apu_run(Apu *a, uint64_t target) {
  while (a->cycles < target) {
    if (a->stopped) {
      uint64_t n = target - a->cycles;
      tick(a, n > 4096 ? 4096 : (int)n);
    } else {
      tick(a, step(a));
    }
  }
}

uint8_t apu_cpu_read(Apu *a, int port) { return a->port_out[port & 3]; }

void apu_cpu_write(Apu *a, int port, uint8_t v) { a->port_in[port & 3] = v; }

int apu_pop_samples(Apu *a, int16_t *out, int max_frames) {
  uint32_t avail = a->ring_w - a->ring_r;
  int n = max_frames < 0 ? 0 : (avail < (uint32_t)max_frames ? (int)avail : max_frames);
  for (int i = 0; i < n; i++) {
    const int16_t *s = a->ring[a->ring_r++ & (APU_RING - 1)];
    out[i * 2] = s[0];
    out[i * 2 + 1] = s[1];
  }
  return n;
}
