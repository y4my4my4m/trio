#include <string.h>
#include "game/recomp_rt.h"
#include "port/port_api.h"

//PORT OPTIONS in the title OPTION MODE screen of each game (docs/MENUS.md).
//
//The three games run the same option mode loop. G3 (00:EA2F): cursor
//$7E:FF80 (0-5 key config, 6 sound, 7 exit), previous cursor $7E:FF81; the
//new presses are in two bytes (B Y Select Start Up Down Left Right, then
//A X L R). Up/Down move the cursor with wraparound; a change redraws the
//old item plain and the new one highlighted through a string table; then
//the item's handler runs from a jump table. G2 is the same at other
//addresses; G1 has ten items (6 sound mode, 7 BGM test, 8 S.E. test, 9 exit).
//
//An item is added before EXIT. Its row and the options page are written
//straight into the BG3 tilemap (VRAM $0800, 32 x 64, font tiles are ASCII
//codes in G2 and G3). While the page is open the ROM loop only yields.

typedef struct OptGame {
  uint8_t pad_hi, pad_lo;      //direct page: new presses (B Y Sel Start U D L R) / (A X L R)
  int item_before, item_exit, item_port;   //the entry goes between these two; its own index
  uint16_t pc_moved;           //after the ROM's cursor move and wraparound
  uint16_t pc_new;             //the highlighted redraw of the new item (skip target of the old one)
  uint16_t pc_yield;           //wait a frame, back to the loop head
  int row_entry, col_entry;
  uint16_t a_text, a_sel, a_value, a_frame;
  bool box;                    //the key config box tiles ($AA-$AD) are there
} OptGame;

static const OptGame games[4] = {
  [SNES_GAME_G1] = {0xAC, 0xAB, 8, 9, 10, 0xEAF4, 0xEB1B, 0xEB39, 25, 10, 0x2000, 0x2C00, 0x2000, 0x2C00, false},
  [SNES_GAME_G2] = {0xAD, 0xAC, 6, 7, 8, 0xEB06, 0xEB30, 0xEB4E, 25, 10, 0x2000, 0x2800, 0x3400, 0x2C00, true},
  [SNES_GAME_G3] = {0xAD, 0xAC, 6, 7, 8, 0xEA7D, 0xEAA7, 0xEB7D, 25, 10, 0x2000, 0x2800, 0x3400, 0x2C00, true},
};

#define CUR 0x0FF80        //$7E:FF80
#define PREV 0x0FF81       //$7E:FF81
#define BG3 0x0800         //tilemap word address

static const OptGame *G;   //the game of the machine being run
#define ITEM_PORT (G->item_port)
#define A_TEXT (G->a_text)
#define A_SEL (G->a_sel)
#define A_VALUE (G->a_value)
#define A_FRAME (G->a_frame)
#define ROW_ENTRY (G->row_entry)
#define COL_ENTRY (G->col_entry)
#define PC_YIELD (G->pc_yield)

enum {
  PI_WIDE, PI_WIDE_HUD, PI_ASPECT, PI_FLICKER, PI_CHARGE, PI_TURBO, PI_REWIND, PI_AUTOSAVE,
  PI_SCANLINES, PI_FILTER, PI_HEALTH, PI_LIVES, PI_MORE, PI_BACK, PI_COUNT
};

static bool g_open;
static int g_sel;
static uint16_t g_saved[32 * 28];

static Settings *cfg(void) {
  static Settings fallback;
  static bool init;
  if (g_port.settings) return g_port.settings;
  if (!init) { settings_default(&fallback); init = true; }
  return &fallback;
}

static void put(Snes *s, int row, int col, uint16_t tile) {
  if (row < 0 || row >= 32 || col < 0 || col >= 32) return;
  uint32_t a = (uint32_t)(BG3 + row * 32 + col) * 2;
  s->ppu.vram[a] = (uint8_t)tile;
  s->ppu.vram[a + 1] = (uint8_t)(tile >> 8);
}

static uint16_t get(const Snes *s, int row, int col) {
  uint32_t a = (uint32_t)(BG3 + row * 32 + col) * 2;
  return (uint16_t)(s->ppu.vram[a] | s->ppu.vram[a + 1] << 8);
}

static void text(Snes *s, int row, int col, const char *str, uint16_t attr) {
  for (; *str; str++, col++) put(s, row, col, *str == ' ' ? 0 : (uint16_t)(attr | (uint8_t)*str));
}

static void text_right(Snes *s, int row, int end_col, const char *str, uint16_t attr) {
  text(s, row, end_col - (int)strlen(str) + 1, str, attr);
}

static uint8_t pressed_hi(const Snes *s) { return s->wram[G->pad_hi]; }
static uint8_t pressed_lo(const Snes *s) { return s->wram[G->pad_lo]; }

//Entry row on the option screen, drawn every loop frame (the ROM never
//touches this row).
static void draw_entry(Snes *s, bool sel) {
  text(s, ROW_ENTRY, COL_ENTRY, "PORT OPTIONS", sel ? A_SEL : A_TEXT);
}

