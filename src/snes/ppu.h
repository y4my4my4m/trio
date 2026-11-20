#ifndef SNES_PPU_H
#define SNES_PPU_H

#include <stdint.h>
#include <stdbool.h>

//S-PPU1/S-PPU2. Scanline renderer. Output is up to 239 visible lines,
//XRGB8888, line 0 of the framebuffer = scanline 1.
//
//Widescreen: `wide` extra columns are rendered on each side of the 256
//classic columns. Framebuffer column 0 is screen x = -wide. wide=0 is the
//hardware behaviour.

#define PPU_W 256
#define PPU_H 239
#define PPU_WIDE_MAX 96
#define PPU_MAXW (PPU_W + 2 * PPU_WIDE_MAX)

//Frontend enhancements; not machine state, not saved in states.
typedef struct PpuEnh {
  int wide;                //extra columns per side, 0..PPU_WIDE_MAX
  uint8_t wide_layers;     //bit per layer (BG1-4, OBJ) allowed to extend
  bool wide_win_center;    //extended pixels use window state at x=128, else nearest edge
  bool no_sprite_limit;    //ignore 32 sprite / 34 tile per-line limits
  uint8_t layer_mask;      //bit per layer (BG1-4, OBJ) shown on main and sub screen; 0 = all
  //Per-BG tilemap used instead of the 32/64-wide VRAM tilemap (all columns;
  //game ring buffers leave edge columns stale): column-major 16-bit entries, vmap_h entries per column, indexed
  //by (full scroll + screen pixel) / 8. Outside the map is transparent.
  //Full scroll = the value congruent to the scroll register mod 256 nearest
  //to vmap_hint (games may keep only the low bits in the register).
  const uint8_t *vmap[4];
  int vmap_w[4], vmap_h[4];
  int vmap_hint_x[4], vmap_hint_y[4];
  uint16_t hide_chr_lo[4], hide_chr_hi[4];  //tiles in [lo, hi] render transparent when hi > 0
  //Per-BG, 8 bytes per character (row top first, bit 7 = leftmost unflipped
  //pixel): set pixels of 2bpp color index 2 render as index 3. NULL = none.
  const uint8_t *ink_mask[4];
  //Per-BG: tilemap entries at tile (tx, ty) in [move_tx0, move_tx1) x
  //[move_ty0, move_ty1) (after scroll) with character in [move_chr_lo,
  //move_chr_hi] draw move_dx pixels to the right, into extended columns if
  //needed. move_dx = 0: none.
  int move_dx[4];
  //OAM entries [0, hud_objs) are fixed HUD sprites: in widescreen they move
  //hud_dx pixels outward (left half of the screen left, right half right).
  int hud_objs, hud_dx;
  uint8_t move_tx0[4], move_tx1[4], move_ty0[4], move_ty1[4];
  uint16_t move_chr_lo[4], move_chr_hi[4];
} PpuEnh;

typedef struct Ppu {
  uint8_t vram[0x10000];   //64 KiB, word addressed by $2116
  uint16_t cgram[256];     //BGR555
  uint8_t oam[544];        //512 low + 32 high

  //Register state; layout private to ppu.c beyond these fields.
  uint8_t regs_opaque[1024];

  //H/V counter latch, written by the owner on $2137 read or WRIO latch.
  uint16_t hcount_latch, vcount_latch;
  bool counter_latched;

  bool overscan;           //SETINI bit 2
  bool forced_blank;
  uint8_t brightness;

  const struct PpuEnh *enh; //frontend enhancements; NULL = hardware behaviour

  int out_w;               //width of the last rendered frame, 256 + 2*wide
  uint32_t frame[PPU_H][PPU_MAXW];
} Ppu;

void ppu_reset(Ppu *p);
void ppu_write(Ppu *p, uint8_t reg, uint8_t val);  //reg = low byte, $00..$33 for $2100..$2133
uint8_t ppu_read(Ppu *p, uint8_t reg, uint8_t open_bus);  //$34..$3F for $2134..$213F
//BG register state for frontend overlays (end-of-frame values).
typedef struct PpuBgInfo {
  uint16_t map_base, chr_base;  //word addresses
  uint8_t map_size;        //BGnSC bits 0-1
  uint16_t hofs, vofs;     //10 bits
  uint8_t mode;            //BGMODE bits 0-2
  bool main_screen;        //TM bit
  bool tile16;
} PpuBgInfo;
void ppu_bg_info(const Ppu *p, int bg, PpuBgInfo *o);
//OBJ tile bases (OBSEL, end-of-frame values): word addresses of the first
//and second name tables, and the size setting.
void ppu_obj_info(const Ppu *p, uint16_t *name, uint16_t *sel, uint8_t *size);

//Draws OAM entries [first, end) straight onto an XRGB8888 image, in front of
//what is there (no priorities, windows or color math): one machine's sprites
//composited over another's picture. 9-bit X below 256 + ext is taken as
//positive (widescreen). Screen (x, y) lands on image pixel (x + dx, y + dy).
//One sprite (OAM-style tile and attribute byte, size small / large) at image
//(x, y), with this PPU's VRAM, palettes and sprite settings, at full brightness.
void ppu_draw_sprite(const Ppu *p, int x, int y, uint8_t tile, uint8_t attr, bool big, uint32_t *img, int stride, int w, int h);
void ppu_draw_objs(const Ppu *p, int first, int end, int ext, uint32_t *img, int stride, int w, int h, int dx, int dy);

//Draws a 4bpp BG layer from a whole-level tilemap (the PpuEnh.vmap layout:
//column-major, mh entries per column) with this PPU's characters for layer
//bg and its palettes: the tiles whose priority bit equals prio, opaque
//pixels only. Map pixel (sx, sy) lands on image pixel (0, 0).
void ppu_draw_bg_map(const Ppu *p, int bg, const uint8_t *map, int mw, int mh, int sx, int sy, int prio,
                     uint32_t *img, int stride, int w, int h);
//Backdrop color (CGRAM 0) as XRGB8888, at the current brightness.
uint32_t ppu_backdrop(const Ppu *p);

void ppu_render_line(Ppu *p, int line);  //line 1..224 (or 239 when overscan)
void ppu_frame_start(Ppu *p);            //called at V=0: OAM addr reload, range flags reset
void ppu_vblank_start(Ppu *p);           //called at first vblank line: OAM address reset if not forced blank

#endif
