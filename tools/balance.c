#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arena/battle.h"
#include "game/recomp_rt.h"

//Balance check for battle mode: CPU LV3 against CPU LV3, one on one, for
//every pair of characters, on arenas taken in turn. Prints each match and a
//table of wins per character.
//usage: trio-balance ROMDIR [MATCHES_PER_PAIR [CHARS]]
//CHARS: comma-separated roster indexes (default all).

static uint8_t *load(const char *dir, int g, uint32_t *size) { return snes_rom_find(dir, g, size); }

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "rom";
  int per = argc > 2 ? atoi(argv[2]) : 2;
  const uint8_t *roms[4] = {0};
  uint32_t sizes[4] = {0};
  for (int g = 1; g <= 3; g++) roms[g] = load(dir, g, &sizes[g]);
  int nc = battle_characters(), chars[32], n = 0;
  if (argc > 3) {
    for (char *t = strtok(argv[3], ","); t && n < 32; t = strtok(NULL, ",")) chars[n++] = atoi(t);
  } else {
    for (int c = 0; c < nc && n < 32; c++) chars[n++] = c;
  }
  int wins[32] = {0}, played[32] = {0}, room = 0;
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      for (int m = 0; m < per; m++) {
        BattleSetup s = {{BS_CPU3, BS_CPU3, BS_OFF, BS_OFF}, 2, room, true, {0}, -1, true, BV_NORMAL, BT_OFF, 0};
        s.fighter[0] = m % 2 ? chars[b] : chars[a];
        s.fighter[1] = m % 2 ? chars[a] : chars[b];
        room = (room + 7) % battle_rooms();
        if (!battle_begin(&s, roms, sizes, true, 0)) { printf("no start in %s\n", battle_room_name(s.room)); continue; }
        uint16_t none[BATTLE_SLOTS] = {0};
        int16_t audio[4096];
        const Arena *A = battle_arena();
        int fr = 0;
        while (A->phase != AP_OVER && fr < 60 * 60 * 4) {
          battle_frame(none, 0);
          while (battle_audio(audio, 2048) > 0) {}
          fr++;
        }
        int w = A->phase == AP_OVER ? A->winner : -1;
        printf("%-14s vs %-14s %-22s %5ds  winner %s  (hp %d/%d stocks %d/%d dealt %d/%d)\n",
               battle_character_name(s.fighter[0]), battle_character_name(s.fighter[1]), battle_room_name(s.room), fr / 60,
               w < 0 ? "-" : battle_character_name(s.fighter[w]), A->f[0].p.hp, A->f[1].p.hp, A->f[0].stocks, A->f[1].stocks,
               A->f[0].dealt, A->f[1].dealt);
        fflush(stdout);
        for (int k = 0; k < n; k++) {
          if (chars[k] == s.fighter[0] || chars[k] == s.fighter[1]) played[k]++;
          if (w >= 0 && chars[k] == s.fighter[w]) wins[k]++;
        }
        battle_end();
      }
  printf("\n");
  for (int k = 0; k < n; k++) printf("%-14s %3d / %3d\n", battle_character_name(chars[k]), wins[k], played[k]);
  return 0;
}
