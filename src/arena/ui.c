#include "ui.h"
#include <string.h>

static uint8_t glyph[96][8][8];
static uint32_t font_pal[UI_PAL_COUNT][4];
static bool have_font;

uint32_t ui_rgb555(uint16_t c) {
  uint32_t r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
  return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

bool ui_grab_font(const Snes *title) {
  if (!title || (title->game != SNES_GAME_G2 && title->game != SNES_GAME_G3)) return false;
  PpuBgInfo bi;
  ppu_bg_info(&title->ppu, 2, &bi);
  for (int t = 0; t < 96; t++)
    for (int y = 0; y < 8; y++) {
      uint32_t a = ((uint32_t)bi.chr_base * 2 + (uint32_t)(t + 32) * 16 + (uint32_t)y * 2) & 0xFFFF;
      uint8_t p0 = title->ppu.vram[a], p1 = title->ppu.vram[a + 1];
      for (int x = 0; x < 8; x++) glyph[t][y][x] = (uint8_t)((p0 >> (7 - x) & 1) | (p1 >> (7 - x) & 1) << 1);
    }
  for (int p = 0; p < 2; p++)
    for (int i = 0; i < 4; i++) font_pal[p][i] = ui_rgb555(title->ppu.cgram[p * 4 + i]);
  //Dimmed: the plain palette at half brightness.
  for (int i = 0; i < 4; i++) font_pal[UI_DIM][i] = (font_pal[UI_TEXT][i] >> 1) & 0x7F7F7F;
  have_font = true;
  return true;
}

bool ui_have_font(void) { return have_font; }

int ui_text_w(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

static void glyphs(UiCanvas *c, int x, int y, const char *s, int pal, int scale, bool shadow) {
  for (; *s; s++, x += 8 * scale) {
    int ch = (unsigned char)*s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 32 || ch >= 128 || ch == ' ') continue;
    for (int gy = 0; gy < 8 * scale; gy++)
      for (int gx = 0; gx < 8 * scale; gx++) {
        int X = x + gx, Y = y + gy;
        if (X < 0 || X >= c->w || Y < 0 || Y >= c->h) continue;
        int v;
        if (have_font) v = glyph[ch - 32][gy / scale][gx / scale];
        else v = (gx / scale > 0 && gx / scale < 7 && gy / scale > 0 && gy / scale < 7) ? 3 : 0;  //no font: blocks
        if (v) c->px[Y * c->stride + X] = shadow ? 0 : (have_font ? font_pal[pal][v] : 0xFFFFFF);
      }
  }
}

void ui_text(UiCanvas *c, int x, int y, const char *s, int pal, int scale) {
  if (pal < 0 || pal >= UI_PAL_COUNT) pal = UI_TEXT;
  glyphs(c, x + 1, y + 1, s, pal, scale, true);
  glyphs(c, x, y, s, pal, scale, false);
}

void ui_text_c(UiCanvas *c, int cx, int y, const char *s, int pal, int scale) {
  ui_text(c, cx - ui_text_w(s, scale) / 2, y, s, pal, scale);
}

void ui_dim_rect(UiCanvas *c, int x0, int y0, int x1, int y1, int shift) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > c->w) x1 = c->w;
  if (y1 > c->h) y1 = c->h;
  uint32_t mask = (0xFFu >> shift) * 0x010101u;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) c->px[y * c->stride + x] = (c->px[y * c->stride + x] >> shift) & mask;
}

void ui_dim(UiCanvas *c, int y0, int y1) { ui_dim_rect(c, 0, y0, c->w, y1, 2); }

void ui_fill(UiCanvas *c, int x0, int y0, int x1, int y1, uint32_t rgb) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > c->w) x1 = c->w;
  if (y1 > c->h) y1 = c->h;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) c->px[y * c->stride + x] = rgb;
}
