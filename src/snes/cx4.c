#include "cx4.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

//Cx4 HLE. Offsets below are relative to $6000 (ram[0] = $6000).
//Register block: $7F40.. = ram[0x1F40..].

#define REG(a) (0x1F00 | ((a) & 0xFF))

typedef struct {
  const uint8_t *rom;
  uint32_t size;
  const uint8_t *wram;
} RomView;

int cx4_wide;

static int16_t sin_tab[512];
static bool tabs_ready;

static void init_tabs(void) {
  if (tabs_ready) return;
  for (int i = 0; i < 512; i++) {
    double v = sin(i * 2.0 * M_PI / 512.0) * 32767.0;
    sin_tab[i] = (int16_t)lround(v);
  }
  tabs_ready = true;
}

static inline int16_t c4sin(int a) { return sin_tab[a & 0x1FF]; }
static inline int16_t c4cos(int a) { return sin_tab[(a + 128) & 0x1FF]; }

static inline uint16_t rd16(const Cx4 *c, int o) {
  return (uint16_t)(c->ram[o & 0x1FFF] | c->ram[(o + 1) & 0x1FFF] << 8);
}
static inline uint32_t rd24(const Cx4 *c, int o) {
  return rd16(c, o) | (uint32_t)c->ram[(o + 2) & 0x1FFF] << 16;
}
static inline void wr16(Cx4 *c, int o, uint16_t v) {
  c->ram[o & 0x1FFF] = (uint8_t)v;
  c->ram[(o + 1) & 0x1FFF] = (uint8_t)(v >> 8);
}
static inline void wr24(Cx4 *c, int o, uint32_t v) {
  wr16(c, o, (uint16_t)v);
  c->ram[(o + 2) & 0x1FFF] = (uint8_t)(v >> 16);
}
static inline int32_t sar(int64_t v, int n) { return (int32_t)(v >> n); }

//LoROM bus address -> byte. ROM, plus WRAM for DMA sources.
static inline uint8_t rom8(const RomView *r, uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16);
  if ((bank & 0xFE) == 0x7E) return r->wram[a & 0x1FFFF];
  if (!(bank & 0x40) && (a & 0xFFFF) < 0x2000) return r->wram[a & 0x1FFF];
  uint32_t off = ((a >> 16 & 0x7F) << 15) | (a & 0x7FFF);
  return r->rom[off & (r->size - 1)];
}

void cx4_reset(Cx4 *c) {
  init_tabs();
  memset(c->ram, 0, sizeof(c->ram));
}

uint8_t cx4_read(Cx4 *c, uint16_t addr) {
  uint16_t o = addr & 0x1FFF;
  if (o == 0x1F5E) return 0;  //never busy
  return c->ram[o];
}

//---------------------------------------------------------------- OAM builder

