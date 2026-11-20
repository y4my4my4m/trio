#include "ppu.h"
#include <string.h>

typedef struct PpuRegs {
  //OAM
  uint16_t oam_base;       //word address, 9 bits
  uint16_t oam_addr;       //byte address, 10 bits
  uint8_t oam_latch;
  bool oam_prio;
  uint8_t first_obj;
  uint8_t obj_size;
  uint16_t obj_name, obj_sel;  //word addresses
  bool time_over, range_over;

  //BG
  uint8_t mode;
  bool bg3prio;
  bool tile16[4];
  uint8_t mosaic_size;     //1..16
  uint8_t mosaic_en;
  uint16_t sc_base[4];     //word address
  uint8_t sc_size[4];
  uint16_t chr_base[4];    //word address
  uint16_t hofs[4], vofs[4];  //full 16-bit latch result; 10 bits used
  uint8_t ofs_latch, m7_latch;

  //VRAM port
  uint16_t vaddr, vread, vstep;
  bool vinc_high;
  uint8_t vremap;

  //Mode 7
  bool m7_hflip, m7_vflip;
  uint8_t m7_repeat;
  int16_t m7a, m7b, m7c, m7d;
  int16_t m7x, m7y, m7hofs, m7vofs;  //13-bit signed
  int32_t mult;

  //CGRAM port
  uint8_t cg_addr, cg_latch;
  bool cg_toggle;

  //Windows, screens, color math
  uint8_t wsel[3];
  uint8_t wh[4];
  uint16_t wlog;           //WBGLOG | WOBJLOG<<8
  uint8_t tm, ts, tmw, tsw;
  uint8_t cgwsel, cgadsub;
  uint16_t fixed;          //BGR555
  uint8_t setini;

  bool hct_toggle, vct_toggle, field;
  bool vblank;
} PpuRegs;

_Static_assert(sizeof(PpuRegs) <= sizeof(((Ppu *)0)->regs_opaque), "PpuRegs exceeds regs_opaque");

static inline PpuRegs *R(Ppu *p) { return (PpuRegs *)(void *)p->regs_opaque; }

static inline uint16_t vram_word(const Ppu *p, uint16_t w) {
  w &= 0x7FFF;
  return p->vram[w * 2] | p->vram[w * 2 + 1] << 8;
}

static inline int sext13(int v) { v &= 0x1FFF; return (v ^ 0x1000) - 0x1000; }

static uint16_t vram_remap(const PpuRegs *r) {
  uint16_t a = r->vaddr;
  switch (r->vremap) {
  case 1: return (a & 0xFF00) | (a & 0x001F) << 3 | (a >> 5 & 7);
  case 2: return (a & 0xFE00) | (a & 0x003F) << 3 | (a >> 6 & 7);
  case 3: return (a & 0xFC00) | (a & 0x007F) << 3 | (a >> 7 & 7);
  }
  return a;
}

//Result packs pixel i (left to right) in bits 8i..8i+7.
static uint64_t bit_expand[2][256];  //[0] bit 7 -> pixel 0; [1] bit 0 -> pixel 0

static void bit_expand_init(void) {
  if (bit_expand[0][1]) return;
  for (int b = 0; b < 256; b++)
    for (int i = 0; i < 8; i++) {
      if (b >> (7 - i) & 1) bit_expand[0][b] |= 1ull << (8 * i);
      if (b >> i & 1) bit_expand[1][b] |= 1ull << (8 * i);
    }
}

static void oam_reload(PpuRegs *r) {
  r->oam_addr = r->oam_base << 1;
  r->first_obj = r->oam_prio ? (r->oam_addr >> 2) & 127 : 0;
}

void ppu_reset(Ppu *p) {
  memset(p->vram, 0, sizeof p->vram);
  memset(p->cgram, 0, sizeof p->cgram);
  memset(p->oam, 0, sizeof p->oam);
  memset(p->regs_opaque, 0, sizeof p->regs_opaque);
  memset(p->frame, 0, sizeof p->frame);
  p->out_w = PPU_W;
  bit_expand_init();
  PpuRegs *r = R(p);
  r->mosaic_size = 1;
  r->vstep = 1;
  r->obj_sel = 0x1000;
  p->hcount_latch = p->vcount_latch = 0;
  p->counter_latched = false;
  p->overscan = false;
  p->forced_blank = true;
  p->brightness = 0;
}

static void mult_update(PpuRegs *r) { r->mult = (int32_t)r->m7a * (int8_t)((uint16_t)r->m7b >> 8); }

