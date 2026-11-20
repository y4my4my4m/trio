//Sample-level S-DSP: a C port of SPC_DSP.cpp from blargg's snes_spc 0.9.0
//(http://www.slack.net/~ant/). Per-voice ordering and arithmetic follow its
//voice_V2..V5, echo_22..30 and misc_27..30, collapsed to one step per
//output frame.
//
//Copyright (C) 2007 Shay Green.
//Modified 2026 by y4my4my4m: ported to C, one step per output frame, state
//in Apu.dsp_opaque.
//
//This file is free software; you can redistribute it and/or modify it under
//the terms of the GNU Lesser General Public License as published by the Free
//Software Foundation; either version 2.1 of the License, or (at your option)
//any later version. It is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser
//General Public License for more details: LICENSES/LGPL-2.1-or-later.txt.

#include "dsp.h"
#include "apu.h"

#include <stddef.h>
#include <string.h>

//Per-voice register offsets (voice n at n*0x10).
enum { V_VOLL, V_VOLR, V_PITCHL, V_PITCHH, V_SRCN, V_ADSR1, V_ADSR2, V_GAIN,
       V_ENVX, V_OUTX };

//Global registers.
enum {
  R_MVOLL = 0x0C, R_MVOLR = 0x1C, R_EVOLL = 0x2C, R_EVOLR = 0x3C,
  R_KON = 0x4C, R_KOFF = 0x5C, R_FLG = 0x6C, R_ENDX = 0x7C,
  R_EFB = 0x0D, R_PMON = 0x2D, R_NON = 0x3D, R_EON = 0x4D,
  R_DIR = 0x5D, R_ESA = 0x6D, R_EDL = 0x7D, R_FIR = 0x0F
};

enum { ENV_RELEASE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN };

#define BRR_BUF 12
#define COUNTER_RANGE (2048 * 5 * 3)

typedef struct {
  int buf[BRR_BUF * 2];  //decoded samples, second half mirrors the first
  int buf_pos;
  int interp_pos;        //4.12 fixed point
  int brr_addr;
  int brr_offset;
  int kon_delay;
  int env_mode;
  int env;               //11-bit
  int hidden_env;
} Voice;

typedef struct {
  uint8_t regs[128];
  Voice v[8];
  int echo_hist[16][2];  //8 entries, mirrored
  int echo_hist_pos;
  int echo_offset;
  int echo_length;
  int every_other;
  int kon, new_kon, koff;
  int counter;
  int noise;             //15-bit LFSR
} Dsp;

_Static_assert(sizeof(Dsp) <= sizeof(((Apu *)0)->dsp_opaque),
               "Dsp exceeds dsp_opaque");

//Gaussian interpolation kernel, 512 entries, 11-bit.
static const int16_t gauss[512] = {
     0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
     1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,
     2,   2,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,   5,
     6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  10,
    11,  11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  15,  16,  16,  17,  17,
    18,  19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  27,  27,
    28,  29,  29,  30,  31,  32,  32,  33,  34,  35,  36,  36,  37,  38,  39,  40,
    41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
    58,  59,  60,  61,  62,  64,  65,  66,  67,  69,  70,  71,  73,  74,  76,  77,
    78,  80,  81,  83,  84,  86,  87,  89,  90,  92,  94,  95,  97,  99, 100, 102,
   104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
   134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
   171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
   212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
   260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
   314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
   374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
   439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
   508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
   582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
   659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
   737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
   816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
   894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
   969, 974, 978, 983, 988, 992, 997,1001,1005,1010,1014,1019,1023,1027,1032,1036,
  1040,1045,1049,1053,1057,1061,1066,1070,1074,1078,1082,1086,1090,1094,1098,1102,
  1106,1109,1113,1117,1121,1125,1128,1132,1136,1139,1143,1146,1150,1153,1157,1160,
  1164,1167,1170,1174,1177,1180,1183,1186,1190,1193,1196,1199,1202,1205,1207,1210,
  1213,1216,1219,1221,1224,1227,1229,1232,1234,1237,1239,1241,1244,1246,1248,1251,
  1253,1255,1257,1259,1261,1263,1265,1267,1269,1270,1272,1274,1275,1277,1279,1280,
  1282,1283,1284,1286,1287,1288,1290,1291,1292,1293,1294,1295,1296,1297,1297,1298,
  1299,1300,1300,1301,1302,1302,1303,1303,1303,1304,1304,1304,1304,1304,1305,1305,
};