//Sprite objects at $6220, 16 bytes each:
//  +0 x16  +2 y16  +4 attr  +5 tile  +6 attr bits  +7 ptr24 to a piece list
//Piece list: count, then per piece: flags (bit 5 = 16x16, bits 6-7 flip), dx, dy, tile add.
//Count 0 means the object is a single 16x16 sprite at (x, y).
//Output: OAM low table at $6000, high table at $6200, starting at slot [$6626].
static void build_oam(Cx4 *c, const RomView *r) {
  int first = c->ram[0x626];
  for (int i = 0x1FD; i > first * 4; i -= 4) c->ram[i] = 0xE0;
  int nobj = c->ram[0x620];
  if (!nobj) return;
  uint16_t gx = rd16(c, 0x621), gy = rd16(c, 0x623);
  int slot = first;
  int left = 128 - first;
  const int src0 = 0x220;

  for (int n = 0; n < nobj && left > 0; n++) {
    int src = src0 + n * 16;
    int16_t ox = (int16_t)(rd16(c, src) - gx);
    int16_t oy = (int16_t)(rd16(c, src + 2) - gy);
    uint8_t attr = c->ram[src + 4] | c->ram[src + 6];
    uint8_t tile = c->ram[src + 5];
    uint32_t p = rd24(c, src + 7);
    int cnt = rom8(r, p);
    if (cnt == 0) {
      c->ram[slot * 4 + 0] = (uint8_t)ox;
      c->ram[slot * 4 + 1] = (uint8_t)oy;
      c->ram[slot * 4 + 2] = tile;
      c->ram[slot * 4 + 3] = attr;
      int hi = 0x200 + (slot >> 2), sh = (slot & 3) * 2;
      c->ram[hi] = (uint8_t)((c->ram[hi] & ~(3 << sh)) | ((ox & 0x100) ? 3 : 2) << sh);
      slot++;
      left--;
      continue;
    }
    p++;
    for (; cnt > 0 && left > 0; cnt--, p += 4) {
      uint8_t fl = rom8(r, p);
      int big = fl & 0x20;
      int sz = big ? 16 : 8;
      int x = (int8_t)rom8(r, p + 1);
      int y = (int8_t)rom8(r, p + 2);
      if (attr & 0x40) x = -x - sz;
      if (attr & 0x80) y = -y - sz;
      x += ox;
      y += oy;
      if (x < -16 - cx4_wide || x > 272 + cx4_wide || y < -16 || y > 224) continue;
      c->ram[slot * 4 + 0] = (uint8_t)x;
      c->ram[slot * 4 + 1] = (uint8_t)y;
      c->ram[slot * 4 + 2] = (uint8_t)(tile + rom8(r, p + 3));
      c->ram[slot * 4 + 3] = attr ^ (fl & 0xC0);
      int hi = 0x200 + (slot >> 2), sh = (slot & 3) * 2;
      uint8_t bits = (uint8_t)(((x & 0x100) ? 1 : 0) | (big ? 2 : 0));
      c->ram[hi] = (uint8_t)((c->ram[hi] & ~(3 << sh)) | bits << sh);
      slot++;
      left--;
    }
  }
}

//---------------------------------------------------------------- bitmaps

//Plots a 4bpp pixel into SNES planar tiles laid out as rows of 8x8 tiles.
static inline void plot4(Cx4 *c, int idx, uint8_t mask, uint8_t pix) {
  if (idx < 0 || idx + 17 >= 0x1F00) return;
  if (pix & 1) c->ram[idx] |= mask;
  if (pix & 2) c->ram[idx + 1] |= mask;
  if (pix & 4) c->ram[idx + 16] |= mask;
  if (pix & 8) c->ram[idx + 17] |= mask;
}

//Scales and rotates the packed 4bpp bitmap at $6600 (w x h, 2 pixels per
//byte, low nibble first) about (cx, cy) into tiles at $6000.
static void scale_rotate(Cx4 *c, int row_pad) {
  int32_t sx = rd16(c, 0x1F8F), sy = rd16(c, 0x1F92);
  if (sx & 0x8000) sx = 0x7FFF;
  if (sy & 0x8000) sy = 0x7FFF;
  int ang = rd16(c, 0x1F80);
  int16_t ma, mb, mc, md;
  switch (ang) {
  case 0:   ma = (int16_t)sx;  mb = 0;               mc = 0;               md = (int16_t)sy;  break;
  case 128: ma = 0;            mb = (int16_t)-sy;    mc = (int16_t)sx;     md = 0;            break;
  case 256: ma = (int16_t)-sx; mb = 0;               mc = 0;               md = (int16_t)-sy; break;
  case 384: ma = 0;            mb = (int16_t)sy;     mc = (int16_t)-sx;    md = 0;            break;
  default:
    ma = (int16_t)sar((int64_t)c4cos(ang) * sx, 15);
    mb = (int16_t)-sar((int64_t)c4sin(ang) * sy, 15);
    mc = (int16_t)sar((int64_t)c4sin(ang) * sx, 15);
    md = (int16_t)sar((int64_t)c4cos(ang) * sy, 15);
    break;
  }
  int w = c->ram[0x1F89] & ~7, h = c->ram[0x1F8C] & ~7;
  int clear = (w + row_pad / 4) * h / 2;
  if (clear > 0x1F00) clear = 0x1F00;
  memset(c->ram, 0, (size_t)clear);

  int32_t cx = (int16_t)rd16(c, 0x1F83), cy = (int16_t)rd16(c, 0x1F86);
  //12-bit fractions; the matrix entries carry their own.
  int32_t lx = (cx << 12) - cx * ma - cx * mb;
  int32_t ly = (cy << 12) - cy * mc - cy * md;
  int out = 0;
  uint8_t bit = 0x80;
  for (int y = 0; y < h; y++) {
    uint32_t px = (uint32_t)lx, py = (uint32_t)ly;
    for (int x = 0; x < w; x++) {
      uint8_t pix = 0;
      if ((px >> 12) < (uint32_t)w && (py >> 12) < (uint32_t)h) {
        uint32_t a = (py >> 12) * (uint32_t)w + (px >> 12);
        pix = c->ram[(0x600 + (a >> 1)) & 0x1FFF];
        if (a & 1) pix >>= 4;
      }
      plot4(c, out, bit, pix);
      bit >>= 1;
      if (!bit) { bit = 0x80; out += 32; }
      px += (uint32_t)(int32_t)ma;
      py += (uint32_t)(int32_t)mc;
    }
    out += 2 + row_pad;
    if (out & 0x10) out &= ~0x10;
    else out -= w * 4 + row_pad;
    lx += mb;
    ly += md;
  }
}

