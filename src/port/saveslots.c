#include "saveslots.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define ss_mkdir(p) _mkdir(p)
#else
#define ss_mkdir(p) mkdir(p, 0755)
#endif

#define META_VERSION 1
#define STATE_MAGIC 0x4D583331u   //snes.c: state header word 0
#define THUMB_ROWS 224            //framebuffer rows sampled; overscan lines excluded

//NTSC frames per minute (60.0988 Hz).
#define FRAMES_PER_MIN 3606
#define AUTO_SETTLE 30

static char g_dir[512] = "saves";
static const uint8_t *g_rom;
static uint32_t g_rom_size;
static char g_sha1[41];
static SlotInfo g_info[SS_COUNT];
static bool g_valid[SS_COUNT];
static uint32_t g_gen;

static struct {
  bool pending;            //stage entered, not autosaved yet
  int settle;              //consecutive steady frames
  uint64_t last_frame;     //frame_count at the last autosave or rebase
} g_auto = {false, 0, 0};

//Location label written with the next save (set by the frontend).
static char g_location[40] = "-";
void ss_set_location(const char *loc) { snprintf(g_location, sizeof(g_location), "%s", loc && *loc ? loc : "-"); }

//---- SHA-1 (FIPS 180-1) ----

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_hex(const uint8_t *data, size_t n, char out[41]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) sha1_block(h, data + i);
  uint8_t tail[128] = {0};
  size_t r = n - i;
  memcpy(tail, data + i, r);
  tail[r] = 0x80;
  size_t tn = r < 56 ? 64 : 128;
  uint64_t bits = (uint64_t)n * 8;
  for (int k = 0; k < 8; k++) tail[tn - 1 - k] = (uint8_t)(bits >> (8 * k));
  sha1_block(h, tail);
  if (tn == 128) sha1_block(h, tail + 64);
  for (int k = 0; k < 5; k++) snprintf(out + 8 * k, 9, "%08x", h[k]);
}

//---- PNG ----

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      crc_table[i] = v;
    }
  c = ~c;
  while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

static uint32_t adler32(const uint8_t *p, size_t n) {
  uint32_t a = 1, b = 0;
  while (n--) { a = (a + *p++) % 65521; b = (b + a) % 65521; }
  return b << 16 | a;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t rbe32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3]; }

static bool chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t hdr[8];
  be32(hdr, n);
  memcpy(hdr + 4, type, 4);
  uint32_t crc = crc32_update(crc32_update(0, hdr + 4, 4), data, n);
  uint8_t tail[4];
  be32(tail, crc);
  return fwrite(hdr, 1, 8, f) == 8 && (!n || fwrite(data, 1, n, f) == n) && fwrite(tail, 1, 4, f) == 4;
}

static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

bool png_write_rgb(const char *path, const uint8_t *rgb, int w, int h) {
  size_t stride = (size_t)w * 3 + 1;
  size_t raw_n = stride * (size_t)h;
  uint8_t *raw = malloc(raw_n);
  //zlib: 2-byte header, stored blocks of <= 65535 bytes (5-byte header each), adler32.
  size_t blocks = raw_n / 65535 + 1;
  size_t z_n = 2 + blocks * 5 + raw_n + 4;
  uint8_t *z = malloc(z_n);
  if (!raw || !z) { free(raw); free(z); return false; }
  for (int y = 0; y < h; y++) {
    raw[stride * y] = 0;
    memcpy(raw + stride * y + 1, rgb + (size_t)w * 3 * y, (size_t)w * 3);
  }
  uint8_t *o = z;
  *o++ = 0x78; *o++ = 0x01;   //deflate, 32K window, no dictionary; (0x7801 % 31 == 0)
  size_t pos = 0;
  do {
    size_t len = raw_n - pos > 65535 ? 65535 : raw_n - pos;
    *o++ = pos + len == raw_n;  //BFINAL, BTYPE 00
    *o++ = (uint8_t)len; *o++ = (uint8_t)(len >> 8);
    *o++ = (uint8_t)~len; *o++ = (uint8_t)(~len >> 8);
    memcpy(o, raw + pos, len);
    o += len;
    pos += len;
  } while (pos < raw_n);
  be32(o, adler32(raw, raw_n));
  o += 4;
  uint8_t ihdr[13];
  be32(ihdr, (uint32_t)w);
  be32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
  FILE *f = fopen(path, "wb");
  bool ok = f && fwrite(png_sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
            chunk(f, "IDAT", z, (uint32_t)(o - z)) && chunk(f, "IEND", NULL, 0);
  if (f && fclose(f)) ok = false;
  free(raw);
  free(z);
  return ok;
}

static uint8_t *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = len > 0 ? malloc((size_t)len) : NULL;
  if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
  fclose(f);
  *n = buf ? (size_t)len : 0;
  return buf;
}

