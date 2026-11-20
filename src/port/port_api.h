#ifndef PORT_PORT_API_H
#define PORT_PORT_API_H

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

//Battle mode setup, edited on the game select screen's BATTLE MODE page
//(src/arena/launcher.c) and run by the frontend (src/arena/battle.c).
enum {
  BS_OFF, BS_PLAYER, BS_KEYS1, BS_KEYS2, BS_PAD1, BS_PAD2, BS_PAD3, BS_PAD4,
  BS_CPU1, BS_CPU2, BS_CPU3, BS_COUNT
};
#define BATTLE_SLOTS 4

//Battle picture size: NORMAL is the port's widescreen width by 224 lines;
//LARGE 512x288 and HUGE 640x360 show more of the arena at the same pixel size.
enum { BV_NORMAL, BV_LARGE, BV_HUGE, BV_COUNT };

//Battle time limit: none, 2, 3 or 5 minutes (TIME: most lives, then life).
enum { BT_OFF, BT_2, BT_3, BT_5, BT_COUNT };

typedef struct BattleSetup {
  int slot[BATTLE_SLOTS];  //BS_*
  int stocks;              //1..9
  int room;                //index into the arena rooms
  bool weapons;            //all special weapons, or the blaster only
  int fighter[BATTLE_SLOTS];  //index into the character list (battle_character_name)
  int music;               //index into the song list (0 = the stage's own)
  bool items;              //items drop and appear during the match
  int view;                //BV_*
  int time;                //BT_*: time limit
  int weapon_games;        //bit g - 1: the hero of every style has game g's weapons (0: his own game's)
} BattleSetup;

//Frontend services reachable from game code (src/game). Filled by the
//frontend; NULL members are absent (trio-verify, headless runs).
typedef struct PortApi {
  Settings *settings;
  void (*settings_changed)(void);  //settings edited in game: apply and save
  void (*open_menu)(void);         //port menu (overlay)
  //Battle mode setup, edited on the game select screen.
  BattleSetup *battle;
} PortApi;

extern PortApi g_port;

#ifdef __cplusplus
}
#endif

#endif
