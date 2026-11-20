//PPU smoke test: register read-back, mode 1 frame with sprites, windows and color math,
//mode 7 frame, timing. Writes PPMs to the directory given as argv[1] (default ".").
#include "../src/snes/ppu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Ppu ppu;
static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void w(uint8_t reg, uint8_t v) { ppu_write(&ppu, reg, v); }
static uint8_t rd(uint8_t reg) { return ppu_read(&ppu, reg, 0xA5); }
static void w16(uint8_t reg, uint16_t v) { w(reg, v); w(reg, v >> 8); }
static void vaddr(uint16_t a) { w(0x16, a); w(0x17, a >> 8); }
static void vword(uint16_t v) { w(0x18, v); w(0x19, v >> 8); }
static void cg(uint8_t i, uint16_t c) { w(0x21, i); w(0x22, c); w(0x22, c >> 8); }
static uint16_t rgb(int r, int g, int b) { return r | g << 5 | b << 10; }

//Planar 2bpp/4bpp tile from 8x8 color indices; VMAIN must increment on high byte, step 1.
static void put_tile(uint16_t addr, int bpp, uint8_t px[8][8]) {
  vaddr(addr);
  for (int pl = 0; pl < bpp; pl += 2)
    for (int y = 0; y < 8; y++) {
      uint8_t lo = 0, hi = 0;
      for (int x = 0; x < 8; x++) {
        lo |= (px[y][x] >> pl & 1) << (7 - x);
        hi |= (px[y][x] >> (pl + 1) & 1) << (7 - x);
      }
      vword(lo | hi << 8);
    }
}

static void test_readback(void) {
  ppu_reset(&ppu);
  w(0x15, 0x80);
  vaddr(0x7000);
  for (int i = 0; i < 4; i++) vword(0x1234 + i * 0x1111);
  //Latch refills from the current address before the increment: first word repeats.
  vaddr(0x7000);
  for (int i = 0; i < 4; i++) {
    uint16_t v = rd(0x39);
    v |= rd(0x3A) << 8;
    uint16_t want = 0x1234 + (i ? i - 1 : 0) * 0x1111;
    CHECK(v == want, "vram inc-high word %d = %04X want %04X", i, v, want);
  }
  w(0x15, 0x00);
  vaddr(0x7000);
  uint8_t lo0 = rd(0x39), lo1 = rd(0x39);
  CHECK(lo0 == 0x34 && lo1 == 0x34, "vram inc-low reads %02X %02X", lo0, lo1);
  CHECK(rd(0x3A) == 0x23, "vram high after two low reads");

  w(0x15, 0x81);
  vaddr(0x7100);
  vword(0xBEEF);
  vword(0xCAFE);
  CHECK(ppu.vram[0x7100 * 2] == 0xEF && ppu.vram[(0x7100 + 32) * 2 + 1] == 0xCA, "vram step 32");

  w(0x15, 0x84);
  vaddr(0x0001);
  vword(0x5A5A);
  CHECK(ppu.vram[0x0008 * 2] == 0x5A, "vram remap mode 1");

  cg(0x10, 0x7FFF);
  cg(0x11, 0x1234);
  w(0x21, 0x10);
  uint8_t a = rd(0x3B), b = rd(0x3B), c = rd(0x3B), d = rd(0x3B);
  CHECK(a == 0xFF && b == 0xFF && c == 0x34 && d == (0x12 | 0x80), "cgram %02X %02X %02X %02X", a, b, c, d);

  w(0x02, 0x00); w(0x03, 0x00);
  w(0x04, 0x11);
  CHECK(ppu.oam[0] == 0x00, "oam low write buffered until odd byte");
  w(0x04, 0x22); w(0x04, 0x33); w(0x04, 0x44);
  w(0x02, 0x00); w(0x03, 0x01);
  w(0x04, 0x9C);
  CHECK(ppu.oam[0x200] == 0x9C, "oam high table direct write");
  w(0x02, 0x00); w(0x03, 0x00);
  uint8_t o[4];
  for (int i = 0; i < 4; i++) o[i] = rd(0x38);
  CHECK(o[0] == 0x11 && o[1] == 0x22 && o[2] == 0x33 && o[3] == 0x44, "oam read %02X %02X %02X %02X", o[0], o[1], o[2], o[3]);
  w(0x02, 0x00); w(0x03, 0x01);
  CHECK(rd(0x38) == 0x9C, "oam high read");

  w16(0x1B, 0x1234);
  w(0x1C, 0xFE);
  int32_t m = rd(0x34);
  m |= rd(0x35) << 8;
  m |= rd(0x36) << 16;
  CHECK(m == ((-0x2468) & 0xFFFFFF), "mult %06X", m);

  ppu.hcount_latch = 0x1AB;
  ppu.vcount_latch = 0x0CD;
  ppu.counter_latched = true;
  uint8_t s = rd(0x3F);
  CHECK((s & 0x4F) == 0x43, "stat78 %02X", s);
  CHECK(!(rd(0x3F) & 0x40), "stat78 latch flag clears");
  uint8_t h0 = rd(0x3C), h1 = rd(0x3C), v0 = rd(0x3D);
  CHECK(h0 == 0xAB && (h1 & 1) == 1 && v0 == 0xCD, "ophct/opvct %02X %02X %02X", h0, h1, v0);
  CHECK((rd(0x3E) & 0x0F) == 1, "stat77 version");
}

