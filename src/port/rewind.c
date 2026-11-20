#include "rewind.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//Snapshot blob: the Snes struct without the PPU framebuffer. Host pointers
//inside it are ignored on restore (snes_state_load re-binds them).

typedef struct { uint8_t *data; size_t len; } Delta;

static size_t g_frame_off, g_frame_len, g_blob_len;
static uint8_t *g_head, *g_cur;   //latest recorded snapshot, scratch
static bool g_have_head;
static Delta *g_ring;
static int g_cap, g_first, g_n;
static size_t g_bytes;
static unsigned g_tick;

static void blob_from(const Snes *s, uint8_t *out) {
  const uint8_t *p = (const uint8_t *)s;
  memcpy(out, p, g_frame_off);
  memcpy(out + g_frame_off, p + g_frame_off + g_frame_len, sizeof(Snes) - g_frame_off - g_frame_len);
}

void rw_init(void) {
  g_frame_off = offsetof(Snes, ppu) + offsetof(Ppu, frame);
  g_frame_len = sizeof(((Ppu *)0)->frame);
  g_blob_len = sizeof(Snes) - g_frame_len;
  if (!g_head) {
    g_head = malloc(g_blob_len);
    g_cur = malloc(g_blob_len);
  }
  if (!g_cap) rw_set_capacity(20);
}

void rw_clear(void) {
  for (int i = 0; i < g_n; i++) free(g_ring[(g_first + i) % g_cap].data);
  g_first = g_n = 0;
  g_bytes = 0;
  g_have_head = false;
  g_tick = 0;
}

void rw_set_capacity(int seconds) {
  int cap = seconds * 60 / RW_INTERVAL;
  if (cap < 1) cap = 1;
  if (cap == g_cap) return;
  rw_clear();
  free(g_ring);
  g_ring = calloc((size_t)cap, sizeof(Delta));
  g_cap = cap;
}

//Encoding of a XOR buffer: repeated [u32 zero run][u32 literal count][literals].
static uint8_t *encode(const uint8_t *a, const uint8_t *b, size_t n, size_t *out_len) {
  size_t cap = 4096, len = 0;
  uint8_t *out = malloc(cap);
  size_t i = 0;
  while (i < n) {
    size_t z = i;
    while (z < n && a[z] == b[z]) z++;
    size_t l = z;
    //A literal run ends at 8 equal bytes in a row.
    while (l < n) {
      if (a[l] == b[l]) {
        size_t k = l;
        while (k < n && k - l < 8 && a[k] == b[k]) k++;
        if (k - l >= 8 || k == n) break;
        l = k;
      } else {
        l++;
      }
    }
    uint32_t zr = (uint32_t)(z - i), lr = (uint32_t)(l - z);
    if (len + 8 + lr > cap) {
      while (len + 8 + lr > cap) cap *= 2;
      out = realloc(out, cap);
    }
    memcpy(out + len, &zr, 4);
    memcpy(out + len + 4, &lr, 4);
    len += 8;
    for (uint32_t k = 0; k < lr; k++) out[len + k] = a[z + k] ^ b[z + k];
    len += lr;
    i = l;
  }
  *out_len = len;
  return realloc(out, len ? len : 1);
}

static void apply(uint8_t *buf, const Delta *d) {
  size_t i = 0, p = 0;
  while (p + 8 <= d->len) {
    uint32_t zr, lr;
    memcpy(&zr, d->data + p, 4);
    memcpy(&lr, d->data + p + 4, 4);
    p += 8;
    i += zr;
    for (uint32_t k = 0; k < lr && i < g_blob_len; k++) buf[i++] ^= d->data[p + k];
    p += lr;
  }
}

void rw_record(Snes *s) {
  if (!g_head) return;
  if (g_tick++ % RW_INTERVAL) return;
  blob_from(s, g_cur);
  if (g_have_head) {
    Delta d;
    d.data = encode(g_head, g_cur, g_blob_len, &d.len);
    if (g_n == g_cap) {
      g_bytes -= g_ring[g_first].len;
      free(g_ring[g_first].data);
      g_first = (g_first + 1) % g_cap;
      g_n--;
    }
    g_ring[(g_first + g_n) % g_cap] = d;
    g_n++;
    g_bytes += d.len;
  }
  uint8_t *t = g_head;
  g_head = g_cur;
  g_cur = t;
  g_have_head = true;
}

bool rw_step_back(Snes *s) {
  if (!g_have_head || g_n == 0) return false;
  Delta *d = &g_ring[(g_first + g_n - 1) % g_cap];
  apply(g_head, d);
  g_bytes -= d->len;
  free(d->data);
  g_n--;
  //Rebuild a full state image around the blob (framebuffer from s) and load
  //it, which also restores the host pointers.
  size_t n = snes_state_size();
  uint8_t *img = malloc(n);
  snes_state_save(s, img);
  uint8_t *body = img + (n - sizeof(Snes));
  memcpy(body, g_head, g_frame_off);
  memcpy(body + g_frame_off + g_frame_len, g_head + g_frame_off, sizeof(Snes) - g_frame_off - g_frame_len);
  bool ok = snes_state_load(s, img);
  free(img);
  g_tick = 1;  //next record lands RW_INTERVAL frames later
  return ok;
}

int rw_count(void) { return g_n; }
size_t rw_bytes(void) { return g_bytes; }