void ppu_write(Ppu *p, uint8_t reg, uint8_t val) {
  PpuRegs *r = R(p);
  switch (reg) {
  case 0x00:
    p->forced_blank = val & 0x80;
    p->brightness = val & 15;
    break;
  case 0x01:
    r->obj_size = val >> 5;
    r->obj_name = (val & 7) << 13;
    r->obj_sel = ((val >> 3 & 3) + 1) << 12;
    break;
  case 0x02:
    r->oam_base = (r->oam_base & 0x100) | val;
    oam_reload(r);
    break;
  case 0x03:
    r->oam_base = (r->oam_base & 0xFF) | (val & 1) << 8;
    r->oam_prio = val & 0x80;
    oam_reload(r);
    break;
  case 0x04: {
    uint16_t a = r->oam_addr;
    if (!(a & 1)) r->oam_latch = val;
    if (a & 0x200) p->oam[0x200 | (a & 0x1F)] = val;
    else if (a & 1) { p->oam[a - 1] = r->oam_latch; p->oam[a] = val; }
    r->oam_addr = (a + 1) & 0x3FF;
    break;
  }
  case 0x05:
    r->mode = val & 7;
    r->bg3prio = val & 8;
    for (int i = 0; i < 4; i++) r->tile16[i] = val >> (4 + i) & 1;
    break;
  case 0x06:
    r->mosaic_size = (val >> 4) + 1;
    r->mosaic_en = val & 15;
    break;
  case 0x07: case 0x08: case 0x09: case 0x0A:
    r->sc_base[reg - 7] = (val & 0xFC) << 8;
    r->sc_size[reg - 7] = val & 3;
    break;
  case 0x0B:
    r->chr_base[0] = (val & 15) << 12;
    r->chr_base[1] = (val >> 4) << 12;
    break;
  case 0x0C:
    r->chr_base[2] = (val & 15) << 12;
    r->chr_base[3] = (val >> 4) << 12;
    break;
  case 0x0D: case 0x0F: case 0x11: case 0x13: {
    int i = (reg - 0x0D) >> 1;
    if (reg == 0x0D) { r->m7hofs = sext13(val << 8 | r->m7_latch); r->m7_latch = val; }
    r->hofs[i] = (uint16_t)(val << 8 | (r->ofs_latch & ~7) | (r->hofs[i] >> 8 & 7));
    r->ofs_latch = val;
    break;
  }
  case 0x0E: case 0x10: case 0x12: case 0x14: {
    int i = (reg - 0x0E) >> 1;
    if (reg == 0x0E) { r->m7vofs = sext13(val << 8 | r->m7_latch); r->m7_latch = val; }
    r->vofs[i] = (uint16_t)(val << 8 | r->ofs_latch);
    r->ofs_latch = val;
    break;
  }
  case 0x15: {
    static const uint8_t step[4] = {1, 32, 128, 128};
    r->vinc_high = val & 0x80;
    r->vremap = val >> 2 & 3;
    r->vstep = step[val & 3];
    break;
  }
  case 0x16:
    r->vaddr = (r->vaddr & 0xFF00) | val;
    r->vread = vram_word(p, vram_remap(r));
    break;
  case 0x17:
    r->vaddr = (r->vaddr & 0x00FF) | val << 8;
    r->vread = vram_word(p, vram_remap(r));
    break;
  case 0x18:
    //VRAM is inaccessible to the CPU during active display.
    if (p->forced_blank || r->vblank) p->vram[(vram_remap(r) & 0x7FFF) * 2] = val;
    if (!r->vinc_high) r->vaddr += r->vstep;
    break;
  case 0x19:
    if (p->forced_blank || r->vblank) p->vram[(vram_remap(r) & 0x7FFF) * 2 + 1] = val;
    if (r->vinc_high) r->vaddr += r->vstep;
    break;
  case 0x1A:
    r->m7_hflip = val & 1;
    r->m7_vflip = val & 2;
    r->m7_repeat = val >> 6;
    break;
  case 0x1B: r->m7a = (int16_t)(val << 8 | r->m7_latch); r->m7_latch = val; mult_update(r); break;
  case 0x1C: r->m7b = (int16_t)(val << 8 | r->m7_latch); r->m7_latch = val; mult_update(r); break;
  case 0x1D: r->m7c = (int16_t)(val << 8 | r->m7_latch); r->m7_latch = val; break;
  case 0x1E: r->m7d = (int16_t)(val << 8 | r->m7_latch); r->m7_latch = val; break;
  case 0x1F: r->m7x = sext13(val << 8 | r->m7_latch); r->m7_latch = val; break;
  case 0x20: r->m7y = sext13(val << 8 | r->m7_latch); r->m7_latch = val; break;
  case 0x21:
    r->cg_addr = val;
    r->cg_toggle = false;
    break;
  case 0x22:
    if (!r->cg_toggle) r->cg_latch = val;
    else p->cgram[r->cg_addr++] = (val & 0x7F) << 8 | r->cg_latch;
    r->cg_toggle = !r->cg_toggle;
    break;
  case 0x23: case 0x24: case 0x25: r->wsel[reg - 0x23] = val; break;
  case 0x26: case 0x27: case 0x28: case 0x29: r->wh[reg - 0x26] = val; break;
  case 0x2A: r->wlog = (r->wlog & 0xFF00) | val; break;
  case 0x2B: r->wlog = (r->wlog & 0x00FF) | (val & 15) << 8; break;
  case 0x2C: r->tm = val & 0x1F; break;
  case 0x2D: r->ts = val & 0x1F; break;
  case 0x2E: r->tmw = val & 0x1F; break;
  case 0x2F: r->tsw = val & 0x1F; break;
  case 0x30: r->cgwsel = val; break;
  case 0x31: r->cgadsub = val; break;
  case 0x32: {
    uint16_t c = val & 0x1F;
    if (val & 0x20) r->fixed = (r->fixed & ~0x001F) | c;
    if (val & 0x40) r->fixed = (r->fixed & ~0x03E0) | c << 5;
    if (val & 0x80) r->fixed = (r->fixed & ~0x7C00) | c << 10;
    break;
  }
  case 0x33:
    r->setini = val;
    p->overscan = val & 4;
    break;
  }
}

uint8_t ppu_read(Ppu *p, uint8_t reg, uint8_t open_bus) {
  PpuRegs *r = R(p);
  switch (reg) {
  case 0x34: return r->mult;
  case 0x35: return r->mult >> 8;
  case 0x36: return r->mult >> 16;
  case 0x38: {
    uint16_t a = r->oam_addr;
    uint8_t v = p->oam[a & 0x200 ? 0x200 | (a & 0x1F) : a];
    r->oam_addr = (a + 1) & 0x3FF;
    return v;
  }
  case 0x39: {
    uint8_t v = r->vread;
    if (!r->vinc_high) { r->vread = vram_word(p, vram_remap(r)); r->vaddr += r->vstep; }
    return v;
  }
  case 0x3A: {
    uint8_t v = r->vread >> 8;
    if (r->vinc_high) { r->vread = vram_word(p, vram_remap(r)); r->vaddr += r->vstep; }
    return v;
  }
  case 0x3B: {
    uint8_t v;
    if (!r->cg_toggle) v = p->cgram[r->cg_addr];
    else v = (p->cgram[r->cg_addr++] >> 8 & 0x7F) | (open_bus & 0x80);
    r->cg_toggle = !r->cg_toggle;
    return v;
  }
  case 0x3C: {
    uint8_t v = r->hct_toggle ? (p->hcount_latch >> 8 & 1) | (open_bus & 0xFE) : (uint8_t)p->hcount_latch;
    r->hct_toggle = !r->hct_toggle;
    return v;
  }
  case 0x3D: {
    uint8_t v = r->vct_toggle ? (p->vcount_latch >> 8 & 1) | (open_bus & 0xFE) : (uint8_t)p->vcount_latch;
    r->vct_toggle = !r->vct_toggle;
    return v;
  }
  case 0x3E:
    return r->time_over << 7 | r->range_over << 6 | (open_bus & 0x10) | 1;
  case 0x3F: {
    uint8_t v = r->field << 7 | p->counter_latched << 6 | (open_bus & 0x20) | 3;
    p->counter_latched = false;
    r->hct_toggle = r->vct_toggle = false;
    return v;
  }
  }
  return open_bus;
}

