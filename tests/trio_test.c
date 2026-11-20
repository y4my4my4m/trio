#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/stage.h"
#include "game/recomp_rt.h"

//The game interface of battle mode (src/arena/trio.c) on each of the three
//games: boot into a stage, the damage routine, death, the warp-in, the
//arena collision installed in a copy of another game, Blade. Needs the ROMs
//in ROMDIR (skipped, exit 77, without any).
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *dir, int g, uint32_t *size) { return snes_rom_find(dir, g, size); }

static Snes s, t;
static ArenaStage st;

static void run(Snes *m, int frames, uint16_t pad, const ArenaStage *a) {
  for (int i = 0; i < frames; i++) {
    if (a) stage_follow(a, m);
    else trio_quiet(m);
    m->pad[0] = pad;
    m->pad[1] = 0;
    snes_run_frame(m);
  }
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "rom";
  uint8_t *rom[4] = {0};
  uint32_t size[4] = {0};
  int found = 0;
  for (int g = 1; g <= 3; g++) found += (rom[g] = load(dir, g, &size[g])) != NULL;
  if (!found) { printf("no ROMs, skipped\n"); return 77; }
  snes_skip_render = true;

  for (int g = 1; g <= 3; g++) {
    if (!rom[g]) continue;
    const TrioGame *gm = &trio_games[g];
    CHECK(trio_boot_stage(&s, rom[g], size[g], true, gm->home, NULL), "%s: boot", gm->name);
    TrioPlayer p, q;
    trio_read_player(&s, &p);
    printf("%s: stage %d x %d y %d hp %d ground %d\n", gm->name, gm->home, p.x, p.y, p.hp, p.ground);
    CHECK(p.hp == 16 && p.ground && p.action == TRIO_ACT_IDLE, "%s: standing with full HP", gm->name);
    CHECK(trio_attr(&s, p.x, p.y + 17) >= TRIO_AT_SOLID_MIN, "%s: floor under the feet (%d)", gm->name, trio_attr(&s, p.x, p.y + 17));
    CHECK(trio_attr(&s, p.x, p.y) == 0, "%s: air at the body", gm->name);

    //The game's damage routine: HP drops, hurt, knockback away, then invincible.
    snes_clone(&t, &s);
    trio_damage(&t, 3, p.x + 20, p.y);
    run(&t, 4, 0, NULL);
    trio_read_player(&t, &q);
    printf("%s: hit: hp %d action %02X vx %d\n", gm->name, q.hp, q.action, q.vx);
    CHECK(q.hp == 13, "%s: hp 16 -> 13, got %d", gm->name, q.hp);
    CHECK(q.action == TRIO_ACT_HURT, "%s: hurt action, got %02X", gm->name, q.action);
    CHECK(q.vx < 0, "%s: knocked away from an attacker on the right", gm->name);
    run(&t, 45, 0, NULL);
    trio_read_player(&t, &q);
    CHECK(q.iframes > 0, "%s: invincible after the hurt animation (%d)", gm->name, q.iframes);
    //Enough damage kills, with the death sound queued.
    uint8_t seen = t.wram[gm->snd_w] & 0x1E;
    trio_damage(&t, 40, q.x - 20, q.y);
    bool death_snd = false;
    for (int i = 0; i < 60; i++) {
      run(&t, 1, 0, NULL);
      TrioSnd sq[16];
      int n = trio_sound_take(&t, &seen, sq, 16);
      for (int k = 0; k < n; k++) death_snd |= sq[k].id == gm->sfx_death;
    }
    trio_read_player(&t, &q);
    CHECK(q.hp == 0 && q.action == TRIO_ACT_DYING, "%s: dies (hp %d action %02X)", gm->name, q.hp, q.action);
    CHECK(death_snd, "%s: death sound %02X queued", gm->name, gm->sfx_death);

    //Warp-in: from 100 px up, he beams down to the floor and is in control.
    snes_clone(&t, &s);
    trio_place(&t, p.x, p.y - 100, true);
    trio_warp_in(&t);
    seen = t.wram[gm->snd_w] & 0x1E;
    bool warp_snd = false;
    int fr = 0;
    for (; fr < 120; fr++) {
      run(&t, 1, 0, NULL);
      TrioSnd sq[16];
      int n = trio_sound_take(&t, &seen, sq, 16);
      for (int k = 0; k < n; k++) warp_snd |= sq[k].id == gm->sfx_warp;
      trio_read_player(&t, &q);
      if (q.action == TRIO_ACT_IDLE && q.ground) break;
    }
    printf("%s: warp-in landed after %d frames at y %d\n", gm->name, fr, q.y);
    CHECK(fr < 120 && q.y == p.y, "%s: warp-in lands on the floor", gm->name);
    CHECK(warp_snd, "%s: warp-in sound %02X", gm->name, gm->sfx_warp);
  }

  //Special weapons: owned and full after unlocking; a shot of weapon 1
  //uses energy.
  for (int g = 1; g <= 3; g++) {
    if (!rom[g]) continue;
    const TrioGame *gm = &trio_games[g];
    CHECK(trio_boot_stage(&s, rom[g], size[g], true, gm->home, NULL), "boot");
    snes_clone(&t, &s);
    trio_unlock_weapons(&t);
    trio_set_weapon(&t, 1);
    int e0 = trio_weapon_energy(&t, 1);
    run(&t, 2, 0, NULL);
    run(&t, 2, PAD_Y, NULL);
    run(&t, 40, 0, NULL);
    int e1 = trio_weapon_energy(&t, 1);
    printf("%s: weapon 1 energy %d -> %d\n", gm->name, e0, e1);
    CHECK(e0 == gm->weapon_full && e1 < e0, "%s: a weapon shot uses energy (%d -> %d)", gm->name, e0, e1);
  }

  //Every arena: its stage boots, the box has floors and four spawns on them,
  //and a fighter of each game installed there stands on the first one.
  for (int i = 0; i < arena_ndefs; i++) {
    const ArenaDef *d = &arena_defs[i];
    if (!rom[d->game]) continue;
    bool ok = stage_boot(&st, d, rom[d->game], size[d->game], true);
    CHECK(ok, "%s: boots", d->name);
    if (!ok) continue;
    for (int k = 0; k < 4; k++)
      CHECK(st.spawn_x[k] > d->x0 && st.spawn_x[k] < d->x1 && st.spawn_y[k] > d->y0 && st.spawn_y[k] <= d->y1 &&
            (trio_at_solid(trio_geo_at(&st.geo, st.spawn_x[k], st.spawn_y[k])) || trio_geo_at(&st.geo, st.spawn_x[k], st.spawn_y[k]) == TRIO_AT_LADDER_TOP),
            "%s: spawn %d on a floor or a one-way platform", d->name, k);
    for (int g = 1; g <= 3; g++) {
      if (!rom[g]) continue;
      CHECK(trio_boot_stage(&s, rom[g], size[g], true, trio_games[g].home, NULL), "boot");
      CHECK(stage_install(&st, &s), "%s: %s installs", d->name, trio_games[g].name);
      trio_place(&s, st.spawn_x[1], st.spawn_y[1] - 60, true);
      run(&s, 60, 0, &st);
      TrioPlayer q;
      trio_read_player(&s, &q);
      CHECK(q.ground && q.y + 17 == st.spawn_y[1] && q.hp > 0, "%s: %s lands on spawn 1 (x %d y %d ground %d, floor %d)", d->name, trio_games[g].name,
            q.x, q.y, q.ground, st.spawn_y[1]);
    }
    stage_free(&st);
  }

  //Blade (G3): his flags and palette; holding Y to the second level slashes.
  if (rom[3]) {
    CHECK(trio_boot_stage(&s, rom[3], size[3], true, 1, NULL), "boot for zero");
    snes_clone(&t, &s);
    trio_make_blade(&t);
    bool pal_differs = memcmp(s.ppu.cgram + 128, t.ppu.cgram + 128, 128 * sizeof(uint16_t)) != 0;
    CHECK(trio_is_blade(&t) && !trio_is_blade(&s), "zero flag");
    CHECK(pal_differs, "zero has his own palette");
    bool slash = false;
    TrioPlayer q;
    for (int i = 0; i < 260 && !slash; i++) {
      run(&t, 1, i < 220 ? PAD_Y : 0, NULL);
      trio_read_player(&t, &q);
      slash = q.action == TRIO_ACT_CHARGED || q.action == TRIO_ACT_SWORD;
    }
    CHECK(slash, "second charge level fires the charged charged blaster and slash");
  }
  //Upgrades: G3 with life upgrades and gold armor.
  if (rom[3]) {
    TrioUpgrades up = {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, TRIO_CHIPS_GOLD, false};
    CHECK(trio_boot_stage(&s, rom[3], size[3], true, 1, &up), "boot with upgrades");
    TrioPlayer q;
    trio_read_player(&s, &q);
    printf("upgraded: hp %d max %d\n", q.hp, trio_max_hp(&s));
    CHECK(q.hp == 32 && trio_max_hp(&s) == 32, "life upgrades give 32 HP");
  }
  if (!fails) printf("trio: all tests passed\n");
  return fails ? 1 : 0;
}