static void write_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); fails++; return; }
  fprintf(f, "P6\n%d %d\n255\n", PPU_W, 224);
  for (int y = 0; y < 224; y++)
    for (int x = 0; x < PPU_W; x++) {
      uint32_t c = ppu.frame[y][x];
      uint8_t px[3] = {c >> 16, c >> 8, c};
      fwrite(px, 1, 3, f);
    }
  fclose(f);
}

static void render_frame(void) {
  ppu_frame_start(&ppu);
  for (int l = 1; l <= 224; l++) ppu_render_line(&ppu, l);
  ppu_vblank_start(&ppu);
}

static void setup_mode1(void) {
  ppu_reset(&ppu);
  w(0x15, 0x80);
  uint8_t t[8][8];

  //BG1/BG2 chars at $2000 (4bpp, 16 words each)
  memset(t, 1, sizeof t);
  put_tile(0x2000 + 1 * 16, 4, t);
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) t[y][x] = ((x >> 1) ^ (y >> 1)) & 1 ? 2 : 3;
  put_tile(0x2000 + 2 * 16, 4, t);
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) t[y][x] = 4 + ((x + y) >> 1);
  put_tile(0x2000 + 3 * 16, 4, t);
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) t[y][x] = (x == 0 || y == 0 || x == 7 || y == 7) ? 12 : 0;
  put_tile(0x2000 + 4 * 16, 4, t);

  //BG3 char 1 at $3000 (2bpp): diamond
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) t[y][x] = (abs(x - 3) + abs(y - 3) <= 3) ? 1 + ((x + y) & 1) : 0;
  put_tile(0x3000 + 1 * 8, 2, t);

  //OBJ chars at $4000: 16x16 ball in chars 0,1,16,17; 8x8 arrow in char 2
  for (int q = 0; q < 4; q++) {
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
      int X = x + (q & 1) * 8, Y = y + (q >> 1) * 8;
      int dx = 2 * X - 15, dy = 2 * Y - 15, d = dx * dx + dy * dy;
      t[y][x] = d < 200 ? (d < 60 ? 3 : d < 120 ? 2 : 1) : 0;
    }
    put_tile(0x4000 + ((q & 1) + (q >> 1) * 16) * 16, 4, t);
  }
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) t[y][x] = (x <= y && x <= 7 - y) || (y >= 3 && y <= 4) ? 1 : 0;
  put_tile(0x4000 + 2 * 16, 4, t);

  //BG1 map at $0000: framed pattern, palette 2
  vaddr(0x0000);
  for (int ty = 0; ty < 32; ty++)
    for (int tx = 0; tx < 32; tx++) {
      uint16_t e = 0;
      if (tx >= 1 && tx <= 30 && ty >= 1 && ty <= 26) {
        if (tx == 1 || tx == 30 || ty == 1 || ty == 26) e = 4;
        else e = 1 + ((tx / 4 + ty / 4) % 3);
        if (e == 3 && (tx & 1)) e |= 0x4000;
        if (e == 3 && (ty & 1)) e |= 0x8000;
        e |= 2 << 10;
      }
      vword(e);
    }
  //BG2 map at $0400: red block, palette 1
  vaddr(0x0400);
  for (int ty = 0; ty < 32; ty++)
    for (int tx = 0; tx < 32; tx++)
      vword((tx >= 10 && tx < 22 && ty >= 8 && ty < 18) ? (1 | 1 << 10) : 0);
  //BG3 map at $0800: row of diamonds at top, high priority, palette 1
  vaddr(0x0800);
  for (int ty = 0; ty < 32; ty++)
    for (int tx = 0; tx < 32; tx++)
      vword((ty == 3 && tx >= 4 && tx < 28) ? (1 | 1 << 10 | 0x2000) : 0);

  cg(0, rgb(2, 2, 8));
  cg(1 * 16 + 1, rgb(31, 0, 0));
  cg(2 * 16 + 1, rgb(0, 20, 0));
  cg(2 * 16 + 2, rgb(28, 28, 28));
  cg(2 * 16 + 3, rgb(8, 8, 8));
  for (int i = 0; i < 8; i++) cg(2 * 16 + 4 + i, rgb(4 * i, 31 - 4 * i, 16));
  cg(2 * 16 + 12, rgb(31, 31, 0));
  cg(4 + 1, rgb(0, 31, 31));
  cg(4 + 2, rgb(0, 12, 31));
  for (int pal = 0; pal < 8; pal++) {
    cg(128 + pal * 16 + 1, rgb(31, 10, 31));
    cg(128 + pal * 16 + 2, rgb(20, 0, 20));
    cg(128 + pal * 16 + 3, rgb(31, 31, 31));
  }

  w(0x05, 0x09);         //mode 1, BG3 priority
  w(0x07, 0x00);         //BG1 map $0000 32x32
  w(0x08, 0x04);         //BG2 map $0400
  w(0x09, 0x08);         //BG3 map $0800
  w(0x0B, 0x22);         //BG1/BG2 chr $2000
  w(0x0C, 0x03);         //BG3 chr $3000
  w16(0x0E, 0x3FF);      //BG1 VOFS -1
  w16(0x10, 0x3FF);
  w16(0x12, 0x3FF);
  w(0x01, 0x02);         //OBJ base $4000, 8x8/16x16

  w(0x02, 0); w(0x03, 0);
  uint8_t spr[4][4] = {
    {60, 100, 0x00, 0x30 | 4 << 1},   //palette 4: color math
    {150, 120, 0x00, 0x30 | 0 << 1},  //palette 0: no color math
    {100, 95, 0x00, 0x30 | 4 << 1},   //over red sub-screen block
    {240, 30, 0x02, 0x70},            //8x8 arrow, hflip
  };
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) w(0x04, spr[i][j]);
  for (int i = 4; i < 128; i++) { w(0x04, 0); w(0x04, 240); w(0x04, 0); w(0x04, 0); }
  w(0x02, 0x00); w(0x03, 0x01);
  w(0x04, 0x2A);         //sprites 0-2 large, 3 small
  for (int i = 1; i < 32; i++) w(0x04, 0);

  w(0x26, 200); w(0x27, 230);  //window 1
  w(0x23, 0x02);               //BG1 window 1 enable
  w(0x2C, 0x15);               //TM: BG1 BG3 OBJ
  w(0x2D, 0x02);               //TS: BG2
  w(0x2E, 0x01);               //TMW: BG1
  w(0x30, 0x02);               //math against sub-screen
  w(0x31, 0x51);               //add, half, BG1 + OBJ
  w(0x00, 0x0F);
}