//Envelope/noise rate periods in samples; rate 0 never fires.
static const uint16_t counter_rates[32] = {
  0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
  64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1
};

static const uint16_t counter_offsets[32] = {
  1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
  0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0
};

static inline int clamp16(int v) {
  return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static inline int fires(const Dsp *d, int rate) {
  if (!rate) return 0;
  return (d->counter + counter_offsets[rate]) % counter_rates[rate] == 0;
}

void dsp_reset(void *p) {
  Dsp *d = p;
  memset(d, 0, sizeof *d);
  d->regs[R_FLG] = 0xE0;
  d->noise = 0x4000;
  d->every_other = 1;
  for (int i = 0; i < 8; i++) d->v[i].brr_offset = 1;
}

uint8_t dsp_read(void *p, uint8_t addr) {
  return ((Dsp *)p)->regs[addr & 0x7F];
}

void dsp_write(void *p, uint8_t addr, uint8_t val) {
  Dsp *d = p;
  if (addr >= 0x80) return;
  d->regs[addr] = val;
  if (addr == R_KON) d->new_kon = val;
  else if (addr == R_ENDX) d->regs[R_ENDX] = 0;  //any write clears
}

static void run_envelope(Dsp *d, Voice *v, const uint8_t *vr) {
  int env = v->env;
  if (v->env_mode == ENV_RELEASE) {
    if ((env -= 8) < 0) env = 0;
    v->env = env;
    return;
  }
  int rate;
  int env_data = vr[V_ADSR2];
  int adsr1 = vr[V_ADSR1];
  if (adsr1 & 0x80) {
    if (v->env_mode >= ENV_DECAY) {
      env--;
      env -= env >> 8;
      rate = env_data & 0x1F;
      if (v->env_mode == ENV_DECAY) rate = ((adsr1 >> 3) & 0x0E) + 0x10;
    } else {
      rate = (adsr1 & 0x0F) * 2 + 1;
      env += rate < 31 ? 0x20 : 0x400;
    }
  } else {
    env_data = vr[V_GAIN];
    int mode = env_data >> 5;
    if (mode < 4) {
      env = env_data * 0x10;
      rate = 31;
    } else {
      rate = env_data & 0x1F;
      if (mode == 4) {
        env -= 0x20;
      } else if (mode == 5) {
        env--;
        env -= env >> 8;
      } else {
        env += 0x20;
        if (mode == 7 && (unsigned)v->hidden_env >= 0x600) env += 0x08 - 0x20;
      }
    }
  }
  if ((env >> 8) == (env_data >> 5) && v->env_mode == ENV_DECAY)
    v->env_mode = ENV_SUSTAIN;
  v->hidden_env = env;
  //Unsigned compare also catches linear decrease below zero.
  if ((unsigned)env > 0x7FF) {
    env = env < 0 ? 0 : 0x7FF;
    if (v->env_mode == ENV_ATTACK) v->env_mode = ENV_DECAY;
  }
  if (fires(d, rate)) v->env = env;
}

//Block: 1 header byte (shift<<4 | filter<<2 | loop<<1 | end), 8 data bytes,
//high nibble first. Decodes 4 samples from 2 bytes.
static void decode_brr(Voice *v, const uint8_t *ram, int header) {
  unsigned nyb = (unsigned)ram[(v->brr_addr + v->brr_offset) & 0xFFFF] << 8 |
                 ram[(v->brr_addr + v->brr_offset + 1) & 0xFFFF];
  int *pos = &v->buf[v->buf_pos];
  if ((v->buf_pos += 4) >= BRR_BUF) v->buf_pos = 0;
  int shift = header >> 4;
  int filter = header & 0x0C;
  for (int *end = pos + 4; pos < end; pos++, nyb <<= 4) {
    int s = (int)((nyb >> 12) & 0x0F);
    s -= (s & 8) << 1;  //sign-extend 4 bits
    s = (s * (1 << shift)) >> 1;
    if (shift >= 0xD) s = s < 0 ? -2048 : 0;
    int p1 = pos[BRR_BUF - 1];
    int p2 = pos[BRR_BUF - 2] >> 1;
    if (filter >= 8) {
      s += p1;
      s -= p2;
      if (filter == 8) {
        s += p2 >> 4;
        s += (p1 * -3) >> 6;
      } else {
        s += (p1 * -13) >> 7;
        s += (p2 * 3) >> 4;
      }
    } else if (filter) {
      s += p1 >> 1;
      s += (-p1) >> 5;
    }
    s = clamp16(s);
    s = (int16_t)(s * 2);  //15-bit wrap
    pos[BRR_BUF] = pos[0] = s;
  }
}

static int interpolate(const Voice *v) {
  int off = (v->interp_pos >> 4) & 0xFF;
  const int16_t *fwd = gauss + 255 - off;
  const int16_t *rev = gauss + off;
  const int *in = &v->buf[(v->interp_pos >> 12) + v->buf_pos];
  int out;
  out  = (fwd[0]   * in[0]) >> 11;
  out += (fwd[256] * in[1]) >> 11;
  out += (rev[256] * in[2]) >> 11;
  out = (int16_t)out;
  out += (rev[0]   * in[3]) >> 11;
  return clamp16(out) & ~1;
}

void dsp_step_sample(void *p, uint8_t *ram, int16_t out[2]) {
  Dsp *d = p;
  uint8_t *r = d->regs;

  if (--d->counter < 0) d->counter = COUNTER_RANGE - 1;
  if (fires(d, r[R_FLG] & 0x1F)) {
    int fb = (d->noise << 13) ^ (d->noise << 14);
    d->noise = (fb & 0x4000) ^ (d->noise >> 1);
  }

  //KON/KOFF polled every other sample; KON bits clear once consumed.
  d->every_other ^= 1;
  if (d->every_other) {
    d->new_kon &= ~d->kon;
    d->kon = d->new_kon;
    d->koff = r[R_KOFF];
  }

  int pmon = r[R_PMON] & 0xFE;
  int non = r[R_NON];
  int eon = r[R_EON];
  int dir = r[R_DIR] << 8;
  int endx = r[R_ENDX];
  int main_out[2] = { 0, 0 };
  int echo_out[2] = { 0, 0 };
  int prev_out = 0;

  for (int i = 0; i < 8; i++) {
    Voice *v = &d->v[i];
    uint8_t *vr = &r[i << 4];
    int vbit = 1 << i;

    int entry = (dir + vr[V_SRCN] * 4 + (v->kon_delay ? 0 : 2)) & 0xFFFF;
    int next_addr = ram[entry] | ram[(entry + 1) & 0xFFFF] << 8;
    int header = ram[v->brr_addr];
    int pitch = vr[V_PITCHL] | (vr[V_PITCHH] & 0x3F) << 8;
    if (pmon & vbit) pitch += ((prev_out >> 5) * pitch) >> 10;

    if (v->kon_delay) {
      if (v->kon_delay == 5) {
        v->brr_addr = next_addr;
        v->brr_offset = 1;
        v->buf_pos = 0;
        header = 0;
      }
      v->env = 0;
      v->hidden_env = 0;
      v->interp_pos = 0;
      if (--v->kon_delay & 3) v->interp_pos = 0x4000;
      pitch = 0;
    }

    int s = interpolate(v);
    if (non & vbit) s = (int16_t)(d->noise * 2);
    int o = ((s * v->env) >> 11) & ~1;
    vr[V_ENVX] = (uint8_t)(v->env >> 4);
    vr[V_OUTX] = (uint8_t)(o >> 8);

    if ((r[R_FLG] & 0x80) || (header & 3) == 1) {
      v->env_mode = ENV_RELEASE;
      v->env = 0;
    }
    if (d->every_other) {
      if (d->koff & vbit) v->env_mode = ENV_RELEASE;
      if (d->kon & vbit) {
        v->kon_delay = 5;
        v->env_mode = ENV_ATTACK;
      }
    }
    if (!v->kon_delay) run_envelope(d, v, vr);

    int looped = 0;
    if (v->interp_pos >= 0x4000) {
      decode_brr(v, ram, header);
      if ((v->brr_offset += 2) >= 9) {
        v->brr_addr = (v->brr_addr + 9) & 0xFFFF;
        if (header & 1) {
          v->brr_addr = next_addr;
          looped = vbit;
        }
        v->brr_offset = 1;
      }
    }
    v->interp_pos = (v->interp_pos & 0x3FFF) + pitch;
    if (v->interp_pos > 0x7FFF) v->interp_pos = 0x7FFF;

    for (int ch = 0; ch < 2; ch++) {
      int amp = (o * (int8_t)vr[V_VOLL + ch]) >> 7;
      main_out[ch] = clamp16(main_out[ch] + amp);
      if (eon & vbit) echo_out[ch] = clamp16(echo_out[ch] + amp);
    }

    endx |= looped;
    if (v->kon_delay == 5) endx &= ~vbit;
    prev_out = o;
  }
  r[R_ENDX] = (uint8_t)endx;

  //Echo: read oldest frame at ESA*0x100+offset, FIR, write back with EFB.
  int eaddr = ((r[R_ESA] << 8) + d->echo_offset) & 0xFFFF;
  d->echo_hist_pos = (d->echo_hist_pos + 1) & 7;
  int hp = d->echo_hist_pos;
  int echo_in[2];
  for (int ch = 0; ch < 2; ch++) {
    int a0 = (eaddr + ch * 2) & 0xFFFF;
    int s = (int16_t)(ram[a0] | ram[(a0 + 1) & 0xFFFF] << 8);
    d->echo_hist[hp][ch] = d->echo_hist[hp + 8][ch] = s >> 1;
  }
  for (int ch = 0; ch < 2; ch++) {
    int acc = 0;
    for (int t = 0; t < 7; t++)
      acc += (d->echo_hist[hp + t + 1][ch] * (int8_t)r[R_FIR + t * 0x10]) >> 6;
    acc = (int16_t)acc;
    acc += (int16_t)((d->echo_hist[hp + 8][ch] * (int8_t)r[R_FIR + 0x70]) >> 6);
    echo_in[ch] = clamp16(acc) & ~1;
  }

  for (int ch = 0; ch < 2; ch++) {
    int m = (int16_t)((main_out[ch] * (int8_t)r[R_MVOLL + ch * 0x10]) >> 7);
    int e = (int16_t)((echo_in[ch] * (int8_t)r[R_EVOLL + ch * 0x10]) >> 7);
    int s = clamp16(m + e);
    if (r[R_FLG] & 0x40) s = 0;
    out[ch] = (int16_t)s;
  }

  if (!(r[R_FLG] & 0x20)) {
    for (int ch = 0; ch < 2; ch++) {
      int e = echo_out[ch] + (int16_t)((echo_in[ch] * (int8_t)r[R_EFB]) >> 7);
      e = clamp16(e) & ~1;
      int a0 = (eaddr + ch * 2) & 0xFFFF;
      ram[a0] = (uint8_t)e;
      ram[(a0 + 1) & 0xFFFF] = (uint8_t)(e >> 8);
    }
  }

  //EDL latched only when the offset wraps to 0.
  if (!d->echo_offset) d->echo_length = (r[R_EDL] & 0x0F) * 0x800;
  d->echo_offset += 4;
  if (d->echo_offset >= d->echo_length) d->echo_offset = 0;
}
