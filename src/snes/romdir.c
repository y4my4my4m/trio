#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define strcasecmp _stricmp
#else
#include <dirent.h>
#include <strings.h>
#endif
#include "snes.h"

static uint8_t *image_of(const char *path, int game, uint32_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *raw = n > 0 ? malloc((size_t)n) : NULL;
  bool ok = raw && fread(raw, 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  uint8_t *img = ok ? snes_rom_image(raw, (size_t)n, out_size) : NULL;
  free(raw);
  if (img && snes_rom_game(img, *out_size) != game) { free(img); img = NULL; }
  return img;
}

uint8_t *snes_rom_find(const char *dir, int game, uint32_t *out_size) {
  char path[1024];
  snprintf(path, sizeof path, "%s/g%d.sfc", dir, game);
  uint8_t *img = image_of(path, game, out_size);
  if (img) return img;
#ifdef _WIN32
  struct _finddata_t fd;
  snprintf(path, sizeof path, "%s/*.*", dir);
  intptr_t h = _findfirst(path, &fd);
  if (h == -1) return NULL;
  do {
    const char *name = fd.name;
#else
  DIR *dp = opendir(dir);
  if (!dp) return NULL;
  struct dirent *de;
  while ((de = readdir(dp))) {
    const char *name = de->d_name;
#endif
    size_t n = strlen(name);
    if (n < 5 || (strcasecmp(name + n - 4, ".sfc") && strcasecmp(name + n - 4, ".smc"))) continue;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if ((img = image_of(path, game, out_size))) break;
#ifdef _WIN32
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  }
  closedir(dp);
#endif
  return img;
}