//Scales the bitmap at $6600 about (cx, cy) with 8.8 steps (break-up effect).
static void disintegrate(Cx4 *c) {
  int w = c->ram[0x1F89], h = c->ram[0x1F8C];
  int32_t cx = (int16_t)rd16(c, 0x1F80), cy = (int16_t)rd16(c, 0x1F83);
  int32_t kx = (int16_t)rd16(c, 0x1F86), ky = (int16_t)rd16(c, 0x1F8F);
  uint32_t x0 = (uint32_t)(-cx * kx + (cx << 8));
  uint32_t y = (uint32_t)(-cy * ky + (cy << 8));
  int clear = w * h / 2;
  if (clear > 0x1F00) clear = 0x1F00;
  memset(c->ram, 0, (size_t)clear);
  int src = 0x600;
  for (int i = 0; i < h; i++, y += (uint32_t)ky) {
    uint32_t x = x0;
    for (int j = 0; j < w; j++, x += (uint32_t)kx) {
      if ((x >> 8) < (uint32_t)w && (y >> 8) < (uint32_t)h && (y >> 8) * (uint32_t)w + (x >> 8) < 0x2000) {
        uint8_t b = c->ram[src & 0x1FFF];
        uint8_t pix = (j & 1) ? b >> 4 : b;
        int idx = (int)((y >> 11) * (uint32_t)w * 4 + (x >> 11) * 32 + ((y >> 8) & 7) * 2);
        plot4(c, idx, (uint8_t)(0x80 >> ((x >> 8) & 7)), pix);
      }
      if (j & 1) src++;
    }
  }
}

//Vertical wave distortion of a 2bpp bitplane pair, wave table at $6B00,
//fill pattern at $6A00.
static void bitplane_wave(Cx4 *c) {
  int dst = 0;
  unsigned wp = c->ram[0x1F83];
  uint16_t m1 = 0xC0C0, m2 = 0x3F3F;
  for (int j = 0; j < 0x10; j++) {
    for (int half = 0; half < 2; half++) {
      do {
        int16_t hgt = (int16_t)(-(int8_t)c->ram[(wp + 0xB00) & 0x1FFF] - 16);
        for (int i = 0; i < 40; i++) {
          int o = dst + (i >> 3) * 0x200 + (i & 7) * 2;
          uint16_t t = rd16(c, o) & m2;
          if (hgt >= 0) t |= m1 & (hgt < 8 ? rd16(c, 0xA00 + hgt * 2) : 0xFF00);
          wr16(c, o, t);
          hgt++;
        }
        wp = (wp + 1) & 0x7F;
        m1 = (uint16_t)((m1 >> 2) | (m1 << 6));
        m2 = (uint16_t)((m2 >> 2) | (m2 << 6));
      } while (m1 != 0xC0C0);
      dst += 16;
    }
  }
}