bool png_read_rgb(const char *path, uint8_t **out, int *ow, int *oh) {
  size_t n;
  uint8_t *f = slurp(path, &n);
  uint8_t *z = NULL, *raw = NULL, *rgb = NULL;
  size_t z_n = 0;
  uint32_t w = 0, h = 0;
  bool ok = false, have_hdr = false, end = false;
  if (!f || n < 8 || memcmp(f, png_sig, 8)) goto done;
  for (size_t p = 8; p + 12 <= n && !end;) {
    uint32_t len = rbe32(f + p);
    if (len > n - p - 12) goto done;
    const uint8_t *type = f + p + 4, *data = f + p + 8;
    if (crc32_update(0, type, len + 4) != rbe32(data + len)) goto done;
    if (!memcmp(type, "IHDR", 4)) {
      if (len != 13) goto done;
      w = rbe32(data); h = rbe32(data + 4);
      if (!w || !h || w > 2048 || h > 2048 || data[8] != 8 || data[9] != 2 || data[10] || data[11] || data[12])
        goto done;
      have_hdr = true;
    } else if (!memcmp(type, "IDAT", 4)) {
      uint8_t *nz = realloc(z, z_n + len + 1);
      if (!nz) goto done;
      z = nz;
      memcpy(z + z_n, data, len);
      z_n += len;
    } else if (!memcmp(type, "IEND", 4)) {
      end = true;
    }
    p += 12 + len;
  }
  if (!have_hdr || !end || z_n < 6 || (z[0] & 0x0F) != 8 || (z[0] << 8 | z[1]) % 31 || (z[1] & 0x20)) goto done;
  size_t stride = (size_t)w * 3 + 1, raw_n = stride * h, got = 0;
  raw = malloc(raw_n);
  if (!raw) goto done;
  size_t p = 2;
  for (bool final = false; !final;) {
    if (p + 5 > z_n) goto done;
    uint8_t hb = z[p];
    final = hb & 1;
    if ((hb >> 1 & 3) != 0) goto done;  //compressed blocks: not produced by png_write_rgb
    size_t len = z[p + 1] | z[p + 2] << 8, nlen = z[p + 3] | z[p + 4] << 8;
    if ((len ^ 0xFFFF) != nlen || p + 5 + len > z_n || got + len > raw_n) goto done;
    memcpy(raw + got, z + p + 5, len);
    got += len;
    p += 5 + len;
  }
  if (got != raw_n || p + 4 > z_n || rbe32(z + p) != adler32(raw, raw_n)) goto done;
  rgb = malloc((size_t)w * h * 3);
  if (!rgb) goto done;
  for (uint32_t y = 0; y < h; y++) {
    if (raw[stride * y] != 0) goto done;
    memcpy(rgb + (size_t)w * 3 * y, raw + stride * y + 1, (size_t)w * 3);
  }
  *out = rgb;
  *ow = (int)w;
  *oh = (int)h;
  rgb = NULL;
  ok = true;
done:
  free(f); free(z); free(raw); free(rgb);
  return ok;
}

//---- files ----

static bool write_atomic(const char *path, const void *data, size_t n) {
  char tmp[600];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  if (fclose(f)) ok = false;
  if (!ok) { remove(tmp); return false; }
#ifdef _WIN32
  remove(path);
#endif
  return rename(tmp, path) == 0;
}

