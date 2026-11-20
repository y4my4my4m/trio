//APU tests: IPL handshake, IPL upload + SPC700 arithmetic, DSP voice output.
//Usage: apu_test [out.wav]

#include "../src/snes/apu.h"
#include "../src/snes/dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Apu apu;
static int failures;

#define CHECK(cond, ...) do { \
  if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void run(Apu *a, uint64_t n) { apu_run(a, a->cycles + n); }

static int wait_port(Apu *a, int port, uint8_t v, uint64_t limit) {
  uint64_t end = a->cycles + limit;
  while (a->cycles < end) {
    if (apu_cpu_read(a, port) == v) return 1;
    run(a, 32);
  }
  return apu_cpu_read(a, port) == v;
}

//S-CPU side of the IPL transfer protocol: one block, then jump to exec.
static int ipl_upload(Apu *a, uint16_t dst, const uint8_t *data, int n, uint16_t exec) {
  if (!wait_port(a, 0, 0xAA, 100000) || !wait_port(a, 1, 0xBB, 1000)) return 0;
  apu_cpu_write(a, 1, 1);
  apu_cpu_write(a, 2, (uint8_t)dst);
  apu_cpu_write(a, 3, (uint8_t)(dst >> 8));
  apu_cpu_write(a, 0, 0xCC);
  if (!wait_port(a, 0, 0xCC, 10000)) return 0;
  for (int i = 0; i < n; i++) {
    apu_cpu_write(a, 1, data[i]);
    apu_cpu_write(a, 0, (uint8_t)i);
    if (!wait_port(a, 0, (uint8_t)i, 10000)) return 0;
  }
  uint8_t kick = (uint8_t)(n + 1);
  if (!kick) kick = 2;
  apu_cpu_write(a, 2, (uint8_t)exec);
  apu_cpu_write(a, 3, (uint8_t)(exec >> 8));
  apu_cpu_write(a, 1, 0);
  apu_cpu_write(a, 0, kick);
  return wait_port(a, 0, kick, 10000);
}

static void test_ipl_boot(void) {
  apu_reset(&apu);
  CHECK(apu.pc == 0xFFC0, "reset pc %04X", apu.pc);
  CHECK(apu.control == 0xB0, "control %02X", apu.control);
  CHECK(wait_port(&apu, 0, 0xAA, 100000), "port0 %02X", apu_cpu_read(&apu, 0));
  CHECK(wait_port(&apu, 1, 0xBB, 100), "port1 %02X", apu_cpu_read(&apu, 1));
  printf("ipl boot: $AA/$BB after %llu cycles\n", (unsigned long long)apu.cycles);
}

static const uint8_t arith_prog[] = {
  //Phase 1: DIV, MUL, DAA.
  0xE8, 0xE8,        //mov a,#$E8
  0x8D, 0x03,        //mov y,#$03
  0xCD, 0x07,        //mov x,#$07
  0x9E,              //div ya,x        1000/7 -> A=$8E Y=$06
  0xC4, 0xF4,        //mov $F4,a
  0xCB, 0xF5,        //mov $F5,y
  0xE8, 0x0C,        //mov a,#$0C
  0x8D, 0x0B,        //mov y,#$0B
  0xCF,              //mul ya          -> $0084
  0xC4, 0xF6,        //mov $F6,a
  0x60,              //clrc
  0xE8, 0x19,        //mov a,#$19
  0x88, 0x28,        //adc a,#$28      -> $41, H
  0xDF,              //daa             -> $47
  0xC4, 0xF7,        //mov $F7,a
  0x78, 0x01, 0xF4,  //cmp $F4,#$01
  0xD0, 0xFB,        //bne -5
  //Phase 2: ADDW, SUBW.
  0x8F, 0x20, 0x10,  //mov $10,#$20
  0x8F, 0x0E, 0x11,  //mov $11,#$0E
  0xE8, 0xF0,        //mov a,#$F0
  0x8D, 0x12,        //mov y,#$12
  0x7A, 0x10,        //addw ya,$10     $12F0+$0E20 -> $2110, H
  0x0D,              //push psw
  0xC4, 0xF4,        //mov $F4,a
  0xCB, 0xF5,        //mov $F5,y
  0xAE,              //pop a
  0xC4, 0xF6,        //mov $F6,a
  0x9A, 0x10,        //subw ya,$10     -> $12F0
  0xCB, 0xF7,        //mov $F7,y
  0x78, 0x02, 0xF4,  //cmp $F4,#$02
  0xD0, 0xFB,        //bne -5
  //Phase 3: DIV overflow, TSET1.
  0xE8, 0xFF,        //mov a,#$FF
  0x8D, 0xFF,        //mov y,#$FF
  0xCD, 0x01,        //mov x,#$01
  0x9E,              //div ya,x        -> A=$01 Y=$FE, V H
  0x0D,              //push psw
  0xC4, 0xF4,        //mov $F4,a
  0xCB, 0xF5,        //mov $F5,y
  0xAE,              //pop a
  0xC4, 0xF6,        //mov $F6,a
  0x8F, 0x0F, 0x20,  //mov $20,#$0F
  0xE8, 0x30,        //mov a,#$30
  0x0E, 0x20, 0x00,  //tset1 !$0020    -> $3F
  0xE4, 0x20,        //mov a,$20
  0xC4, 0xF7,        //mov $F7,a
  0x2F, 0xFE,        //bra $
};