//---------------------------------------------------------------- wireframe

typedef struct {
  int16_t x, y, z;     //vertex in, projected out
  int16_t rx, ry, rz;  //rotation angles (128 per turn)
  int16_t scale;
  int16_t dist;        //step count from line_setup
} Wire;

//Rotates (x, y, z) by rx, ry, rz and projects with perspective.
static void wire_project(Wire *w) {
  double x = w->x, y = w->y, z = (double)w->z - 0x95;
  double t, x2, y2, z2;
  t = -(double)w->rx * M_PI * 2 / 128;
  y2 = y * cos(t) - z * sin(t);
  z2 = y * sin(t) + z * cos(t);
  t = -(double)w->ry * M_PI * 2 / 128;
  x2 = x * cos(t) + z2 * sin(t);
  z = -x * sin(t) + z2 * cos(t);
  t = -(double)w->rz * M_PI * 2 / 128;
  x = x2 * cos(t) - y2 * sin(t);
  y = x2 * sin(t) + y2 * cos(t);
  double k = (double)w->scale / (0x90 * (z + 0x95)) * 0x95;
  w->x = (int16_t)(x * k);
  w->y = (int16_t)(y * k);
}

//Same rotation, orthographic, scale/256.
static void wire_rotate(Wire *w) {
  double x = w->x, y = w->y, z = w->z;
  double t, x2, y2, z2;
  t = -(double)w->rx * M_PI * 2 / 128;
  y2 = y * cos(t) - z * sin(t);
  z2 = y * sin(t) + z * cos(t);
  t = -(double)w->ry * M_PI * 2 / 128;
  x2 = x * cos(t) + z2 * sin(t);
  t = -(double)w->rz * M_PI * 2 / 128;
  x = x2 * cos(t) - y2 * sin(t);
  y = x2 * sin(t) + y2 * cos(t);
  w->x = (int16_t)(x * w->scale / 0x100);
  w->y = (int16_t)(y * w->scale / 0x100);
}

//Line from (x1, y1) to (x2, y2): returns 8.8 steps in (*dx, *dy) and the
//number of pixels.
static int line_setup(int x1, int y1, int x2, int y2, int16_t *dx, int16_t *dy) {
  int ddx = x2 - x1, ddy = y2 - y1;
  if (abs(ddx) > abs(ddy)) {
    *dy = (int16_t)(256.0 * ddy / abs(ddx));
    *dx = ddx < 0 ? -256 : 256;
    return abs(ddx) + 1;
  }
  if (ddy) {
    *dx = (int16_t)(256.0 * ddx / abs(ddy));
    *dy = ddy < 0 ? -256 : 256;
    return abs(ddy) + 1;
  }
  *dx = (int16_t)ddx;
  *dy = (int16_t)ddy;
  return 0;
}

//Draws one 2bpp line into the 96x96 tile canvas at $6300.
static void draw_line(Cx4 *c, int16_t x1, int16_t y1, int16_t z1, int16_t x2, int16_t y2, int16_t z2, uint8_t color) {
  Wire a = {x1, y1, z1, c->ram[0x1F86], c->ram[0x1F87], c->ram[0x1F88], c->ram[0x1F90], 0};
  Wire b = a;
  b.x = x2; b.y = y2; b.z = z2;
  wire_rotate(&a);
  wire_rotate(&b);
  int32_t px = (a.x + 48) << 8, py = (a.y + 48) << 8;
  int16_t sx, sy;
  int n = line_setup(px >> 8, py >> 8, (b.x + 48), (b.y + 48), &sx, &sy);
  for (int i = n ? n : 1; i > 0; i--) {
    if (px > 0xFF && py > 0xFF && px < 0x6000 && py < 0x6000) {
      int tx = (px >> 8) >> 3, ty = (py >> 8) >> 3;
      int addr = ty * 0xC0 + tx * 16 + ((py >> 8) & 7) * 2;
      uint8_t bit = (uint8_t)(0x80 >> ((px >> 8) & 7));
      if (addr + 0x301 < 0x1F00) {
        c->ram[addr + 0x300] &= (uint8_t)~bit;
        c->ram[addr + 0x301] &= (uint8_t)~bit;
        if (color & 1) c->ram[addr + 0x300] |= bit;
        if (color & 2) c->ram[addr + 0x301] |= bit;
      }
    }
    px += sx;
    py += sy;
  }
}

