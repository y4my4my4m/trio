#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "arena/abot.h"
#include "arena/boss.h"
#include "game/battle_hooks.h"
#include "game/recomp_rt.h"

//CPU matches on the real game code (src/arena/arena.c): fighters of
//different games in arenas of different games land hits, take stocks and
//finish. Needs the ROMs in ROMDIR (exit 77 without any).
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *load(const char *dir, int g, uint32_t *size) { return snes_rom_find(dir, g, size); }

static Snes home[4], blade, base[ARENA_MAX];
static ArenaStage st;
static ArenaSound snd;
static Arena A;

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "rom";
  int matches = argc > 2 ? atoi(argv[2]) : 3;
  uint8_t *rom[4] = {0};
  uint32_t size[4] = {0};
  int ngames = 0;
  for (int g = 1; g <= 3; g++) ngames += (rom[g] = load(dir, g, &size[g])) != NULL;
  if (!ngames) { printf("no ROMs, skipped\n"); return 77; }
  snes_skip_render = true;
  for (int g = 1; g <= 3; g++)
    if (rom[g]) CHECK(trio_boot_stage(&home[g], rom[g], size[g], true, trio_games[g].home, NULL), "%s boots", trio_games[g].name);
  if (rom[3]) { snes_clone(&blade, &home[3]); trio_make_blade(&blade); }
  int finished = 0;
  int m0 = getenv("ARENA_MATCH") ? atoi(getenv("ARENA_MATCH")) : 0;   //debug: start at this match
  for (int m = m0; m < m0 + matches; m++) {
    //An arena of each game in turn, fighters of every game.
    int ai = (m * 11) % arena_ndefs;
    if (getenv("ARENA_ROOM")) ai = atoi(getenv("ARENA_ROOM")) % arena_ndefs;   //debug: this arena
    while (!rom[arena_defs[ai].game]) ai = (ai + 1) % arena_ndefs;
    const ArenaDef *d = &arena_defs[ai];
    CHECK(stage_boot(&st, d, rom[d->game], size[d->game], true), "%s boots", d->name);
    ArenaSlot slots[ARENA_MAX] = {{0}};
    int nf = getenv("ARENA_FIGHTERS") ? atoi(getenv("ARENA_FIGHTERS")) : 2 + m % 3;
    printf("match %d in %s:", m, d->name);
    snd_init(&snd);
    for (int i = 0; i < nf; i++) {
      int g = 1 + (m + i) % 3;
      while (!rom[g]) g = g % 3 + 1;
      Snes *src = (g == 3 && i == 2 && rom[3]) ? &blade : &home[g];
      snes_clone(&base[i], src);
      CHECK(stage_install(&st, &base[i]), "install");
      trio_unlock_weapons(&base[i]);
      if (!snd.silent[g].on) snd_add(&snd, src, false);
      slots[i] = (ArenaSlot){true, true, 3 - (i % 2), &base[i], false, {0}, 0};
      printf(" %s%s", trio_games[g].name, src == &blade ? " BLADE" : "");
    }
    printf("\n");
    snd_add(&snd, st.view, true);
    //Shots live anywhere in the box, as in battle.c.
    battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
    arena_init(&A, &st, &snd, slots, (ArenaRules){2, m % 2 == 0, true, 0});
    ABot bots[ARENA_MAX];
    for (int i = 0; i < ARENA_MAX; i++) abot_init(&bots[i], i, 1234u + (uint32_t)m);
    int hits = 0, fr = 0;
    clock_t t0 = clock();
    while (A.phase != AP_OVER && fr < 60 * 60 * 4) {
      uint16_t pads[ARENA_MAX] = {0};
      for (int i = 0; i < ARENA_MAX; i++) if (A.f[i].on) pads[i] = abot_think(&bots[i], &A, i);
      arena_step(&A, pads);
      snd_frame(&snd);
      int16_t buf[4096];
      while (snd_mix(&snd, buf, 2048) > 0) {}
      for (int k = 0; k < A.nfx; k++) if (A.fx[k].kind == FX_HIT || A.fx[k].kind == FX_BIG_HIT) hits++;
      fr++;
      if (getenv("ARENA_LOG") && fr % 300 == 0) {
        printf("  t%d", fr);
        for (int i = 0; i < ARENA_MAX; i++) if (A.f[i].on) printf("  P%d x%d y%d act%02X hp%d st%d", i + 1, A.f[i].p.x, A.f[i].p.y, A.f[i].p.action, A.f[i].p.hp, A.f[i].stocks);
        printf("\n");
      }
      for (int i = 0; i < ARENA_MAX; i++)
        if (A.f[i].on && !A.f[i].dead && A.f[i].stocks > 0)
          CHECK(A.f[i].p.x >= d->x0 - 16 && A.f[i].p.x <= d->x1 + 16, "fighter %d left the box (x %d)", i, A.f[i].p.x);
      if (fails > 5) break;
    }
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("  %d frames (%.0f s game, %.1f s wall), %d hits, winner %d\n", fr, fr / 60.0, sec, hits, A.winner);
    for (int i = 0; i < ARENA_MAX; i++)
      if (A.f[i].on) printf("  P%d stocks %d hp %d KO %d falls %d dealt %d\n", i + 1, A.f[i].stocks, A.f[i].p.hp, A.f[i].kos, A.f[i].falls, A.f[i].dealt);
    CHECK(hits > 0, "hits land");
    if (A.phase == AP_OVER) { CHECK(A.winner >= 0, "finished match has a winner"); finished++; }
    arena_free(&A);
    stage_free(&st);
  }
  CHECK(finished * 3 >= matches * 2, "only %d/%d matches finished", finished, matches);

  //Weapon switching while a blaster shot is still flying (the games wait
  //for it to be gone; the arena doesn't), for the hero of each game.
  for (int g = 1; g <= 3 && arena_ndefs; g++) {
    if (!rom[g]) continue;
    const ArenaDef *d = &arena_defs[0];
    if (!rom[d->game] || !stage_boot(&st, d, rom[d->game], size[d->game], true)) continue;
    ArenaSlot slots[ARENA_MAX] = {{0}};
    for (int i = 0; i < 2; i++) {
      snes_clone(&base[i], &home[g]);
      CHECK(stage_install(&st, &base[i]), "install");
      trio_unlock_weapons(&base[i]);
      slots[i] = (ArenaSlot){true, i == 1, 1, &base[i], false, {0}, 0};
    }
    battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
    arena_init(&A, &st, NULL, slots, (ArenaRules){2, true, false, 0});
    uint16_t pads[ARENA_MAX] = {0};
    int fr = 0;
    while (fr < 600 && !(A.phase == AP_FIGHT && A.f[0].p.ground && A.f[0].p.action == TRIO_ACT_IDLE)) { arena_step(&A, pads); fr++; }
    pads[0] = PAD_Y;
    arena_step(&A, pads);
    pads[0] = 0;
    for (int k = 0; k < 3; k++) arena_step(&A, pads);
    int shots = A.f[0].nshots, w0 = trio_weapon(A.f[0].s);
    pads[0] = PAD_R;
    for (int k = 0; k < 4; k++) arena_step(&A, pads);
    int w1 = trio_weapon(A.f[0].s);
    pads[0] = 0;
    for (int k = 0; k < 4; k++) arena_step(&A, pads);
    printf("%s: switch with %d shot(s) out: weapon %d -> %d, shots now %d\n", trio_games[g].name, shots, w0, w1, A.f[0].nshots);
    CHECK(shots > 0 && w0 == 0 && w1 != 0, "%s: weapon switches with a shot out", trio_games[g].name);
    //Firing on: the blaster keeps up a steady rate (far shots don't hold the
    //game's limit or its shot table), and the new weapon fires at once.
    for (int k = 0; k < 30; k++) arena_step(&A, pads);
    pads[0] = PAD_L;
    arena_step(&A, pads);
    pads[0] = 0;
    arena_step(&A, pads);
    int fired = 0, prev = A.f[0].p.shots;
    for (int k = 0; k < 120; k++) {
      pads[0] = (k % 8) < 2 ? PAD_Y : 0;
      arena_step(&A, pads);
      if (A.f[0].p.shots > prev) fired += A.f[0].p.shots - prev;
      prev = A.f[0].p.shots;
    }
    pads[0] = PAD_R;
    arena_step(&A, pads);
    pads[0] = 0;
    arena_step(&A, pads);
    int we = trio_weapon(A.f[0].s), e0 = trio_weapon_energy(A.f[0].s, we);
    pads[0] = PAD_Y;
    arena_step(&A, pads);
    pads[0] = 0;
    for (int k = 0; k < 10; k++) arena_step(&A, pads);
    printf("%s: %d blaster shots in 2 s; weapon %d fired after the switch: %d\n", trio_games[g].name, fired, we, trio_weapon_energy(A.f[0].s, we) < e0);
    CHECK(fired >= 8, "%s: blaster fire rate (%d in 2 s)", trio_games[g].name, fired);
    CHECK(we != 0 && trio_weapon_energy(A.f[0].s, we) < e0, "%s: a weapon fires right after switching", trio_games[g].name);
    arena_free(&A);
    stage_free(&st);
  }
  //HERO FULL's specials. G3's the hero does G1's Fireball and G2's Uppercut through
