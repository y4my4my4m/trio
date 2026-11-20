#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/snes/cx4.h"

//Cx4 HLE checks against values the game's own boot self-test expects,
//plus the OAM builder on a synthetic object list.

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t rom[0x10000];
static uint8_t wram[0x20000];

static void w(Cx4 *c, uint16_t a, uint8_t v) { cx4_write(c, a, v, rom, sizeof(rom), wram); }

static void cmd(Cx4 *c, uint8_t sub, uint8_t op) {
  w(c, 0x7F4D, sub);
  w(c, 0x7F4F, op);
}

int main(void) {
  static Cx4 c;
  cx4_reset(&c);

  cmd(&c, 0x0E, 0x89);
  CHECK(cx4_read(&c, 0x7F80) == 0x36 && cx4_read(&c, 0x7F81) == 0x43 && cx4_read(&c, 0x7F82) == 0x05, "version");
  CHECK(cx4_read(&c, 0x7F5E) == 0, "busy flag");

  cmd(&c, 0x0E, 0x5C);
  CHECK(c.ram[0] == 0 && c.ram[3] == 0xFF && c.ram[0x2F] == 0x00 && c.ram[0x2E] == 0xFE, "immediate registers");

  //Square of -3 (24-bit) = 9, 48-bit.
  w(&c, 0x7F80, 0xFD); w(&c, 0x7F81, 0xFF); w(&c, 0x7F82, 0xFF);
  cmd(&c, 0x0E, 0x54);
  CHECK(c.ram[0x1F83] == 9 && c.ram[0x1F84] == 0 && c.ram[0x1F88] == 0, "square");

  //Distance (3, 4) = 5.
  w(&c, 0x7F80, 3); w(&c, 0x7F81, 0); w(&c, 0x7F83, 4); w(&c, 0x7F84, 0);
  cmd(&c, 0x00, 0x15);
  CHECK(c.ram[0x1F80] == 5 && c.ram[0x1F81] == 0, "distance");

  //atan2(0, 1) = 0, atan2(1, 0) = 0x80 (quarter turn of 512).
  w(&c, 0x7F80, 1); w(&c, 0x7F81, 0); w(&c, 0x7F83, 0); w(&c, 0x7F84, 0);
  cmd(&c, 0x00, 0x1F);
  CHECK(c.ram[0x1F86] == 0 && c.ram[0x1F87] == 0, "atan 0");
  w(&c, 0x7F80, 0); w(&c, 0x7F83, 1);
  cmd(&c, 0x00, 0x1F);
  CHECK(c.ram[0x1F86] == 0x80, "atan 90");

  //DMA from ROM bank $00 ($8000 = rom[0]) into RAM $6600.
  for (int i = 0; i < 16; i++) rom[i] = (uint8_t)(0xA0 + i);
  w(&c, 0x7F40, 0x00); w(&c, 0x7F41, 0x80); w(&c, 0x7F42, 0x00);
  w(&c, 0x7F43, 16); w(&c, 0x7F44, 0);
  w(&c, 0x7F45, 0x00); w(&c, 0x7F46, 0x06);
  w(&c, 0x7F47, 0);
  CHECK(c.ram[0x600] == 0xA0 && c.ram[0x60F] == 0xAF, "dma");

  //OAM builder: one object at (100, 50), global (0, 0), two 8x8 pieces.
  memset(c.ram, 0, 0x800);
  uint8_t *pl = rom + 0x100;  //$00:8100
  pl[0] = 2;
  pl[1] = 0x00; pl[2] = 0; pl[3] = 0; pl[4] = 1;           //+0,+0 tile+1
  pl[5] = 0x20; pl[6] = 8; pl[7] = (uint8_t)-8; pl[8] = 2; //16x16 at +8,-8 tile+2
  c.ram[0x620] = 1;
  c.ram[0x626] = 0;
  uint8_t *o = c.ram + 0x220;
  o[0] = 100; o[2] = 50; o[4] = 0x30; o[5] = 0x10; o[6] = 0;
  o[7] = 0x00; o[8] = 0x81; o[9] = 0x00;
  cmd(&c, 0x00, 0x00);
  CHECK(c.ram[0] == 100 && c.ram[1] == 50 && c.ram[2] == 0x11 && c.ram[3] == 0x30, "oam piece 0");
  CHECK(c.ram[4] == 108 && c.ram[5] == 42 && c.ram[6] == 0x12, "oam piece 1");
  CHECK((c.ram[0x200] & 0x0F) == 0x08, "oam high bits %02X", c.ram[0x200]);
  CHECK(c.ram[9] == 0xE0, "unused slots hidden");

  //Horizontal flip mirrors the piece offsets about the object.
  o[4] = 0x70;
  cmd(&c, 0x00, 0x00);
  CHECK(c.ram[0] == 100 - 8 && c.ram[4] == (uint8_t)(100 - 8 - 16), "oam hflip %d %d", c.ram[0], c.ram[4]);

  if (!fails) printf("cx4: all tests passed\n");
  return fails ? 1 : 0;
}