//---------------------------------------------------------------- page

static const char *label(int i) {
  static const char *l[PI_COUNT] = {
    "WIDESCREEN", "WIDE HUD", "PIXEL SHAPE", "FLICKER FIX", "AUTO CHARGE", "TURBO FIRE",
    "REWIND (R)", "AUTOSAVE", "SCANLINES", "FILTER", "INFINITE HEALTH", "INFINITE LIVES",
    "MORE (F1)", "BACK",
  };
  return l[i];
}

static const char *value(int i) {
  const Settings *c = cfg();
  static const char *wide[WIDE_COUNT] = {"OFF", "16:10", "16:9", "2:1"};
  switch (i) {
  case PI_WIDE: return wide[c->widescreen];
  case PI_WIDE_HUD: return c->wide_hud ? "EDGE" : "CENTER";
  case PI_ASPECT: return c->pixel_aspect ? "8:7" : "SQUARE";
  case PI_FLICKER: return c->no_sprite_limit ? "ON" : "OFF";
  case PI_CHARGE: return c->auto_charge ? "ON" : "OFF";
  case PI_TURBO: return c->turbo_fire ? "ON" : "OFF";
  case PI_REWIND: return c->rewind ? "ON" : "OFF";
  case PI_AUTOSAVE: return c->autosave ? "ON" : "OFF";
  case PI_SCANLINES: {
    static const char *v[4] = {"OFF", "25%", "50%", "75%"};
    return v[(c->scanlines + 12) / 25 > 3 ? 3 : (c->scanlines + 12) / 25];
  }
  case PI_FILTER: return c->filter == FILTER_LINEAR ? "SMOOTH" : "SHARP";
  case PI_HEALTH: return c->inf_health ? "ON" : "OFF";
  case PI_LIVES: return c->inf_lives ? "ON" : "OFF";
  default: return "";
  }
}

static int item_row(int i) { return i == PI_BACK ? 24 : 9 + i; }

static void frame_box(Snes *s, int top, int bottom, int left, int right, const char *title) {
  if (!G->box) {
    text(s, top, (left + right + 1 - (int)strlen(title)) / 2, title, A_SEL);
    return;
  }
  put(s, top, left, A_FRAME | 0xAB);
  put(s, top, right, A_FRAME | 0x4000 | 0xAB);
  put(s, bottom, left, A_FRAME | 0x8000 | 0xAB);
  put(s, bottom, right, A_FRAME | 0xC000 | 0xAB);
  for (int c = left + 1; c < right; c++) {
    put(s, top, c, A_FRAME | 0xAC);
    put(s, bottom, c, A_FRAME | 0x8000 | 0xAC);
  }
  for (int r = top + 1; r < bottom; r++) {
    put(s, r, left, A_FRAME | 0xAD);
    put(s, r, right, A_FRAME | 0x4000 | 0xAD);
  }
  int n = (int)strlen(title), c0 = (left + right + 1 - n) / 2;
  put(s, top, c0 - 1, A_FRAME | 0xAA);
  text(s, top, c0, title, A_VALUE);
  put(s, top, c0 + n, A_FRAME | 0x4000 | 0xAA);
}

static void draw_page(Snes *s) {
  for (int r = 6; r < 28; r++)
    for (int c = 0; c < 32; c++) put(s, r, c, 0);
  frame_box(s, 7, 26, 3, 28, "PORT OPTIONS");
  for (int i = 0; i < PI_COUNT; i++) {
    int row = item_row(i);
    bool sel = i == g_sel;
    if (i == PI_BACK) { text(s, row, 14, "BACK", sel ? A_SEL : A_TEXT); continue; }
    text(s, row, 5, label(i), sel ? A_SEL : A_TEXT);
    text_right(s, row, 26, value(i), A_VALUE);
  }
}

static void open_page(Snes *s) {
  for (int r = 0; r < 28; r++)
    for (int c = 0; c < 32; c++) g_saved[r * 32 + c] = get(s, r, c);
  g_open = true;
  g_sel = 0;
  draw_page(s);
}

static void close_page(Snes *s) {
  for (int r = 0; r < 28; r++)
    for (int c = 0; c < 32; c++) put(s, r, c, g_saved[r * 32 + c]);
  g_open = false;
}

static int cycle(int v, int n, int d) { return ((v + d) % n + n) % n; }

static void change(int i, int d) {
  Settings *c = cfg();
  switch (i) {
  case PI_WIDE: c->widescreen = cycle(c->widescreen, WIDE_COUNT, d); break;
  case PI_WIDE_HUD: c->wide_hud = !c->wide_hud; break;
  case PI_ASPECT: c->pixel_aspect = !c->pixel_aspect; break;
  case PI_FLICKER: c->no_sprite_limit = !c->no_sprite_limit; break;
  case PI_CHARGE: c->auto_charge = !c->auto_charge; break;
  case PI_TURBO: c->turbo_fire = !c->turbo_fire; break;
  case PI_REWIND: c->rewind = !c->rewind; break;
  case PI_AUTOSAVE: c->autosave = !c->autosave; break;
  case PI_SCANLINES: c->scanlines = cycle((c->scanlines + 12) / 25, 4, d) * 25; break;
  case PI_FILTER: c->filter = c->filter == FILTER_LINEAR ? FILTER_NEAREST : FILTER_LINEAR; break;
  case PI_HEALTH: c->inf_health = !c->inf_health; break;
  case PI_LIVES: c->inf_lives = !c->inf_lives; break;
  default: return;
  }
  if (g_port.settings_changed) g_port.settings_changed();
}