static void vertex(const RomView *r, uint32_t p, int16_t *x, int16_t *y, int16_t *z) {
  *x = (int16_t)(rom8(r, p) << 8 | rom8(r, p + 1));
  *y = (int16_t)(rom8(r, p + 2) << 8 | rom8(r, p + 3));
  *z = (int16_t)(rom8(r, p + 4) << 8 | rom8(r, p + 5));
}

//Line list at ptr24 [$7F80], 5 bytes each: vertex offset a (BE16), vertex
//offset b (BE16), color. a = $FFFF continues from the previous line's b.
static void draw_wireframe(Cx4 *c, const RomView *r) {
  uint32_t line = rd24(c, 0x1F80);
  uint32_t vbank = (uint32_t)c->ram[0x1F82] << 16;
  for (int i = c->ram[0x295]; i > 0; i--, line += 5) {
    uint32_t pa;
    if (rom8(r, line) == 0xFF && rom8(r, line + 1) == 0xFF) {
      uint32_t t = line - 5;
      while (rom8(r, t + 2) == 0xFF && rom8(r, t + 3) == 0xFF) t -= 5;
      pa = vbank | rom8(r, t + 2) << 8 | rom8(r, t + 3);
    } else {
      pa = vbank | rom8(r, line) << 8 | rom8(r, line + 1);
    }
    uint32_t pb = vbank | rom8(r, line + 2) << 8 | rom8(r, line + 3);
    int16_t x1, y1, z1, x2, y2, z2;
    vertex(r, pa, &x1, &y1, &z1);
    vertex(r, pb, &x2, &y2, &z2);
    draw_line(c, x1, y1, z1, x2, y2, z2, rom8(r, line + 4));
  }
}

//Projects the vertex list at $6000 (16 bytes each: x at +1, y at +5, z at +9)
//and builds line step records at $6600 from the index pairs at $6B02.
static void transform_lines(Cx4 *c) {
  int16_t rx = c->ram[0x1F83], ry = c->ram[0x1F86], rz = c->ram[0x1F89], sc = c->ram[0x1F8C];
  int n = rd16(c, 0x1F80);
  for (int i = 0, p = 0; i < n && p + 10 < 0x600; i++, p += 16) {
    Wire w = {(int16_t)rd16(c, p + 1), (int16_t)rd16(c, p + 5), (int16_t)rd16(c, p + 9), rx, ry, rz, sc, 0};
    wire_project(&w);
    wr16(c, p + 1, (uint16_t)(w.x + 0x80));
    wr16(c, p + 5, (uint16_t)(w.y + 0x50));
  }
  for (int k = 0; k < 2; k++) {
    wr16(c, 0x600 + k * 8, 23);
    wr16(c, 0x602 + k * 8, 0x60);
    wr16(c, 0x605 + k * 8, 0x40);
  }
  int m = rd16(c, 0xB00);
  for (int i = 0; i < m && 0x605 + i * 8 < 0xB00; i++) {
    int a = c->ram[(0xB02 + i * 2) & 0x1FFF] << 4, b = c->ram[(0xB03 + i * 2) & 0x1FFF] << 4;
    int16_t dx, dy;
    int d = line_setup((int16_t)rd16(c, a + 1), (int16_t)rd16(c, a + 5), (int16_t)rd16(c, b + 1),
                       (int16_t)rd16(c, b + 5), &dx, &dy);
    wr16(c, 0x600 + i * 8, (uint16_t)(d ? d : 1));
    wr16(c, 0x602 + i * 8, (uint16_t)dx);
    wr16(c, 0x605 + i * 8, (uint16_t)dy);
  }
}

//---------------------------------------------------------------- math

