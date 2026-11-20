#ifndef ARENA_UI_H
#define ARENA_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"

//Text and pictures for the frontend's own screens (game select, battle
//setup, battle HUD and menus), drawn into an XRGB8888 canvas of PPU_MAXW
//pixels per row with a game's own title font.

#define UI_H 224

typedef struct UiCanvas {
  uint32_t *px;              //XRGB8888, stride pixels per row
  int stride;
  int w, h;                  //visible size
} UiCanvas;

//Font palettes as the title menus use them: plain text and the selected item.
enum { UI_TEXT = 0, UI_SEL = 1, UI_DIM = 2, UI_PAL_COUNT = 3 };

//Takes the BG3 font (tiles are ASCII codes) and its palettes from a machine
//showing the title screen of G2 or G3. Returns false (keeping the previous
//font) when it has none.
bool ui_grab_font(const Snes *title);
bool ui_have_font(void);

int ui_text_w(const char *s, int scale);
//Text with a one-pixel dark shadow, readable over any picture.
void ui_text(UiCanvas *c, int x, int y, const char *s, int pal, int scale);
void ui_text_c(UiCanvas *c, int cx, int y, const char *s, int pal, int scale);
//Darkens rows [y0, y1) to a quarter.
void ui_dim(UiCanvas *c, int y0, int y1);
void ui_dim_rect(UiCanvas *c, int x0, int y0, int x1, int y1, int shift);
void ui_fill(UiCanvas *c, int x0, int y0, int x1, int y1, uint32_t rgb);
uint32_t ui_rgb555(uint16_t c);

#endif