void ppu_frame_start(Ppu *p) {
  PpuRegs *r = R(p);
  r->field = !r->field;
  r->time_over = r->range_over = false;
  r->vblank = false;
  if (!p->forced_blank) oam_reload(r);
}

void ppu_vblank_start(Ppu *p) {
  R(p)->vblank = true;
  if (!p->forced_blank) oam_reload(R(p));
}

//Rendering. Line buffers are indexed by screen x + L->wide.

static const PpuEnh no_enh;

enum { L_BG1, L_BG2, L_BG3, L_BG4, L_OBJ, L_BACK, L_OBJNM };
#define MW PPU_MAXW

typedef struct Line {
  int wide, w;
  uint16_t col[5][MW];
  uint8_t pri[5][MW];      //0 transparent; BG 1/2 = low/high; OBJ 1..4 = prio+1
  uint8_t objmath[MW];
  uint8_t win[6][MW];
} Line;

static inline uint16_t direct_color(unsigned pal, unsigned c) {
  return (c << 2 & 0x001C) | (pal << 1 & 0x0002) | (c << 4 & 0x0380) |
         (pal << 5 & 0x0040) | (c << 7 & 0x6000) | (pal << 10 & 0x1000);
}

//Tile row: addr = word address of row in bitplanes 0/1; planes 2n/2n+1 at +8n words.
static inline uint64_t tile_row(const Ppu *p, uint16_t addr, int bpp, bool hflip) {
  const uint64_t *e = bit_expand[hflip];
  uint64_t v = 0;
  for (int k = 0; k < bpp / 2; k++) {
    uint16_t w = (addr + 8 * k) & 0x7FFF;
    v |= e[p->vram[w * 2]] << (2 * k) | e[p->vram[w * 2 + 1]] << (2 * k + 1);
  }
  return v;
}

static inline uint16_t map_entry(const Ppu *p, const PpuRegs *r, int bg, int tx, int ty) {
  uint16_t a = r->sc_base[bg] + ((ty & 31) << 5) + (tx & 31);
  if (tx & 32) a += 0x400;
  if (ty & 32) a += (r->sc_size[bg] & 1) ? 0x800 : 0x400;
  return vram_word(p, a);
}

//BG3 tilemap entry at pixel position, for offset-per-tile lookups.
static uint16_t opt_entry(const Ppu *p, const PpuRegs *r, int hx, int vy) {
  int ts = r->tile16[2] ? 4 : 3;
  int hmask = (32 << ts << (r->sc_size[2] & 1)) - 1;
  int vmask = (32 << ts << (r->sc_size[2] >> 1 & 1)) - 1;
  return map_entry(p, r, 2, (hx & hmask) >> ts, (vy & vmask) >> ts);
}

