#ifndef ARENA_LAUNCHER_H
#define ARENA_LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"
#include "port/port_api.h"

//Game select screen (docs/LAUNCHER.md): the title screen of the game under
//the cursor, with its opening song, and a menu to play it, start a battle
//or open the options. Each game's title machine is booted once and held on
//its title screen; the song plays on a chip of the screen's own.

typedef struct LauncherGame {
  const uint8_t *rom;      //NULL = not available
  uint32_t size;
  bool recomp;
} LauncherGame;

enum { LA_NONE, LA_PLAY, LA_BATTLE, LA_OPTIONS, LA_QUIT };

//games[1..3] (index = SNES_GAME_*). Boots the selected game's title now and
//the others over the next frames. selected 0 picks the newest available.
void launcher_open(const LauncherGame games[4], int selected);
bool launcher_is_open(void);
void launcher_close(void);
//One frame with the buttons of every device together. Returns LA_*; for
//LA_PLAY the game is launcher_game() and launcher_machine() is its title.
int launcher_frame(uint16_t any);
int launcher_game(void);
//The selected game's title machine, held on its title screen. A copy
//handed to the player must call launcher_resume() on it first.
const Snes *launcher_machine(int game);
//Puts a copy of a held title machine back in step: its sound chip ran on
//while the CPU waited.
void launcher_resume(Snes *s);
const uint32_t *launcher_picture(int *w, int *h);
int launcher_audio(int16_t *out, int max_frames);
//The battle setup page asked for a match (setup in g_port.battle).
const char *launcher_game_name(int game);

#endif