//the helper copies (the fireball hits the opponent in front; the
//Uppercut lifts him); G1's own Fireball works below full HP.
  if (rom[1] && rom[2] && rom[3] && arena_ndefs) {
    const ArenaDef *d = &arena_defs[0];
    static Snes full1, full2;
    TrioUpgrades up1 = {TRIO_PARTS_ALL, TRIO_HEARTS_ALL, 0, true};
    CHECK(trio_boot_stage(&full1, rom[1], size[1], true, trio_games[1].home, &up1), "G1 FULL boots");
    CHECK(trio_boot_stage(&full2, rom[2], size[2], true, trio_games[2].home, &up1), "G2 FULL boots");
    //The helpers are HERO FULL of their games (they own the specials).
    static Snes hb[3];
    if (stage_boot(&st, d, rom[d->game], size[d->game], true)) {
      for (int g = 1; g <= 2; g++) { snes_clone(&hb[g], g == 1 ? &full1 : &full2); CHECK(stage_install(&st, &hb[g]), "helper install"); }
      for (int sc = 0; sc < 3; sc++) {
        ArenaSlot slots[ARENA_MAX] = {{0}};
        snes_clone(&base[0], sc == 2 ? &full1 : &home[3]);
        snes_clone(&base[1], &home[3]);
        for (int i = 0; i < 2; i++) CHECK(stage_install(&st, &base[i]), "install");
        slots[0] = (ArenaSlot){true, false, 1, &base[0], true, {0}, 0};
        slots[1] = (ArenaSlot){true, false, 1, &base[1], false, {0}, 0};
        battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
        arena_init(&A, &st, NULL, slots, (ArenaRules){9, false, false, 0});
        for (int g = 1; g <= 2; g++) arena_set_helper(&A, g, &hb[g]);
        uint16_t pads[ARENA_MAX] = {0};
        int fr = 0;
        while (fr < 600 && !(A.phase == AP_FIGHT && A.f[0].p.ground && A.f[1].p.ground && A.f[0].p.action == TRIO_ACT_IDLE)) { arena_step(&A, pads); fr++; }
        //The opponent 64 px in front, the fighter facing him; half HP.
        bool right = A.f[0].p.facing_right;
        int gap = sc == 1 ? 28 : 64;
        trio_place(A.f[1].s, A.f[0].p.x + (right ? gap : -gap), A.f[0].p.y, !right);
        if (sc == 2) trio_set_hp(A.f[0].s, 16);
        for (int k = 0; k < 10; k++) arena_step(&A, pads);
        uint16_t fwd = A.f[0].p.facing_right ? PAD_RIGHT : PAD_LEFT;
        uint16_t seq[4][4] = {{0}, {PAD_DOWN, PAD_DOWN | fwd, fwd, fwd | PAD_Y}, {fwd, PAD_DOWN, PAD_DOWN | fwd, PAD_DOWN | fwd | PAD_Y}};
        int move = sc == 1 ? 2 : 1;
        for (int st_ = 0; st_ < 4; st_++) {
          for (int k = 0; k < 3; k++) { pads[0] = seq[move][st_]; if (st_ == 3 && k > 0) pads[0] &= (uint16_t)~PAD_Y; arena_step(&A, pads); }
        }
        pads[0] = 0;
        int y0 = A.f[0].p.y, top = y0, hp1 = A.f[1].p.hp, act = 0;
        for (int k = 0; k < 120; k++) {
          arena_step(&A, pads);
          if (A.f[0].p.y < top) top = A.f[0].p.y;
          if (A.f[0].p.action == 0x42) act = 1;
          if (getenv("SP_LOG") && k % 6 == 0) {
            printf("  k%d f0 %d,%d sp %d |", k, A.f[0].p.x, A.f[0].p.y, A.f[0].sp_move);
            for (int g = 1; g <= 2; g++) for (int j = 0; j < A.f[0].nhshots[g]; j++) printf(" g%d kind %02X %d,%d mv %d", g, A.f[0].hshots[g][j].kind, A.f[0].hshots[g][j].x, A.f[0].hshots[g][j].y, A.f[0].hshots[g][j].moving);
            printf(" | opp %d,%d\n", A.f[1].p.x, A.f[1].p.y);
          }
        }
        const char *names[3] = {"G3 Fireball (helper)", "G3 Uppercut (helper)", "G1 Fireball at half HP"};
        printf("%s: rise %d, opponent hp %d -> %d, own action $42 %d\n", names[sc], y0 - top, hp1, A.f[1].p.hp, act);
        if (sc == 1) CHECK(y0 - top >= 60 && A.f[1].p.hp < hp1, "%s lifts the fighter and hits", names[sc]);
        else CHECK(A.f[1].p.hp < hp1, "%s hits", names[sc]);
        if (sc == 2) CHECK(act, "G1 Fireball starts below full HP");
        arena_free(&A);
      }
      stage_free(&st);
    }
  }

  //A time limit ends the match with TIME, the most lives (then life) winning.
  if (arena_ndefs) {
    const ArenaDef *d = &arena_defs[0];
    int g = d->game;
    if (rom[g] && stage_boot(&st, d, rom[g], size[g], true)) {
      ArenaSlot slots[ARENA_MAX] = {{0}};
      for (int i = 0; i < 2; i++) {
        snes_clone(&base[i], &home[g]);
        CHECK(stage_install(&st, &base[i]), "install");
        slots[i] = (ArenaSlot){true, true, 1, &base[i], false, {0}, 0};
      }
      battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
      arena_init(&A, &st, NULL, slots, (ArenaRules){9, true, false, 600});
      uint16_t pads[ARENA_MAX] = {0};
      int fr = 0;
      while (A.phase != AP_OVER && fr < 2000) { arena_step(&A, pads); fr++; }
      printf("time limit: over after %d frames, time_up %d, winner %d (hp %d/%d)\n", fr, A.time_up, A.winner, A.f[0].p.hp, A.f[1].p.hp);
      CHECK(A.phase == AP_OVER && A.time_up, "a time limit ends the match");
      CHECK(A.winner >= 0 || (A.f[0].p.hp == A.f[1].p.hp && A.f[0].stocks == A.f[1].stocks), "TIME picks a winner unless tied");
      arena_free(&A);
      stage_free(&st);
    }
  }

  //WEAPON SETS: G3's hero with G1's weapons. R from the blaster goes into his
  //G1 copy (the same place and life) with G1's first weapon, which fires.
  if (rom[1] && rom[3] && arena_ndefs) {
    const ArenaDef *d = &arena_defs[0];
    if (rom[d->game] && stage_boot(&st, d, rom[d->game], size[d->game], true)) {
      static Snes x1;
      snes_clone(&x1, &home[1]);
      CHECK(stage_install(&st, &x1), "install");
      trio_unlock_weapons(&x1);
      ArenaSlot slots[ARENA_MAX] = {{0}};
      for (int i = 0; i < 2; i++) {
        snes_clone(&base[i], &home[3]);
        CHECK(stage_install(&st, &base[i]), "install");
        trio_unlock_weapons(&base[i]);
        slots[i] = (ArenaSlot){true, i == 1, 1, &base[i], false, {0}, 0};
      }
      slots[0].alt[1] = &x1;
      slots[0].pool = 1;
      battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
      arena_init(&A, &st, NULL, slots, (ArenaRules){2, true, false, 0});
      uint16_t pads[ARENA_MAX] = {0};
      int fr = 0;
      while (fr < 600 && !(A.phase == AP_FIGHT && A.f[0].p.ground && A.f[0].p.action == TRIO_ACT_IDLE)) { arena_step(&A, pads); fr++; }
      int x0 = A.f[0].p.x, n = A.f[0].nwl;
      pads[0] = PAD_R;
      arena_step(&A, pads);
      pads[0] = 0;
      for (int k = 0; k < 3; k++) arena_step(&A, pads);
      int g = trio_game(A.f[0].s)->id, w = trio_weapon(A.f[0].s), e0 = trio_weapon_energy(A.f[0].s, w);
      pads[0] = PAD_Y;
      arena_step(&A, pads);
      pads[0] = 0;
      for (int k = 0; k < 10; k++) arena_step(&A, pads);
      printf("weapon sets: %d weapons; R: game X%d weapon %d (x %d -> %d), fired %d\n", n, g, w, x0, A.f[0].p.x, trio_weapon_energy(A.f[0].s, w) < e0);
      CHECK(n == 9 && g == 1 && w == 1 && abs(A.f[0].p.x - x0) < 4 && trio_weapon_energy(A.f[0].s, w) < e0, "G3's X switches to G1's weapon and fires it");
      arena_free(&A);
      stage_free(&st);
    }
  }

  //Blade's sword button: the slash on the ground, the jump slash in the air.
  if (rom[3] && arena_ndefs) {
    const ArenaDef *d = &arena_defs[0];
    if (rom[d->game] && stage_boot(&st, d, rom[d->game], size[d->game], true)) {
      ArenaSlot slots[ARENA_MAX] = {{0}};
      Snes *src[2] = {&blade, &home[3]};
      for (int i = 0; i < 2; i++) {
        snes_clone(&base[i], src[i]);
        CHECK(stage_install(&st, &base[i]), "install");
        slots[i] = (ArenaSlot){true, i == 1, 1, &base[i], false, {0}, 0};
      }
      battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
      arena_init(&A, &st, NULL, slots, (ArenaRules){2, true, false, 0});
      uint16_t pads[ARENA_MAX] = {0};
      int fr = 0;
      while (fr < 600 && !(A.phase == AP_FIGHT && A.f[0].p.ground && A.f[0].p.action == TRIO_ACT_IDLE)) { arena_step(&A, pads); fr++; }
      bool ground = false, air = false;
      pads[0] = PAD_X;
      for (int k = 0; k < 8; k++) { arena_step(&A, pads); pads[0] = 0; ground |= A.f[0].p.action == TRIO_ACT_SWORD; }
      for (int k = 0; k < 90; k++) arena_step(&A, pads);
      for (int k = 0; k < 20; k++) {
        pads[0] = PAD_B | (k == 8 ? PAD_X : 0);
        arena_step(&A, pads);
        air |= A.f[0].p.action == TRIO_ACT_SWORD_AIR;
      }
      printf("blade: slash %d, jump slash %d\n", ground, air);
      CHECK(ground && air, "blade's sword button slashes on the ground and in the air");
      arena_free(&A);
      stage_free(&st);
    }
  }
  //Bosses between their attacks: B jumps (rises and lands again), and Y
  //still starts their own attack.
  for (int kind = 1; kind < TRIO_BOSS_COUNT && arena_ndefs; kind++) {
    int g = trio_boss_game(kind);
    const ArenaDef *d = &arena_defs[0];
    if (!rom[g] || !rom[d->game]) continue;
    static TrioBossSrc src;
    if (!trio_boss_source(&src, kind, rom[g], size[g], true)) { CHECK(0, "%s: source boots", trio_boss_name(kind)); continue; }
    if (!stage_boot(&st, d, rom[d->game], size[d->game], true)) continue;
    ArenaSlot slots[ARENA_MAX] = {{0}};
    snes_clone(&base[0], &home[g]);
    CHECK(stage_install(&st, &base[0]), "install");
    trio_boss_install(&base[0], &src, st.spawn_x[0], st.spawn_y[0]);
    snes_clone(&base[1], &home[g == 1 ? 1 : g]);
    CHECK(stage_install(&st, &base[1]), "install");
    slots[0] = (ArenaSlot){true, false, 1, &base[0], false, {0}, 0};
    slots[1] = (ArenaSlot){true, true, 1, &base[1], false, {0}, 0};
    battle_box = (BattleBox){true, d->x0 - 256, d->y0 - 256, d->x1 + 256, d->y1 + 256};
    arena_init(&A, &st, NULL, slots, (ArenaRules){2, true, false, 0});
    uint16_t pads[ARENA_MAX] = {0};
    int fr = 0;
    while (fr < 900 && !(A.phase == AP_FIGHT && A.f[0].p.ground)) { arena_step(&A, pads); fr++; }
    int y0 = A.f[0].p.y, top = y0;
    bool landed = false, left_ground = false;
    for (int k = 0; k < 120; k++) {
      pads[0] = k < 30 ? PAD_B : 0;
      arena_step(&A, pads);
      if (!A.f[0].p.ground) left_ground = true;
      if (A.f[0].p.y < top) top = A.f[0].p.y;
      if (left_ground && A.f[0].p.ground) landed = true;
    }
    pads[0] = 0;
    for (int k = 0; k < 20; k++) arena_step(&A, pads);
    bool attacked = false;
    for (int k = 0; k < 60 && !attacked; k++) {
      pads[0] = k < 20 ? PAD_Y : 0;
      arena_step(&A, pads);
      attacked = !A.f[0].ctl.holding;
    }
    printf("%s: jump %d px, landed %d, attack %d\n", trio_boss_name(kind), y0 - top, landed, attacked);
    CHECK(y0 - top >= 40 && landed, "%s jumps and lands", trio_boss_name(kind));
    CHECK(attacked, "%s attacks with Y", trio_boss_name(kind));
    arena_free(&A);
    stage_free(&st);
  }
  if (!fails) printf("arena: all tests passed\n");
  return fails ? 1 : 0;
}