static void expect_ports(const char *tag, uint8_t p0, uint8_t p1, uint8_t p2mask,
                         uint8_t p2, uint8_t p3) {
  uint8_t o[4];
  for (int i = 0; i < 4; i++) o[i] = apu_cpu_read(&apu, i);
  printf("%s: ports %02X %02X %02X %02X\n", tag, o[0], o[1], o[2], o[3]);
  CHECK(o[0] == p0, "%s port0 %02X want %02X", tag, o[0], p0);
  CHECK(o[1] == p1, "%s port1 %02X want %02X", tag, o[1], p1);
  CHECK((o[2] & p2mask) == p2, "%s port2 %02X&%02X want %02X", tag, o[2], p2mask, p2);
  CHECK(o[3] == p3, "%s port3 %02X want %02X", tag, o[3], p3);
}

static void test_arith(void) {
  apu_reset(&apu);
  int ok = ipl_upload(&apu, 0x0200, arith_prog, (int)sizeof arith_prog, 0x0200);
  CHECK(ok, "ipl upload");
  if (!ok) return;
  run(&apu, 2000);
  expect_ports("div/mul/daa", 0x8E, 0x06, 0xFF, 0x84, 0x47);
  apu_cpu_write(&apu, 0, 1);
  run(&apu, 2000);
  //NVHZC mask: ADDW leaves only H set.
  expect_ports("addw/subw", 0x10, 0x21, 0xCB, 0x08, 0x12);
  apu_cpu_write(&apu, 0, 2);
  run(&apu, 2000);
  expect_ports("div ovf/tset1", 0x01, 0xFE, 0xCA, 0x48, 0x3F);  //NVHZ; C carries over
}

//DSP register writes as SPC700 code: mov $F2,#reg / mov $F3,#val.
static int emit_dsp(uint8_t *p, uint8_t reg, uint8_t val) {
  p[0] = 0x8F; p[1] = reg; p[2] = 0xF2;
  p[3] = 0x8F; p[4] = val; p[5] = 0xF3;
  return 6;
}

static void write_wav(const char *path, const int16_t *s, int frames) {
  FILE *f = fopen(path, "wb");
  if (!f) { printf("cannot write %s\n", path); return; }
  uint32_t data = (uint32_t)frames * 4;
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  uint32_t v = 36 + data;
  for (int i = 0; i < 4; i++) h[4 + i] = (uint8_t)(v >> (8 * i));
  memcpy(h + 8, "WAVEfmt ", 8);
  const uint32_t fmt[] = { 16, 1 | 2u << 16, APU_SAMPLE_RATE, APU_SAMPLE_RATE * 4, 4 | 16u << 16 };
  for (int k = 0; k < 5; k++)
    for (int i = 0; i < 4; i++) h[16 + k * 4 + i] = (uint8_t)(fmt[k] >> (8 * i));
  memcpy(h + 36, "data", 4);
  for (int i = 0; i < 4; i++) h[40 + i] = (uint8_t)(data >> (8 * i));
  fwrite(h, 1, 44, f);
  for (int i = 0; i < frames * 2; i++) {
    uint8_t b[2] = { (uint8_t)s[i], (uint8_t)(s[i] >> 8) };
    fwrite(b, 1, 2, f);
  }
  fclose(f);
}

