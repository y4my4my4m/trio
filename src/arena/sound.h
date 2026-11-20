#ifndef ARENA_SOUND_H
#define ARENA_SOUND_H

#include <stdbool.h>
#include <stdint.h>
#include "snes/snes.h"
#include "trio.h"

//Battle mode's sound (docs/BATTLE.md). The fighters' copies run without a
//sound chip; the sound effects they queue are sent to chips run here, the
//way the game's NMI would (one a frame, with the driver's handshake).
//
//Every fighter has chips of its own per game it plays in: the drivers
//have a few effect channels and priorities made for one player, so on a
//shared chip one fighter's sounds cut another's (a charge stopping, sound
//$06 in G3, silences a hurt sound from anyone). One chip per game is shared:
//it plays the music (the arena's game or the chosen song's) and the menu
//sounds, so a single song is heard whatever the fighters do.
//
//A fighter has two: one for the hurt and death sounds alone, one for the
//rest (shots, dashes, jumps, pickups). His own long sounds hold the
//channels the hurt sound needs: in G3 a charged shot ($04, $05) 20-40
//frames earlier, a sword swing ($07) or a pickup ($15) leaves it one
//channel, cuts it short or keeps it from starting.

#define SND_QUEUE 32
#define SND_OWNERS 4

typedef struct SndChip {
  bool on;
  Apu apu;
  uint64_t master;         //master cycles this chip has been run to
  uint8_t ack;             //handshake counter ($7E:FFFE in the game)
  TrioSnd q[SND_QUEUE];
  int nq;
  int quiet;               //frames of silence with nothing queued
} SndChip;

typedef struct ArenaSound {
  SndChip chip[4];         //shared, per game (SNES_GAME_*)
  SndChip silent[4];       //per game: a chip with its music stopped, the fighters' chips start as copies
  SndChip own[2][SND_OWNERS][4];  //[SND_ACT / SND_BODY][fighter][game], made on the first sound
  int music_game;          //the chip playing the music (0 = none)
  float volume[4];
} ArenaSound;

void snd_init(ArenaSound *a);
//Takes the sound chip of a machine of its game. music: as the game's
//shared chip, its song playing on. Else with the music stopped: what the
//fighters' chips of that game start from, and the shared chip unless that
//one plays the music.
void snd_add(ArenaSound *a, const Snes *src, bool music);
//Starts song `track` of game `g` from a machine of that game: a copy runs
//the game's music routine (it uploads the song to the chip), then that
//chip plays the music. False if the routine did not finish.
bool snd_add_song(ArenaSound *a, const Snes *src, int track);
//A sound on the game's shared chip.
void snd_play(ArenaSound *a, int game, uint8_t id, uint8_t param);
//A sound of fighter `owner` (0..SND_OWNERS-1) on his own chip of the game
//(which: SND_ACT or SND_BODY); the shared chip when the game has none
//without music.
enum { SND_ACT, SND_BODY };
void snd_play_from(ArenaSound *a, int owner, int which, int game, uint8_t id, uint8_t param);
//One frame: the queued sounds are sent and every chip runs a frame.
void snd_frame(ArenaSound *a);
//Mixed stereo samples; returns frames written.
int snd_mix(ArenaSound *a, int16_t *out, int max_frames);

#endif