//Per-scanline left/right edges of a trapezoid ($6800 left, $6900 right).
static void trapezoid(Cx4 *c) {
  int a1 = rd16(c, 0x1F8C) & 0x1FF, a2 = rd16(c, 0x1F8F) & 0x1FF;
  int32_t t1 = c4cos(a1) ? (int32_t)(((int64_t)c4sin(a1) << 16) / c4cos(a1)) : INT32_MIN;
  int32_t t2 = c4cos(a2) ? (int32_t)(((int64_t)c4sin(a2) << 16) / c4cos(a2)) : INT32_MIN;
  int16_t y = (int16_t)(rd16(c, 0x1F83) - rd16(c, 0x1F89));
  int16_t base = (int16_t)(rd16(c, 0x1F86) - rd16(c, 0x1F80));
  for (int j = 0; j < 225; j++, y++) {
    int16_t l = 1, r = 0;
    if (y >= 0) {
      l = (int16_t)(sar((int64_t)t1 * y, 16) + base);
      r = (int16_t)(sar((int64_t)t2 * y, 16) + base + (int16_t)rd16(c, 0x1F93));
      if (l < 0 && r < 0) { l = 1; r = 0; }
      else if (l < 0) l = 0;
      else if (r < 0) r = 0;
      if (l > 255 && r > 255) { l = 255; r = 254; }
      else if (l > 255) l = 255;
      else if (r > 255) r = 255;
    }
    c->ram[0x800 + j] = (uint8_t)l;
    c->ram[0x900 + j] = (uint8_t)r;
  }
}