//ext: columns rendered beyond each classic edge (0 in hi-res).
static void render_bg(Ppu *p, const PpuRegs *r, int bg, int line, int bpp, bool hires, bool opt,
                      Line *L, int ext) {
  const PpuEnh *E = p->enh ? p->enh : &no_enh;
  uint16_t hcol[512];
  uint8_t hpri[512];
  uint16_t *col = L->col[bg];
  uint8_t *pri = L->pri[bg];
  memset(pri, 0, (size_t)L->w);
  if (hires) ext = 0;
  uint16_t *oc = hires ? hcol : col + L->wide;
  uint8_t *op = hires ? hpri : pri + L->wide;
  int W = hires ? 512 : 256;
  if (hires) memset(hpri, 0, sizeof hpri);

  int y = line;
  if (r->mosaic_en >> bg & 1) y -= line % r->mosaic_size;
  int hscroll = r->hofs[bg] & 0x3FF, vscroll = r->vofs[bg] & 0x3FF;
  if (hires) hscroll <<= 1;

  int tws = (r->tile16[bg] || hires) ? 4 : 3, ths = r->tile16[bg] ? 4 : 3;
  int hmask = (32 << tws << (r->sc_size[bg] & 1)) - 1;
  int vmask = (32 << ths << (r->sc_size[bg] >> 1 & 1)) - 1;
  int palbase = r->mode == 0 ? bg * 32 : 0;
  int palshift = bpp == 2 ? 2 : bpp == 4 ? 4 : 0;
  bool direct = bpp == 8 && (r->cgwsel & 1);
  uint16_t valid = 0x2000 << bg;

  for (int x = -(hscroll & 7) - ((ext + 7) & ~7); x < W + ext; x += 8) {
    int hoff = x + hscroll, voff = y + vscroll;
    if (opt) {
      int ox = x + (hscroll & 7);
      if (ox >= 8) {
        int lx = ox - 8 + (r->hofs[2] & 0x3F8);
        uint16_t h = opt_entry(p, r, lx, r->vofs[2] & 0x3FF);
        if (r->mode == 4) {
          if (h & valid) {
            if (!(h & 0x8000)) hoff = ox + (h & ~7);
            else voff = y + h;
          }
        } else {
          uint16_t v = opt_entry(p, r, lx, (r->vofs[2] & 0x3FF) + 8);
          if (h & valid) hoff = ox + (h & ~7);
          if (v & valid) voff = y + v;
        }
      }
    }
    uint16_t e;
    if (E->vmap[bg] && !opt && tws == 3 && ths == 3) {
      int fx = E->vmap_hint_x[bg] + (int8_t)(uint8_t)(hscroll - E->vmap_hint_x[bg]);
      int fy = E->vmap_hint_y[bg] + (int8_t)(uint8_t)(vscroll - E->vmap_hint_y[bg]);
      hoff = x + fx;
      voff = y + fy;
      int tx = hoff >> 3, ty = voff >> 3;
      if (tx < 0 || ty < 0 || tx >= E->vmap_w[bg] || ty >= E->vmap_h[bg]) continue;
      const uint8_t *m = E->vmap[bg] + ((size_t)tx * E->vmap_h[bg] + ty) * 2;
      e = m[0] | m[1] << 8;
      hoff &= 7;
      voff &= 7;
    } else {
      hoff &= hmask;
      voff &= vmask;
      e = map_entry(p, r, bg, hoff >> tws, voff >> ths);
    }
    if (E->hide_chr_hi[bg] >= E->hide_chr_lo[bg] && E->hide_chr_hi[bg] &&
        (e & 0x3FF) >= E->hide_chr_lo[bg] && (e & 0x3FF) <= E->hide_chr_hi[bg]) continue;
    bool hf = e & 0x4000, vf = e & 0x8000;
    unsigned c = e & 0x3FF;
    if (tws == 4) c += ((hoff >> 3) & 1) ^ hf;
    if (ths == 4) c += (((voff >> 3) & 1) ^ vf) << 4;
    c &= 0x3FF;
    int row = (voff & 7) ^ (vf ? 7 : 0);
    uint64_t px = tile_row(p, r->chr_base[bg] + c * bpp * 4 + row, bpp, hf);
    if (!px) continue;
    if (bpp == 2 && E->ink_mask[bg] && E->ink_mask[bg][c * 8 + row]) {
      uint8_t m = E->ink_mask[bg][c * 8 + row];
      for (int i = 0; i < 8; i++)
        if ((m >> (hf ? i : 7 - i) & 1) && (px >> (8 * i) & 0xFF) == 2) px ^= (uint64_t)1 << (8 * i);
    }
    int mdx = 0, lim = ext;
    if (E->move_dx[bg] && !E->vmap[bg] && !hires) {
      int tx = hoff >> tws, ty = voff >> ths;
      if (tx >= E->move_tx0[bg] && tx < E->move_tx1[bg] && ty >= E->move_ty0[bg] &&
          ty < E->move_ty1[bg] && (e & 0x3FF) >= E->move_chr_lo[bg] && (e & 0x3FF) <= E->move_chr_hi[bg]) {
        mdx = E->move_dx[bg];
        lim = L->wide;
      }
    }
    unsigned pal = e >> 10 & 7;
    uint8_t pr = (e & 0x2000) ? 2 : 1;
    int base = palbase + (pal << palshift);
    for (int i = 0; i < 8; i++, px >>= 8) {
      int cx = x + i + mdx;
      uint8_t ci = px;
      if (cx < -lim || cx >= W + lim || !ci) continue;
      oc[cx] = direct ? direct_color(pal, ci) : p->cgram[(base + ci) & 0xFF];
      op[cx] = pr;
    }
  }
  if (hires)
    for (int x = 0; x < 256; x++) { col[L->wide + x] = hcol[2 * x]; pri[L->wide + x] = hpri[2 * x]; }
}

void ppu_obj_info(const Ppu *p, uint16_t *name, uint16_t *sel, uint8_t *size) {
  const PpuRegs *r = R((Ppu *)p);
  *name = r->obj_name;
  *sel = r->obj_sel;
  *size = r->obj_size;
}

void ppu_bg_info(const Ppu *p, int bg, PpuBgInfo *o) {
  const PpuRegs *r = R((Ppu *)p);
  o->map_base = r->sc_base[bg];
  o->chr_base = r->chr_base[bg];
  o->map_size = r->sc_size[bg];
  o->hofs = r->hofs[bg] & 0x3FF;
  o->vofs = r->vofs[bg] & 0x3FF;
  o->mode = r->mode;
  o->main_screen = r->tm >> bg & 1;
  o->tile16 = r->tile16[bg];
}

static void render_m7(Ppu *p, const PpuRegs *r, int line, Line *L, bool bg1, bool bg2, int ext) {
  int a = r->m7a, b = r->m7b, c = r->m7c, d = r->m7d;
  int hc = r->m7x, vc = r->m7y;
  int y = line;
  if (r->mosaic_en & 1) y -= line % r->mosaic_size;
  if (r->m7_vflip) y = 255 - y;
  #define CLIP(n) (((n) & 0x2000) ? ((n) | ~1023) : ((n) & 1023))
  int dh = CLIP(r->m7hofs - hc), dv = CLIP(r->m7vofs - vc);
  #undef CLIP
  int ox = (a * dh & ~63) + (b * dv & ~63) + (b * y & ~63) + hc * 256;
  int oy = (c * dh & ~63) + (d * dv & ~63) + (d * y & ~63) + vc * 256;
  bool direct = r->cgwsel & 1;
  memset(L->pri[0], 0, (size_t)L->w);
  memset(L->pri[1], 0, (size_t)L->w);
  for (int x = -ext; x < 256 + ext; x++) {
    int X = r->m7_hflip ? 255 - x : x;
    int px = (ox + a * X) >> 8, py = (oy + c * X) >> 8;
    uint8_t tile;
    if ((px | py) & ~1023) {
      if (r->m7_repeat == 2) continue;
      if (r->m7_repeat == 3) tile = 0;
      else tile = p->vram[((py >> 3 & 127) * 128 + (px >> 3 & 127)) * 2];
    } else {
      tile = p->vram[((py >> 3 & 127) * 128 + (px >> 3 & 127)) * 2];
    }
    uint8_t v = p->vram[(tile * 64 + (py & 7) * 8 + (px & 7)) * 2 + 1];
    int i = x + L->wide;
    if (bg1 && v) {
      L->col[0][i] = direct ? direct_color(0, v) : p->cgram[v];
      L->pri[0][i] = 1;
    }
    if (bg2 && (v & 0x7F)) {
      L->col[1][i] = p->cgram[v & 0x7F];
      L->pri[1][i] = (v >> 7) + 1;
    }
  }
}