static bool finish_atomic(const char *tmp, const char *path, bool ok) {
  if (!ok) { remove(tmp); return false; }
#ifdef _WIN32
  remove(path);
#endif
  return rename(tmp, path) == 0;
}

void ss_path(int slot, const char *ext, char *out, size_t n) {
  if (slot == SS_QUICK) snprintf(out, n, "%s/quick.%s", g_dir, ext);
  else if (slot <= SS_MANUAL) snprintf(out, n, "%s/slot%d.%s", g_dir, slot, ext);
  else snprintf(out, n, "%s/auto%d.%s", g_dir, slot - SS_AUTO_FIRST + 1, ext);
}

const char *ss_name(int slot, char *buf, size_t n) {
  if (slot == SS_QUICK) snprintf(buf, n, "Quick");
  else if (slot <= SS_MANUAL) snprintf(buf, n, "Slot %d", slot);
  else snprintf(buf, n, "Auto %d", slot - SS_AUTO_FIRST + 1);
  return buf;
}

const char *ss_rom_sha1(void) { return g_sha1; }

void ss_init(const char *dir, const uint8_t *rom, uint32_t rom_size) {
  snprintf(g_dir, sizeof(g_dir), "%s", dir && *dir ? dir : ".");
  ss_mkdir(g_dir);
  g_rom = rom;
  g_rom_size = rom_size;
  if (rom) sha1_hex(rom, rom_size, g_sha1);
  else g_sha1[0] = 0;
  char path[600];
  ss_path(SS_QUICK, "state", path, sizeof(path));
  FILE *f = fopen(path, "rb");
  if (f) fclose(f);
  else {
    size_t n;
    uint8_t *legacy = slurp("quick.state", &n);
    if (legacy) write_atomic(path, legacy, n);
    free(legacy);
  }
  ss_refresh(-1);
}

//---- metadata ----

static void read_meta(int slot, SlotInfo *in) {
  char path[600], line[256];
  ss_path(slot, "txt", path, sizeof(path));
  FILE *f = fopen(path, "r");
  if (!f) return;
  while (fgets(line, sizeof(line), f)) {
    char key[32], val[128];
    if (line[0] == '#' || sscanf(line, " %31[^= ] = %127[^\r\n]", key, val) != 2) continue;
    if (!strcmp(key, "timestamp")) in->timestamp = strtoll(val, NULL, 10);
    else if (!strcmp(key, "serial")) in->serial = (uint32_t)strtoul(val, NULL, 10);
    else if (!strcmp(key, "rom_sha1")) snprintf(in->rom_sha1, sizeof(in->rom_sha1), "%.40s", val);
    else if (!strcmp(key, "location")) snprintf(in->location, sizeof(in->location), "%.39s", val);
  }
  fclose(f);
}

static bool write_meta(int slot, const SlotInfo *in, int tw, int th) {
  char path[600], tmp[610];
  ss_path(slot, "txt", path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f) return false;
  char when[64] = "";
  time_t t = (time_t)in->timestamp;
  struct tm *tm = localtime(&t);
  if (tm) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
  fprintf(f, "# the three games port save slot (%s)\n", when);
  fprintf(f, "version = %d\n", META_VERSION);
  fprintf(f, "timestamp = %lld\n", (long long)in->timestamp);
  fprintf(f, "serial = %u\n", (unsigned)in->serial);
  fprintf(f, "frames = %llu\n", (unsigned long long)in->frames);
  fprintf(f, "location = %s\n", in->location);
  fprintf(f, "rom_sha1 = %s\n", in->rom_sha1);
  fprintf(f, "state_size = %u\n", (unsigned)in->state_size);
  fprintf(f, "state_magic = %08x\n", (unsigned)in->state_magic);
  fprintf(f, "thumb = %dx%d\n", tw, th);
  bool ok = !ferror(f);
  if (fclose(f)) ok = false;
  return finish_atomic(tmp, path, ok);
}

