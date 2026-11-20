#include "recomp_rt.h"
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>

_Static_assert(offsetof(Snes, cpu) == 0, "recomp_gen.h casts Cpu * to Snes *");

uint64_t rt_insn_count;
uint64_t rt_interp_count;
bool rt_overrides_on = true;

static jmp_buf rt_jb;

typedef struct RtGame {
  const RtEntry *tab;
  const uint32_t *n;
  //Per 64 KiB bank: index + 1 into tab for (pc16 << 2 | m << 1 | x), 0 = none.
  uint16_t *index[256];
  bool built;
} RtGame;

static RtGame games[4] = {
  {0},
  {.tab = rt_dispatch_g1, .n = &rt_dispatch_g1_n},
  {.tab = rt_dispatch_g2, .n = &rt_dispatch_g2_n},
  {.tab = rt_dispatch_g3, .n = &rt_dispatch_g3_n},
};

static RtGame *game_of(const Snes *s) { return s->game >= 1 && s->game <= 3 ? &games[s->game] : &games[0]; }

bool rt_has_game(int game) { return game >= 1 && game <= 3 && *games[game].n > 0; }

static void build_index(RtGame *g) {
  uint32_t n = g->n ? *g->n : 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t k = g->tab[i].key;
    uint8_t bank = (uint8_t)(k >> 18);
    if (!g->index[bank]) {
      g->index[bank] = calloc(0x40000, sizeof(uint16_t));
      if (!g->index[bank]) abort();
    }
    //Indices above 0xFFFE fall back to binary search.
    if (i + 1 < 0xFFFF) g->index[bank][k & 0x3FFFF] = (uint16_t)(i + 1);
  }
  g->built = true;
}

static const RtEntry *lookup(const Snes *s) {
  const Cpu *c = &s->cpu;
  const RtGame *g = game_of(s);
  //P.M is bit 5, P.X bit 4.
  uint32_t k = ((uint32_t)c->pb << 16 | c->pc) << 2 | ((c->p >> 4) & 3);
  const uint16_t *t = g->index[c->pb];
  if (!t) return NULL;
  uint16_t i = t[k & 0x3FFFF];
  if (i) return &g->tab[i - 1];
  uint32_t n = *g->n;
  if (n < 0xFFFF) return NULL;
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (g->tab[mid].key < k) lo = mid + 1; else hi = mid;
  }
  return lo < n && g->tab[lo].key == k ? &g->tab[lo] : NULL;
}

RT_NORETURN void rt_exit(void) { longjmp(rt_jb, 1); }

void rt_dyn(Snes *s, uint32_t ret) {
  const RtEntry *e = lookup(s);
  if (!e) rt_exit();
  e->fn(s, ret, e->ent);
}

bool rt_hook(Snes *s) {
  Cpu *c = &s->cpu;
  if (c->e || c->waiting || c->stopped) return false;
  if (c->nmi_pending || (c->irq_line && !(c->p & CPU_I))) return false;
  const RtEntry *e = lookup(s);
  if (!e) { rt_interp_count++; return false; }
  if (!setjmp(rt_jb)) e->fn(s, RT_ANY, e->ent);
  return true;
}

void rt_install(Snes *s, bool on) {
  RtGame *g = game_of(s);
  if (on && g->n && !g->built) build_index(g);
  s->exec_hook = on && g->n && *g->n ? rt_hook : NULL;
}

bool rt_enabled_by_env(void) {
  const char *v = getenv("TRIO_RECOMP");
  const char *o = getenv("TRIO_OVERRIDES");
  if (o && o[0] == '0' && o[1] == 0) rt_overrides_on = false;
  return !(v && v[0] == '0' && v[1] == 0);
}

//Native mode, 16-bit S in bank 0 low RAM ($0000-$1FFF = WRAM $7E:0000).
//No bus cycles are charged.
void rt_push8(Snes *s, uint8_t v) {
  s->wram[s->cpu.s & 0x1FFF] = v;
  s->cpu.s--;
}

uint8_t rt_pull8(Snes *s) {
  s->cpu.s++;
  return s->wram[s->cpu.s & 0x1FFF];
}

void rt_rts(Snes *s) {
  uint16_t lo = rt_pull8(s);
  s->cpu.pc = (uint16_t)((lo | rt_pull8(s) << 8) + 1);
}

uint16_t rt_wram16(const Snes *s, uint32_t a) {
  return (uint16_t)(s->wram[a & 0x1FFFF] | s->wram[(a + 1) & 0x1FFFF] << 8);
}

void rt_set_wram16(Snes *s, uint32_t a, uint16_t v) {
  s->wram[a & 0x1FFFF] = (uint8_t)v;
  s->wram[(a + 1) & 0x1FFFF] = (uint8_t)(v >> 8);
}

void rt_set_nz(Snes *s, uint16_t v, bool index) {
  bool wide = !(s->cpu.p & (index ? CPU_X : CPU_M));
  if (!wide) v &= 0xFF;
  s->cpu.p &= (uint8_t)~(CPU_N | CPU_Z);
  if (!v) s->cpu.p |= CPU_Z;
  if (v & (wide ? 0x8000 : 0x80)) s->cpu.p |= CPU_N;
}