static void command(Cx4 *c, uint8_t cmd, const RomView *r) {
  if (c->ram[REG(0x4D)] == 0x0E && cmd < 0x40 && (cmd & 3) == 0) {
    c->ram[REG(0x80)] = cmd >> 2;
    return;
  }
  switch (cmd) {
  case 0x00:  //sprite functions
    switch (c->ram[REG(0x4D)]) {
    case 0x00: build_oam(c, r); break;
    case 0x03: scale_rotate(c, 0); break;
    case 0x05: transform_lines(c); break;
    case 0x07: scale_rotate(c, 64); break;
    case 0x08: draw_wireframe(c, r); break;
    case 0x0B: disintegrate(c); break;
    case 0x0C: bitplane_wave(c); break;
    }
    break;
  case 0x01:  //clear the canvas and draw a wireframe
    memset(c->ram + 0x300, 0, 16 * 12 * 3 * 4);
    draw_wireframe(c, r);
    break;
  case 0x05: {  //propulsion
    int32_t t = 0x10000;
    uint16_t d = rd16(c, 0x1F83);
    if (d) t = sar((int64_t)(t / d) * rd16(c, 0x1F81), 8);
    wr16(c, 0x1F80, (uint16_t)t);
    break;
  }
  case 0x0D: {  //scale vector (x, y) to length
    int16_t x = (int16_t)rd16(c, 0x1F80), y = (int16_t)rd16(c, 0x1F83);
    double len = sqrt((double)x * x + (double)y * y);
    double k = len > 0 ? (int16_t)rd16(c, 0x1F86) / len : 0;
    wr16(c, 0x1F89, (uint16_t)(int16_t)(x * k * 0.98));
    wr16(c, 0x1F8C, (uint16_t)(int16_t)(y * k * 0.99));
    break;
  }
  case 0x10: {  //polar -> rectangular, 16-bit radius
    int32_t rad = (int16_t)rd16(c, 0x1F83);
    int a = rd16(c, 0x1F80);
    int32_t t = sar((int64_t)rad * c4cos(a) * 2, 16);
    wr24(c, 0x1F86, (uint32_t)t);
    t = sar((int64_t)rad * c4sin(a) * 2, 16);
    wr24(c, 0x1F89, (uint32_t)(t - sar(t, 6)));
    break;
  }
  case 0x13: {  //polar -> rectangular, 8.8 result
    int32_t rad = (int16_t)rd16(c, 0x1F83);
    int a = rd16(c, 0x1F80);
    int32_t t = sar((int64_t)rad * c4cos(a) * 2, 8);
    wr24(c, 0x1F86, (uint32_t)t);
    t = sar((int64_t)rad * c4sin(a) * 2, 8);
    wr24(c, 0x1F89, (uint32_t)t);
    break;
  }
  case 0x15: {  //distance
    int16_t x = (int16_t)rd16(c, 0x1F80), y = (int16_t)rd16(c, 0x1F83);
    wr16(c, 0x1F80, (uint16_t)(int16_t)sqrt((double)x * x + (double)y * y));
    break;
  }
  case 0x1F: {  //atan2, 512 steps per turn
    int16_t x = (int16_t)rd16(c, 0x1F80), y = (int16_t)rd16(c, 0x1F83);
    int16_t ang;
    if (x == 0) ang = y > 0 ? 0x80 : 0x180;
    else {
      ang = (int16_t)(atan((double)y / x) / (M_PI * 2) * 512);
      if (x < 0) ang += 0x100;
      ang &= 0x1FF;
    }
    wr16(c, 0x1F86, (uint16_t)ang);
    break;
  }
  case 0x22: trapezoid(c); break;
  case 0x25: {  //24x24 multiply, low 24 bits
    int64_t a = rd24(c, 0x1F80), b = rd24(c, 0x1F83);
    wr24(c, 0x1F80, (uint32_t)(a * b));
    break;
  }
  case 0x2D: {  //rotate + scale a point
    Wire w = {(int16_t)rd16(c, 0x1F81), (int16_t)rd16(c, 0x1F84), (int16_t)rd16(c, 0x1F87),
              c->ram[0x1F89], c->ram[0x1F8A], c->ram[0x1F8B], (int16_t)rd16(c, 0x1F90), 0};
    wire_rotate(&w);
    wr16(c, 0x1F80, (uint16_t)w.x);
    wr16(c, 0x1F83, (uint16_t)w.y);
    break;
  }
  case 0x40: {  //checksum of $6000-$67FF
    uint16_t sum = 0;
    for (int i = 0; i < 0x800; i++) sum = (uint16_t)(sum + c->ram[i]);
    wr16(c, 0x1F80, sum);
    break;
  }
  case 0x54: {  //square, 48-bit result
    int64_t a = (int64_t)((uint64_t)rd24(c, 0x1F80) << 40) >> 40;
    a *= a;
    wr24(c, 0x1F83, (uint32_t)a);
    wr24(c, 0x1F86, (uint32_t)(a >> 24));
    break;
  }
  case 0x5C: {  //self-test: the chip's 16 immediate register constants, 24-bit each, to $6000
    static const uint32_t imm[16] = {
      0x000000, 0xFFFFFF, 0x00FF00, 0xFF0000, 0x00FFFF, 0xFFFF00, 0x800000, 0x7FFFFF,
      0x008000, 0x007FFF, 0xFF7FFF, 0xFFFF7F, 0x010000, 0xFEFFFF, 0x000100, 0x00FEFF,
    };
    for (int i = 0; i < 16; i++) wr24(c, i * 3, imm[i]);
    break;
  }
  case 0x89:  //chip version
    c->ram[REG(0x80)] = 0x36;
    c->ram[REG(0x81)] = 0x43;
    c->ram[REG(0x82)] = 0x05;
    break;
  }
}

void cx4_write(Cx4 *c, uint16_t addr, uint8_t v, const uint8_t *rom, uint32_t rom_size, const uint8_t *wram) {
  uint16_t o = addr & 0x1FFF;
  c->ram[o] = v;
  RomView r = {rom, rom_size, wram};
  if (o == REG(0x4F)) {
    command(c, v, &r);
  } else if (o == REG(0x47)) {
    uint32_t src = rd24(c, REG(0x40));
    uint32_t len = rd16(c, REG(0x43));
    uint32_t dst = rd16(c, REG(0x45)) & 0x1FFF;
    //Source and destination may overlap; the source is ROM here, so a plain
    //byte loop is enough.
    for (uint32_t i = 0; i < len && dst + i < 0x2000; i++)
      c->ram[dst + i] = rom8(&r, src + i);
  }
}