static void scan(int slot) {
  SlotInfo *in = &g_info[slot];
  uint32_t gen = in->gen;
  memset(in, 0, sizeof(*in));
  in->gen = gen;
  snprintf(in->location, sizeof(in->location), "-");
  g_valid[slot] = true;
  char path[600];
  ss_path(slot, "state", path, sizeof(path));
  FILE *f = fopen(path, "rb");
  if (!f) return;
  in->exists = true;
  struct stat st;
  if (!stat(path, &st)) in->timestamp = (int64_t)st.st_mtime;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  in->state_size = len > 0 ? (uint32_t)len : 0;
  uint32_t hdr[2] = {0, 0};
  fseek(f, 0, SEEK_SET);
  bool hdr_ok = fread(hdr, 4, 2, f) == 2;
  in->state_magic = hdr[0];
  if (!hdr_ok || hdr[0] != STATE_MAGIC) in->why = "not a state file";
  else if (hdr[1] != sizeof(Snes) || in->state_size != snes_state_size()) in->why = "other build (size mismatch)";
  else {
    Snes *tmp = malloc(sizeof(Snes));
    if (tmp && fread(tmp, sizeof(Snes), 1, f) == 1) {
      in->frames = tmp->frame_count;
      in->compatible = true;
    } else {
      in->why = "unreadable";
    }
    free(tmp);
  }
  fclose(f);
  read_meta(slot, in);
  if (in->compatible && in->rom_sha1[0] && g_sha1[0] && strcmp(in->rom_sha1, g_sha1)) {
    in->compatible = false;
    in->why = "different ROM";
  }
}

void ss_refresh(int slot) {
  for (int i = 0; i < SS_COUNT; i++)
    if (slot < 0 || slot == i) { g_valid[i] = false; g_info[i].gen = ++g_gen; }
}

const SlotInfo *ss_info(int slot) {
  if (slot < 0 || slot >= SS_COUNT) return NULL;
  if (!g_valid[slot]) scan(slot);
  return &g_info[slot];
}

//Framebuffer rows 0..THUMB_ROWS-1, all out_w columns, 2x2 box filter.
static uint8_t *make_thumb(const Snes *s, int *tw, int *th) {
  int w = s->ppu.out_w > 0 && s->ppu.out_w <= PPU_MAXW ? s->ppu.out_w : PPU_W;
  *tw = w / 2;
  *th = THUMB_ROWS / 2;
  uint8_t *rgb = malloc((size_t)*tw * *th * 3);
  if (!rgb) return NULL;
  for (int y = 0; y < *th; y++)
    for (int x = 0; x < *tw; x++) {
      uint32_t r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
          uint32_t c = s->ppu.frame[2 * y + dy][2 * x + dx];
          r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF;
        }
      uint8_t *o = rgb + ((size_t)y * *tw + x) * 3;
      o[0] = (uint8_t)(r / 4); o[1] = (uint8_t)(g / 4); o[2] = (uint8_t)(b / 4);
    }
  return rgb;
}

bool ss_save(int slot, Snes *s) {
  if (slot < 0 || slot >= SS_COUNT) return false;
  ss_mkdir(g_dir);
  size_t n = snes_state_size();
  uint8_t *buf = malloc(n);
  if (!buf) return false;
  snes_state_save(s, buf);
  char path[600];
  ss_path(slot, "state", path, sizeof(path));
  bool ok = write_atomic(path, buf, n);
  free(buf);
  if (ok) {
    int tw, th;
    uint8_t *thumb = make_thumb(s, &tw, &th);
    ss_path(slot, "png", path, sizeof(path));
    char tmp[610];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!thumb || !finish_atomic(tmp, path, png_write_rgb(tmp, thumb, tw, th))) remove(path);
    free(thumb);
    SlotInfo in;
    memset(&in, 0, sizeof(in));
    in.timestamp = (int64_t)time(NULL);
    for (int i = 0; i < SS_COUNT; i++)
      if (i != slot && ss_info(i)->serial > in.serial) in.serial = ss_info(i)->serial;
    in.serial++;
    in.frames = s->frame_count;
    snprintf(in.location, sizeof(in.location), "%s", g_location);
    snprintf(in.rom_sha1, sizeof(in.rom_sha1), "%s", g_sha1);
    in.state_size = (uint32_t)n;
    in.state_magic = STATE_MAGIC;
    write_meta(slot, &in, thumb ? tw : 0, thumb ? th : 0);
  }
  ss_refresh(slot);
  return ok;
}