static void test_dsp(const char *wav) {
  //Image at $0200: program, directory at $0300, BRR at $0400.
  static uint8_t img[0x213];
  memset(img, 0, sizeof img);
  uint8_t *p = img;
  static const uint8_t regs[][2] = {
    { 0x6C, 0x20 },  //FLG: no reset, no mute, echo writes off
    { 0x5D, 0x03 },  //DIR $0300
    { 0x0C, 0x7F }, { 0x1C, 0x7F },  //MVOL
    { 0x2C, 0x00 }, { 0x3C, 0x00 },  //EVOL
    { 0x4D, 0x00 }, { 0x3D, 0x00 }, { 0x2D, 0x00 }, { 0x5C, 0x00 },
    { 0x00, 0x7F }, { 0x01, 0x7F },  //V0 VOL
    { 0x02, 0x00 }, { 0x03, 0x08 },  //V0 pitch $0800 = 16 kHz
    { 0x04, 0x00 },                  //V0 SRCN
    { 0x05, 0x00 }, { 0x07, 0x7F },  //GAIN direct $7F
    { 0x4C, 0x01 },                  //KON V0
  };
  for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++)
    p += emit_dsp(p, regs[i][0], regs[i][1]);
  *p++ = 0x2F; *p++ = 0xFE;

  //Directory entry 0: start $0400, loop $0400.
  uint8_t *dir = img + 0x100;
  dir[0] = 0x00; dir[1] = 0x04; dir[2] = 0x00; dir[3] = 0x04;
  //Two blocks, shift 11, filter 0: 16 x +7 then 16 x -7, end+loop.
  uint8_t *brr = img + 0x200;
  brr[0] = 0xB0;
  memset(brr + 1, 0x77, 8);
  brr[9] = 0xB3;
  memset(brr + 10, 0x99, 8);

  apu_reset(&apu);
  int ok = ipl_upload(&apu, 0x0200, img, (int)sizeof img, 0x0200);
  CHECK(ok, "ipl upload (dsp)");
  if (!ok) return;

  apu.ring_r = apu.ring_w;
  enum { FRAMES = APU_SAMPLE_RATE / 2 };
  static int16_t buf[FRAMES * 2];
  int got = 0;
  while (got < FRAMES) {
    run(&apu, 32 * 1024);
    got += apu_pop_samples(&apu, buf + got * 2, FRAMES - got);
  }

  int nonzero = 0, peak = 0;
  for (int i = 0; i < FRAMES * 2; i++) {
    if (buf[i]) nonzero++;
    int m = abs(buf[i]);
    if (m > peak) peak = m;
  }
  CHECK(nonzero > FRAMES, "nonzero samples %d", nonzero);

  //Rising zero crossings over the second half, left channel.
  int first = -1, last = -1, count = 0;
  for (int i = FRAMES / 2 + 1; i < FRAMES; i++) {
    if (buf[(i - 1) * 2] < 0 && buf[i * 2] >= 0) {
      if (first < 0) first = i;
      else count++;
      last = i;
    }
  }
  double period = count ? (double)(last - first) / count : 0;
  printf("dsp: nonzero %d/%d peak %d period %.2f samples (%.1f Hz), want 64 (500 Hz)\n",
         nonzero, FRAMES * 2, peak, period, period ? APU_SAMPLE_RATE / period : 0);
  CHECK(period > 62 && period < 66, "period %.2f", period);
  CHECK(peak > 8000, "peak %d", peak);

  write_wav(wav, buf, FRAMES);
  printf("wrote %s\n", wav);
}

//Continues from test_dsp state: ADSR sustain, echo buffer writes, KOFF release.
static void test_adsr_echo(void) {
  void *d = apu.dsp_opaque;
  static int16_t tmp[APU_RING * 2];
  dsp_write(d, 0x05, 0xFF);  //ADSR on, attack 15, decay 7
  dsp_write(d, 0x06, 0x40);  //sustain level 2, sustain rate 0
  dsp_write(d, 0x6D, 0x60);  //ESA $6000
  dsp_write(d, 0x7D, 0x02);  //EDL 2 = 4 KiB
  dsp_write(d, 0x0F, 0x7F);  //FIR0
  dsp_write(d, 0x0D, 0x40);  //EFB
  dsp_write(d, 0x2C, 0x40);
  dsp_write(d, 0x3C, 0x40);
  dsp_write(d, 0x4D, 0x01);  //EON V0
  dsp_write(d, 0x6C, 0x00);  //FLG: echo writes on
  dsp_write(d, 0x4C, 0x01);  //KON V0
  run(&apu, 32 * 16);
  int peak_envx = 0;
  for (int i = 0; i < 400; i++) {
    run(&apu, 32);
    int e = dsp_read(d, 0x08);
    if (e > peak_envx) peak_envx = e;
  }
  run(&apu, 32 * 8000);
  apu_pop_samples(&apu, tmp, APU_RING);
  int envx = dsp_read(d, 0x08);
  int echo_nz = 0;
  for (int i = 0x6000; i < 0x7000; i++) echo_nz += apu.ram[i] != 0;
  printf("adsr: peak envx %02X sustain envx %02X; echo buffer nonzero bytes %d\n",
         peak_envx, envx, echo_nz);
  CHECK(peak_envx >= 0x78, "attack peak %02X", peak_envx);
  //Decay stops at (SL+1)*0x100: sustain compare uses the pending step.
  CHECK(envx == 0x30, "sustain envx %02X", envx);
  CHECK(echo_nz > 1024, "echo buffer %d", echo_nz);
  dsp_write(d, 0x5C, 0x01);  //KOFF V0
  run(&apu, 32 * 300);
  CHECK(dsp_read(d, 0x08) == 0, "release envx %02X", dsp_read(d, 0x08));
  CHECK(dsp_read(d, 0x7C) & 1, "ENDX bit0 after loop");
  dsp_write(d, 0x7C, 0x00);
  CHECK(dsp_read(d, 0x7C) == 0, "ENDX clear on write");
}

int main(int argc, char **argv) {
  const char *wav = argc > 1 ? argv[1] : "apu_test.wav";
  test_ipl_boot();
  test_arith();
  test_dsp(wav);
  test_adsr_echo();
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures != 0;
}
