#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/stage.h"
#include "game/recomp_rt.h"

//Renders battle arenas as the battle view shows them: the whole box,
//stitched from view frames, with the spawn points marked (white squares) and
//the collision optionally drawn over it (ARENA_COLLISION=1: solid blue,
//slopes green, ladders yellow, hazards red).
//usage: trio-arena-preview ROMDIR OUTPREFIX [ARENA...]   (default: all)
//Writes OUTPREFIX-NN.ppm.

static uint8_t *load(const char *dir, int g, uint32_t *size) { return snes_rom_find(dir, g, size); }

static void preview(const ArenaDef *d, const uint8_t *rom, uint32_t size, const char *path) {
  static ArenaStage st;
  if (!stage_boot(&st, d, rom, size, true)) { fprintf(stderr, "%s: boot failed\n", d->name); return; }
  st.render = true;   //the display copy's own picture is what this shows
  int W = d->x1 - d->x0, H = d->y1 - d->y0;
  uint32_t *img = calloc((size_t)W * H, 4);
  //256 x 224 tiles of the box, each from a view frame at that camera.
  for (int ty = d->y0; ty < d->y1; ty += 224)
    for (int tx = d->x0; tx < d->x1; tx += 256) {
      int cx = tx, cy = ty;
      for (int k = 0; k < 200; k++) {
        stage_view(&st, cx, cy, 256, false);
        if (st.pcx == st.cx && st.pcy == st.cy && k > 2) break;
      }
      stage_view(&st, cx, cy, 256, false);
      Snes *s = st.view;
      int ox = st.pcx, oy = st.pcy;
      for (int y = 0; y < 224; y++)
        for (int x = 0; x < 256; x++) {
          int X = ox + x - d->x0, Y = oy + y - d->y0;
          if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
          img[(size_t)Y * W + X] = s->ppu.frame[y][x + (s->ppu.out_w - 256) / 2];
        }
    }
  if (getenv("ARENA_COLLISION"))
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        int a = trio_geo_at(&st.geo, d->x0 + x, d->y0 + y);
        uint32_t c = trio_at_solid(a) ? 0x0000FF : (a >= 1 && a <= 0x0C) ? 0x00FF00 : (a == 0x12 || a == 0x13) ? 0xFFFF00 : a >= 0x3E ? 0xFF0000 : 0;
        if (c && ((x + y) & 3) == 0) img[(size_t)y * W + x] = c;
      }
  for (int i = 0; i < 4; i++)
    for (int y = -6; y < 0; y++)
      for (int x = -3; x <= 3; x++) {
        int X = st.spawn_x[i] - d->x0 + x, Y = st.spawn_y[i] - d->y0 + y;
        if (X >= 0 && Y >= 0 && X < W && Y < H) img[(size_t)Y * W + X] = 0xFFFFFF;
      }
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (size_t i = 0; i < (size_t)W * H; i++) { fputc((int)(img[i] >> 16 & 255), f); fputc((int)(img[i] >> 8 & 255), f); fputc((int)(img[i] & 255), f); }
  fclose(f);
  free(img);
  printf("%s: %s spawns", path, d->name);
  for (int i = 0; i < 4; i++) printf(" (%d,%d)", st.spawn_x[i], st.spawn_y[i]);
  printf("\n");
  stage_free(&st);
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: trio-arena-preview ROMDIR OUTPREFIX [ARENA...]\n"); return 2; }
  uint8_t *roms[4] = {0};
  uint32_t sizes[4] = {0};
  for (int g = 1; g <= 3; g++) roms[g] = load(argv[1], g, &sizes[g]);
  snes_skip_render = true;
  for (int i = 0; i < arena_ndefs; i++) {
    bool want = argc == 3;
    for (int a = 3; a < argc; a++) want |= atoi(argv[a]) == i;
    const ArenaDef *d = &arena_defs[i];
    if (!want || !roms[d->game]) continue;
    char path[512];
    snprintf(path, sizeof path, "%s-%02d.ppm", argv[2], i);
    preview(d, roms[d->game], sizes[d->game], path);
  }
  return 0;
}