static const uint8_t obj_w[8][2] = {{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}};
static const uint8_t obj_h[8][2] = {{8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{32,64},{32,32}};

//Hardware sprite pass: 9-bit X wraps, 32 sprite / 34 tile limits.
static void render_obj_hw(Ppu *p, PpuRegs *r, int line, Line *L) {
  uint8_t list[32];
  int n = 0, sy = (line - 1) & 255;
  memset(L->pri[L_OBJ], 0, (size_t)L->w);
  for (int i = 0; i < 128; i++) {
    int idx = (r->first_obj + i) & 127;
    const uint8_t *o = &p->oam[idx * 4];
    uint8_t hi = p->oam[0x200 + (idx >> 2)] >> ((idx & 3) * 2);
    int x = o[0] | (hi & 1) << 8, big = hi >> 1 & 1;
    int w = obj_w[r->obj_size][big], h = obj_h[r->obj_size][big];
    if (((sy - o[1]) & 255) >= h) continue;
    if (x > 256 && x + w - 1 < 512) continue;
    if (n == 32) { r->range_over = true; break; }
    list[n++] = idx;
  }
  int tiles = 0;
  for (int k = n - 1; k >= 0; k--) {
    int idx = list[k];
    const uint8_t *o = &p->oam[idx * 4];
    uint8_t hi = p->oam[0x200 + (idx >> 2)] >> ((idx & 3) * 2);
    int x = o[0] | (hi & 1) << 8, big = hi >> 1 & 1;
    int w = obj_w[r->obj_size][big], h = obj_h[r->obj_size][big];
    uint8_t attr = o[3];
    bool hf = attr & 0x40, vf = attr & 0x80;
    int row = (sy - o[1]) & 255;
    if (vf) row = h - 1 - row;
    int cy = row >> 3, tw = w >> 3;
    uint8_t pr = (attr >> 4 & 3) + 1, math = (attr >> 1 & 7) >= 4;
    int palbase = 128 + ((attr >> 1 & 7) << 4);
    for (int tx = 0; tx < tw; tx++) {
      int sx = (x + tx * 8) & 511;
      if (x != 256 && sx >= 256 && sx + 7 < 512) continue;
      if (tiles++ >= 34) { r->time_over = true; return; }
      int cx = hf ? tw - 1 - tx : tx;
      unsigned t = o[2];
      unsigned ch = (((t & 15) + cx) & 15) | (((t >> 4) + cy) & 15) << 4;
      uint16_t addr = r->obj_name + ch * 16 + (row & 7) + ((attr & 1) ? r->obj_sel : 0);
      uint64_t px = tile_row(p, addr, 4, hf);
      for (int i = 0; i < 8; i++, px >>= 8) {
        int X = (sx + i) & 511;
        uint8_t ci = px;
        if (X >= 256 || !ci) continue;
        L->col[L_OBJ][X + L->wide] = p->cgram[palbase + ci];
        L->pri[L_OBJ][X + L->wide] = pr;
        L->objmath[X + L->wide] = math;
      }
    }
  }
}

//Enhanced sprite pass. With ext > 0, 9-bit X in [256, 256+ext) is placed
//right of the classic area instead of wrapping left. Limits apply only to
//sprites/tiles touching the classic area, and not at all with nolimit.
static void render_obj_ext(Ppu *p, PpuRegs *r, int line, Line *L, int ext, bool nolimit) {
  const PpuEnh *E = p->enh ? p->enh : &no_enh;
  uint8_t list[128];
  int16_t lx[128];
  int n = 0, classic = 0, sy = (line - 1) & 255;
  int lo = -ext, hi_x = 256 + ext;
  memset(L->pri[L_OBJ], 0, (size_t)L->w);
  for (int i = 0; i < 128; i++) {
    int idx = (r->first_obj + i) & 127;
    const uint8_t *o = &p->oam[idx * 4];
    uint8_t hi = p->oam[0x200 + (idx >> 2)] >> ((idx & 3) * 2);
    int v = o[0] | (hi & 1) << 8, big = hi >> 1 & 1;
    int w = obj_w[r->obj_size][big], h = obj_h[r->obj_size][big];
    int x = v < 256 + ext ? v : v - 512;
    if (idx < E->hud_objs && E->hud_dx) {
      //HUD sprites: decided on the classic position, then moved to the edge.
      int cx = v < 256 ? v : v - 512;
      x = cx + (cx + w / 2 < 128 ? -E->hud_dx : E->hud_dx);
    }
    if (((sy - o[1]) & 255) >= h) continue;
    if (x >= hi_x || x + w <= lo) continue;
    if (x < 256 && x + w > 0 && !nolimit) {
      if (classic == 32) { r->range_over = true; continue; }
      classic++;
    }
    lx[n] = (int16_t)x;
    list[n++] = idx;
  }
  int tiles = 0;
  for (int k = n - 1; k >= 0; k--) {
    int idx = list[k], x = lx[k];
    const uint8_t *o = &p->oam[idx * 4];
    uint8_t hi = p->oam[0x200 + (idx >> 2)] >> ((idx & 3) * 2);
    int big = hi >> 1 & 1;
    int w = obj_w[r->obj_size][big], h = obj_h[r->obj_size][big];
    uint8_t attr = o[3];
    bool hf = attr & 0x40, vf = attr & 0x80;
    int row = (sy - o[1]) & 255;
    if (vf) row = h - 1 - row;
    int cy = row >> 3, tw = w >> 3;
    uint8_t pr = (attr >> 4 & 3) + 1, math = (attr >> 1 & 7) >= 4;
    int palbase = 128 + ((attr >> 1 & 7) << 4);
    for (int tx = 0; tx < tw; tx++) {
      int sx = x + tx * 8;
      if (sx >= hi_x || sx + 8 <= lo) continue;
      if (sx < 256 && sx + 8 > 0 && !nolimit && tiles++ >= 34) { r->time_over = true; continue; }
      int cx = hf ? tw - 1 - tx : tx;
      unsigned t = o[2];
      unsigned ch = (((t & 15) + cx) & 15) | (((t >> 4) + cy) & 15) << 4;
      uint16_t addr = r->obj_name + ch * 16 + (row & 7) + ((attr & 1) ? r->obj_sel : 0);
      uint64_t px = tile_row(p, addr, 4, hf);
      for (int i = 0; i < 8; i++, px >>= 8) {
        int X = sx + i;
        uint8_t ci = px;
        if (X < lo || X >= hi_x || !ci) continue;
        L->col[L_OBJ][X + L->wide] = p->cgram[palbase + ci];
        L->pri[L_OBJ][X + L->wide] = pr;
        L->objmath[X + L->wide] = math;
      }
    }
  }
}

void ppu_draw_sprite(const Ppu *p, int x, int y, uint8_t tile, uint8_t attr, bool big, uint32_t *img, int stride, int w, int h) {
  const PpuRegs *r = R((Ppu *)p);
  uint32_t lut[32];
  for (int i = 0; i < 32; i++) lut[i] = (uint32_t)((i << 3) | (i >> 2)) * 15 / 15;
  int ow = obj_w[r->obj_size][big], oh = obj_h[r->obj_size][big];
  bool hf = attr & 0x40, vf = attr & 0x80;
  int palbase = 128 + ((attr >> 1 & 7) << 4), tw = ow >> 3;
  if (x + ow <= 0 || x >= w || y + oh <= 0 || y >= h) return;
  for (int row = 0; row < oh; row++) {
    int Y = y + row;
    if (Y < 0 || Y >= h) continue;
    int sr = vf ? oh - 1 - row : row, cy = sr >> 3;
    for (int tx = 0; tx < tw; tx++) {
      int cx = hf ? tw - 1 - tx : tx;
      unsigned ch = (((tile & 15) + cx) & 15) | (((tile >> 4) + cy) & 15) << 4;
      uint16_t addr = r->obj_name + ch * 16 + (sr & 7) + ((attr & 1) ? r->obj_sel : 0);
      uint64_t px = tile_row(p, addr, 4, hf);
      for (int i = 0; i < 8; i++, px >>= 8) {
        int X = x + tx * 8 + i;
        uint8_t ci = px;
        if (!ci || X < 0 || X >= w) continue;
        uint16_t c = p->cgram[palbase + ci];
        img[Y * stride + X] = lut[c & 31] << 16 | lut[c >> 5 & 31] << 8 | lut[c >> 10 & 31];
      }
    }
  }
}

void ppu_draw_objs(const Ppu *p, int first, int end, int ext, uint32_t *img, int stride, int w, int h, int dx, int dy) {
  const PpuRegs *r = R((Ppu *)p);
  uint32_t lut[32];
  for (int i = 0; i < 32; i++) lut[i] = (uint32_t)((i << 3) | (i >> 2)) * p->brightness / 15;
  if (first < 0) first = 0;
  if (end > 128) end = 128;
  //Lower OAM indices are in front: draw from the back.
  for (int idx = end - 1; idx >= first; idx--) {
    const uint8_t *o = &p->oam[idx * 4];
    uint8_t hi = p->oam[0x200 + (idx >> 2)] >> ((idx & 3) * 2);
    int v = o[0] | (hi & 1) << 8, big = hi >> 1 & 1;
    int ow = obj_w[r->obj_size][big], oh = obj_h[r->obj_size][big];
    int x = v < 256 + ext ? v : v - 512;
    uint8_t attr = o[3];
    bool hf = attr & 0x40, vf = attr & 0x80;
    int palbase = 128 + ((attr >> 1 & 7) << 4), tw = ow >> 3;
    for (int row = 0; row < oh; row++) {
      //Rows land on screen lines (Y + row) mod 256, as on hardware; only the
      //224 visible lines are drawn.
      int line = (o[1] + row) & 255;
      if (line >= 224) continue;
      int Y = line + dy;
      if (Y < 0 || Y >= h) continue;
      int sr = vf ? oh - 1 - row : row, cy = sr >> 3;
      for (int tx = 0; tx < tw; tx++) {
        int cx = hf ? tw - 1 - tx : tx;
        unsigned t = o[2];
        unsigned ch = (((t & 15) + cx) & 15) | (((t >> 4) + cy) & 15) << 4;
        uint16_t addr = r->obj_name + ch * 16 + (sr & 7) + ((attr & 1) ? r->obj_sel : 0);
        uint64_t px = tile_row(p, addr, 4, hf);
        for (int i = 0; i < 8; i++, px >>= 8) {
          int X = x + tx * 8 + i + dx;
          uint8_t ci = px;
          if (!ci || X < 0 || X >= w) continue;
          uint16_t c = p->cgram[palbase + ci];
          img[Y * stride + X] = lut[c & 31] << 16 | lut[c >> 5 & 31] << 8 | lut[c >> 10 & 31];
        }
      }
    }
  }
}

static uint32_t bright_rgb(const Ppu *p, uint16_t c) {
  uint32_t r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
  r = (r << 3 | r >> 2) * p->brightness / 15;
  g = (g << 3 | g >> 2) * p->brightness / 15;
  b = (b << 3 | b >> 2) * p->brightness / 15;
  return r << 16 | g << 8 | b;
}

uint32_t ppu_backdrop(const Ppu *p) { return bright_rgb(p, p->cgram[0]); }

void ppu_draw_bg_map(const Ppu *p, int bg, const uint8_t *map, int mw, int mh, int sx, int sy, int prio,
                     uint32_t *img, int stride, int w, int h) {
  const PpuRegs *r = R((Ppu *)p);
  uint32_t pal[128];
  for (int i = 0; i < 128; i++) pal[i] = bright_rgb(p, p->cgram[i]);
  for (int ty = sy >> 3; ty <= (sy + h - 1) >> 3; ty++) {
    if (ty < 0 || ty >= mh) continue;
    for (int tx = sx >> 3; tx <= (sx + w - 1) >> 3; tx++) {
      if (tx < 0 || tx >= mw) continue;
      const uint8_t *m = map + ((size_t)tx * (size_t)mh + (size_t)ty) * 2;
      uint16_t e = (uint16_t)(m[0] | m[1] << 8);
      if (((e >> 13) & 1) != prio) continue;
      bool hf = e & 0x4000, vf = e & 0x8000;
      unsigned c = e & 0x3FF;
      const uint32_t *pl = pal + ((e >> 10 & 7) << 4);
      for (int row = 0; row < 8; row++) {
        int Y = ty * 8 + row - sy;
        if (Y < 0 || Y >= h) continue;
        uint64_t px = tile_row(p, (uint16_t)(r->chr_base[bg] + c * 16 + (vf ? 7 - row : row)), 4, hf);
        for (int i = 0; i < 8; i++, px >>= 8) {
          int X = tx * 8 + i - sx;
          uint8_t ci = (uint8_t)px;
          if (ci && X >= 0 && X < w) img[Y * stride + X] = pl[ci];
        }
      }
    }
  }
}

//need: bit i set computes mask for layer i (BG1-4, OBJ, color).
//Extended columns take the classic mask at x=128 (center) or the nearest edge.
static void render_windows(const PpuRegs *r, Line *L, uint8_t need, bool center) {
  static const uint8_t logic_tab[4][4] = {{0,1,1,1}, {0,0,0,1}, {0,1,1,0}, {1,0,0,1}};
  uint8_t w1[256], w2[256], m[256];
  memset(w1, 0, 256);
  memset(w2, 0, 256);
  if (r->wh[0] <= r->wh[1]) memset(w1 + r->wh[0], 1, r->wh[1] - r->wh[0] + 1);
  if (r->wh[2] <= r->wh[3]) memset(w2 + r->wh[2], 1, r->wh[3] - r->wh[2] + 1);
  for (int i = 0; i < 6; i++) {
    if (!(need >> i & 1)) continue;
    uint8_t sel = r->wsel[i >> 1] >> ((i & 1) * 4) & 15;
    bool e1 = sel & 2, e2 = sel & 8;
    uint8_t i1 = sel & 1, i2 = sel >> 2 & 1;
    if (!e1 && !e2) memset(m, 0, 256);
    else if (!e2) for (int x = 0; x < 256; x++) m[x] = w1[x] ^ i1;
    else if (!e1) for (int x = 0; x < 256; x++) m[x] = w2[x] ^ i2;
    else {
      const uint8_t *t = logic_tab[r->wlog >> (2 * i) & 3];
      for (int x = 0; x < 256; x++) m[x] = t[(w1[x] ^ i1) << 1 | (w2[x] ^ i2)];
    }
    uint8_t *dst = L->win[i];
    memcpy(dst + L->wide, m, 256);
    for (int k = 0; k < L->wide; k++) {
      dst[k] = center ? m[128] : m[0];
      dst[L->wide + 256 + k] = center ? m[128] : m[255];
    }
  }
}

//Front-to-back layer order per mode. Token: layer (1-4 BG, O OBJ), priority (H/L or 0-3).
static const char *const prio_order[9] = {
  "O3 1H 2H O2 1L 2L O1 3H 4H O0 3L 4L",
  "O3 1H 2H O2 1L 2L O1 3H O0 3L",
  "O3 1H O2 2H O1 1L O0 2L",
  "O3 1H O2 2H O1 1L O0 2L",
  "O3 1H O2 2H O1 1L O0 2L",
  "O3 1H O2 2H O1 1L O0 2L",
  "O3 1H O2 O1 1L O0",
  "O3 O2 2H O1 1L O0 2L",
  "3H O3 1H 2H O2 1L 2L O1 O0 3L",
};

static void build_z(const PpuRegs *r, uint8_t zbg[4][2], uint8_t zobj[4]) {
  int m = r->mode;
  if (m == 1 && r->bg3prio) m = 8;
  const char *s = prio_order[m];
  memset(zbg, 0, 8);
  memset(zobj, 0, 4);
  int n = (int)(strlen(s) + 1) / 3;
  for (int i = 0; i < n; i++) {
    char l = s[i * 3], q = s[i * 3 + 1];
    uint8_t z = n - i;
    if (l == 'O') zobj[q - '0'] = z;
    else zbg[l - '1'][q == 'H'] = z;
  }
  if (r->mode == 7) zbg[0][1] = zbg[0][0];
}

static inline uint16_t blend(uint16_t a, uint16_t b, bool sub, bool half) {
  int ra = a & 31, ga = a >> 5 & 31, ba = a >> 10 & 31;
  int rb = b & 31, gb = b >> 5 & 31, bb = b >> 10 & 31;
  int rr, gg, bl;
  if (!sub) { rr = ra + rb; gg = ga + gb; bl = ba + bb; }
  else {
    rr = ra - rb; gg = ga - gb; bl = ba - bb;
    if (rr < 0) rr = 0;
    if (gg < 0) gg = 0;
    if (bl < 0) bl = 0;
  }
  if (half) { rr >>= 1; gg >>= 1; bl >>= 1; }
  if (rr > 31) rr = 31;
  if (gg > 31) gg = 31;
  if (bl > 31) bl = 31;
  return rr | gg << 5 | bl << 10;
}

static inline bool region(int sel, bool w) { return sel == 3 || (sel == 1 && !w) || (sel == 2 && w); }

//Blocks aligned to screen x = 0.
static void mosaic_h(Line *L, int layer, int size) {
  uint16_t *col = L->col[layer];
  uint8_t *pri = L->pri[layer];
  for (int i = 0; i < L->w; i++) {
    int x = i - L->wide;
    int s = (x >= 0 ? x / size : -((-x + size - 1) / size)) * size + L->wide;
    if (s < 0) s = 0;
    if (s != i) { col[i] = col[s]; pri[i] = pri[s]; }
  }
}

void ppu_render_line(Ppu *p, int line) {
  const PpuEnh *E = p->enh ? p->enh : &no_enh;
  int maxline = p->overscan ? 239 : 224;
  if (line < 1 || line > maxline) return;
  int wide = E->wide < 0 ? 0 : E->wide > PPU_WIDE_MAX ? PPU_WIDE_MAX : E->wide & ~1;
  int W = PPU_W + 2 * wide;
  p->out_w = W;
  uint32_t *out = p->frame[line - 1];
  if (p->forced_blank) { memset(out, 0, (size_t)W * sizeof *out); return; }
  PpuRegs *r = R(p);
  static Line L;
  L.wide = wide;
  L.w = W;
  int ext[5];
  for (int i = 0; i < 5; i++) ext[i] = (E->wide_layers >> i & 1) ? wide : 0;

  static const uint8_t bpp_tab[8][4] = {
    {2,2,2,2}, {4,4,2,0}, {4,4,0,0}, {8,4,0,0}, {8,2,0,0}, {4,2,0,0}, {4,0,0,0}, {8,0,0,0},
  };
  int mode = r->mode;
  bool hires = mode == 5 || mode == 6, opt = mode == 2 || mode == 4 || mode == 6;
  uint8_t lmask = E->layer_mask ? E->layer_mask : 0x1F;
  uint8_t tm_r = r->tm & lmask, ts_r = r->ts & lmask;
  uint8_t enabled = tm_r | ts_r;
  bool extbg = mode == 7 && (r->setini & 0x40);

  bool bg_on[4] = {0};
  for (int i = 0; i < 4; i++) bg_on[i] = bpp_tab[mode][i] && (enabled >> i & 1);
  if (mode == 7) {
    bg_on[1] = extbg && (enabled & 2);
    if (bg_on[0] || bg_on[1]) render_m7(p, r, line, &L, bg_on[0], bg_on[1], ext[0]);
  } else {
    for (int i = 0; i < 4; i++)
      if (bg_on[i]) render_bg(p, r, i, line, bpp_tab[mode][i], hires, opt, &L, ext[i]);
  }
  for (int i = 0; i < 4; i++)
    if (bg_on[i] && (r->mosaic_en >> i & 1) && r->mosaic_size > 1) mosaic_h(&L, i, r->mosaic_size);

  if (ext[4] || E->no_sprite_limit) render_obj_ext(p, r, line, &L, ext[4], E->no_sprite_limit);
  else render_obj_hw(p, r, line, &L);
  bool obj_on = enabled & 0x10;
  uint8_t need = (r->tmw | r->tsw) & enabled;
  int clipsel = r->cgwsel >> 6, mathsel = r->cgwsel >> 4 & 3;
  if (clipsel == 1 || clipsel == 2 || mathsel == 1 || mathsel == 2) need |= 0x20;
  else memset(L.win[5], 0, (size_t)W);
  render_windows(r, &L, need, E->wide_win_center);

  uint8_t zbg[4][2], zobj[4];
  build_z(r, zbg, zobj);

  uint16_t mcol[MW], scol[MW];
  uint8_t mz[MW], sz[MW], ml[MW], sl[MW];
  uint16_t back = p->cgram[0];
  for (int x = 0; x < W; x++) {
    mcol[x] = back; mz[x] = 0; ml[x] = L_BACK;
    scol[x] = r->fixed; sz[x] = 0; sl[x] = L_BACK;
  }

  for (int i = 0; i < 5; i++) {
    bool on = i < 4 ? bg_on[i] : obj_on;
    if (!on) continue;
    bool tm = tm_r >> i & 1, ts = ts_r >> i & 1;
    bool tmw = r->tmw >> i & 1, tsw = r->tsw >> i & 1;
    const uint8_t *pr = L.pri[i], *win = L.win[i];
    const uint16_t *cc = L.col[i];
    for (int x = 0; x < W; x++) {
      if (!pr[x]) continue;
      uint8_t z = i < 4 ? zbg[i][pr[x] - 1] : zobj[pr[x] - 1];
      uint8_t lid = i < 4 ? i : (L.objmath[x] ? L_OBJ : L_OBJNM);
      if (tm && z > mz[x] && !(tmw && win[x])) { mz[x] = z; mcol[x] = cc[x]; ml[x] = lid; }
      if (ts && z > sz[x] && !(tsw && win[x])) { sz[x] = z; scol[x] = cc[x]; sl[x] = lid; }
    }
  }

  uint32_t lut[32];
  for (int i = 0; i < 32; i++) lut[i] = (uint32_t)((i << 3) | (i >> 2)) * p->brightness / 15;

  bool usesub = r->cgwsel & 2, sub = r->cgadsub & 0x80, halfbit = r->cgadsub & 0x40;
  const uint8_t *cw = L.win[5];
  for (int x = 0; x < W; x++) {
    uint16_t c = mcol[x];
    bool black = region(clipsel, cw[x]);
    if (black) c = 0;
    if (ml[x] != L_OBJNM && (r->cgadsub >> ml[x] & 1) && !region(mathsel, cw[x])) {
      bool half = halfbit && !black;
      uint16_t o;
      if (!usesub) o = r->fixed;
      else if (sl[x] == L_BACK) { o = r->fixed; half = false; }
      else o = scol[x];
      c = blend(c, o, sub, half);
    }
    out[x] = lut[c >> 10 & 31] | lut[c >> 5 & 31] << 8 | lut[c & 31] << 16;
  }
}