static void page_frame(Snes *s) {
  uint8_t hi = pressed_hi(s), lo = pressed_lo(s);
  bool up = hi & 0x08, down = hi & 0x04, left = hi & 0x02, right = hi & 0x01;
  bool confirm = (hi & 0x90) || (lo & 0x80);  //B, Start, A
  bool cancel = hi & 0x40;                    //Y
  if (up) g_sel = cycle(g_sel, PI_COUNT, -1);
  else if (down) g_sel = cycle(g_sel, PI_COUNT, 1);
  else if (left || right) change(g_sel, right ? 1 : -1);
  else if (confirm) {
    if (g_sel == PI_BACK) { close_page(s); return; }
    if (g_sel == PI_MORE) { if (g_port.open_menu) g_port.open_menu(); }
    else change(g_sel, 1);
  } else if (cancel) { close_page(s); return; }
  draw_page(s);
}

//---------------------------------------------------------------- overrides

static bool use(const Snes *s) {
  G = &games[s->game & 3];
  return G->pc_yield != 0;
}

//Loop body after the per-frame bookkeeping (G3 00:EA42). Runs the page
//while open; otherwise moves the cursor in the order ..., before, port, exit.
static bool opt_cursor(Snes *s) {
  if (!use(s)) return false;
  if (g_open) {
    page_frame(s);
    s->cpu.pc = PC_YIELD;
    return true;
  }
  uint8_t hi = pressed_hi(s);
  int cur = s->wram[CUR];
  draw_entry(s, cur == ITEM_PORT);
  if (!(hi & 0x0C)) {
    if (cur != ITEM_PORT) return false;   //ROM: key repeat and wraparound
    s->cpu.pc = G->pc_moved;
    return true;
  }
  //Only moves into or out of the entry are taken over; the rest stay on
  //the ROM path.
  bool down = hi & 0x04;
  int next;
  if (cur == G->item_before && down) next = ITEM_PORT;
  else if (cur == G->item_exit && !down) next = ITEM_PORT;
  else if (cur == ITEM_PORT) next = down ? G->item_exit : G->item_before;
  else return false;
  s->wram[CUR] = (uint8_t)next;
  draw_entry(s, next == ITEM_PORT);
  s->cpu.pc = G->pc_moved;
  return true;
}

//After a cursor change: the old item is redrawn plain from the string
//table, which has no entry for ours.
static bool opt_draw_old(Snes *s) {
  if (!use(s) || s->wram[PREV] != ITEM_PORT) return false;
  s->cpu.pc = G->pc_new;
  return true;
}

//The new item is redrawn highlighted.
static bool opt_draw_new(Snes *s) {
  if (!use(s) || s->wram[CUR] != ITEM_PORT) return false;
  s->cpu.pc = PC_YIELD;
  return true;
}

//Item handler dispatch.
static bool opt_dispatch(Snes *s) {
  if (!use(s) || s->wram[CUR] != ITEM_PORT) return false;
  uint8_t hi = pressed_hi(s), lo = pressed_lo(s);
  if ((hi & 0x90) || (lo & 0x80)) open_page(s);
  s->cpu.pc = PC_YIELD;
  return true;
}

//The overrides of each game (tools/gN/overrides.txt).
bool ovr_OptCursor(Snes *s) { return opt_cursor(s); }        //00:EA42
bool ovr_OptDrawOld(Snes *s) { return opt_draw_old(s); }     //00:EA8A
bool ovr_OptDrawNew(Snes *s) { return opt_draw_new(s); }     //00:EAA7
bool ovr_OptDispatch(Snes *s) { return opt_dispatch(s); }    //00:EACB
bool ovr_G2OptCursor(Snes *s) { return opt_cursor(s); }      //00:EACB
bool ovr_G2OptDrawOld(Snes *s) { return opt_draw_old(s); }   //00:EB13
bool ovr_G2OptDrawNew(Snes *s) { return opt_draw_new(s); }   //00:EB30
bool ovr_G2OptDispatch(Snes *s) { return opt_dispatch(s); }  //00:EB54
bool ovr_G1OptCursor(Snes *s) { return opt_cursor(s); }      //80:EAB9
bool ovr_G1OptDrawOld(Snes *s) { return opt_draw_old(s); }   //80:EAFE
bool ovr_G1OptDrawNew(Snes *s) { return opt_draw_new(s); }   //80:EB1B
bool ovr_G1OptDispatch(Snes *s) { return opt_dispatch(s); }  //80:EB3F