static void setup_mode7(void) {
  ppu_reset(&ppu);
  for (int i = 0; i < 128 * 128; i++) ppu.vram[i * 2] = ((i & 127) / 4 + (i >> 7) / 4) & 1 ? 1 : 2;
  for (int t = 1; t <= 2; t++)
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++)
        ppu.vram[(t * 64 + y * 8 + x) * 2 + 1] = (x == 0 || y == 0) ? 3 : t;
  cg(0, rgb(0, 0, 10));
  cg(1, rgb(31, 16, 0));
  cg(2, rgb(0, 16, 31));
  cg(3, rgb(31, 31, 31));
  //Rotation 30 degrees, scale 1.5: A=D=cos*1.5*256, B=-C=sin*1.5*256
  int16_t ca = 333, sa = 192;
  w(0x05, 0x07);
  w(0x1A, 0x00);
  w16(0x1B, ca); w16(0x1C, sa); w16(0x1D, -sa); w16(0x1E, ca);
  w16(0x1F, 128); w16(0x20, 112);
  w16(0x0D, 0); w16(0x0E, 0);
  w(0x2C, 0x01);
  w(0x00, 0x0F);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  char path[1024];

  test_readback();

  setup_mode1();
  render_frame();
  snprintf(path, sizeof path, "%s/ppu_test.ppm", dir);
  write_ppm(path);

  //Spot checks against expected composition
  uint32_t back = ppu.frame[0][0];
  CHECK(back == 0x101042, "backdrop %06X", back);
  CHECK(ppu.frame[100][210] == back, "TMW window hides BG1 at x=210: %06X", ppu.frame[100][210]);

  clock_t t0 = clock();
  int frames = 500;
  for (int i = 0; i < frames; i++) render_frame();
  double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;
  printf("mode 1 frame: %.3f ms\n", ms);

  setup_mode7();
  render_frame();
  snprintf(path, sizeof path, "%s/ppu_test_m7.ppm", dir);
  write_ppm(path);
  t0 = clock();
  for (int i = 0; i < frames; i++) render_frame();
  ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / frames;
  printf("mode 7 frame: %.3f ms\n", ms);

  printf(fails ? "%d FAILURES\n" : "all checks passed\n", fails);
  return fails != 0;
}