const char *ss_load(int slot, Snes *s) {
  const SlotInfo *in = ss_info(slot);
  if (!in) return "no such slot";
  if (!in->exists) return "empty";
  if (!in->compatible) return in->why ? in->why : "incompatible";
  char path[600];
  ss_path(slot, "state", path, sizeof(path));
  size_t n;
  uint8_t *buf = slurp(path, &n);
  const char *err = NULL;
  if (!buf || n != snes_state_size()) err = "unreadable";
  else if (!snes_state_load(s, buf)) err = "other build (size mismatch)";
  free(buf);
  if (err) ss_refresh(slot);
  return err;
}

bool ss_delete(int slot) {
  if (slot < 0 || slot >= SS_COUNT) return false;
  static const char *ext[] = {"state", "png", "txt"};
  char path[600];
  bool ok = true;
  for (int i = 0; i < 3; i++) {
    ss_path(slot, ext[i], path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) continue;
    fclose(f);
    if (remove(path)) ok = false;
  }
  ss_refresh(slot);
  return ok;
}

bool ss_do(int op, int slot, Snes *s, char *msg, size_t n) {
  char name[16];
  ss_name(slot, name, sizeof(name));
  switch (op) {
  case SS_OP_SAVE:
    if (ss_save(slot, s)) snprintf(msg, n, "Saved: %s", name);
    else snprintf(msg, n, "%s: save failed", name);
    return false;
  case SS_OP_LOAD: {
    const char *err = ss_load(slot, s);
    if (err) { snprintf(msg, n, "%s: %s", name, err); return false; }
    snprintf(msg, n, "Loaded: %s", name);
    return true;
  }
  case SS_OP_DELETE:
    snprintf(msg, n, ss_delete(slot) ? "Deleted: %s" : "%s: delete failed", name);
    return false;
  }
  return false;
}

bool ss_thumb(int slot, uint8_t **rgb, int *w, int *h) {
  if (slot < 0 || slot >= SS_COUNT) return false;
  char path[600];
  ss_path(slot, "png", path, sizeof(path));
  return png_read_rgb(path, rgb, w, h);
}

//---- autosave ----

//Save order: serial, then timestamp.
static bool older(const SlotInfo *a, const SlotInfo *b) {
  if (a->serial != b->serial) return a->serial < b->serial;
  return a->timestamp < b->timestamp;
}

int ss_auto_pick(void) {
  int best = SS_AUTO_FIRST;
  for (int i = SS_AUTO_FIRST; i < SS_COUNT; i++) {
    const SlotInfo *in = ss_info(i);
    if (!in->exists) return i;
    if (older(in, ss_info(best))) best = i;
  }
  return best;
}

int ss_auto_newest(void) {
  int best = -1;
  for (int i = SS_AUTO_FIRST; i < SS_COUNT; i++) {
    const SlotInfo *in = ss_info(i);
    if (in->exists && in->compatible && (best < 0 || older(ss_info(best), in))) best = i;
  }
  return best;
}

void ss_auto_rebase(const Snes *s, bool stage_known) {
  (void)stage_known;
  g_auto.pending = false;
  g_auto.settle = 0;
  g_auto.last_frame = s->frame_count;
}

void ss_auto_stage_entered(void) { g_auto.pending = true; g_auto.settle = 0; }

int ss_auto_tick(Snes *s, bool enabled, int minutes, bool in_area) {
  if (!enabled) return -1;
  if (!in_area || s->ppu.forced_blank || s->ppu.brightness != 15) { g_auto.settle = 0; return -1; }
  if (++g_auto.settle < AUTO_SETTLE) return -1;
  bool timed = minutes > 0 && s->frame_count - g_auto.last_frame >= (uint64_t)minutes * FRAMES_PER_MIN;
  if (!g_auto.pending && !timed) return -1;
  int slot = ss_auto_pick();
  g_auto.pending = false;
  g_auto.last_frame = s->frame_count;
  return ss_save(slot, s) ? slot : -1;
}
