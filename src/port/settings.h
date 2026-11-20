#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//Port settings, stored as key = value lines in trio.ini next to the binary's
//working directory. Everything here is optional; defaults are the original
//game's behaviour except the display defaults.

enum { WIDE_OFF, WIDE_16_10, WIDE_16_9, WIDE_2_1, WIDE_COUNT };
enum { FILTER_NEAREST, FILTER_LINEAR, FILTER_COUNT };
enum { PADLAYOUT_POSITION, PADLAYOUT_LABEL, PADLAYOUT_COUNT };
//Pad binding values beyond SDL_GameControllerButton: the analog triggers.
enum { PAD_TRIGGER_L = 100, PAD_TRIGGER_R = 101 };

//Rebindable SNES buttons, in the order the Controls tab shows them.
enum {
  BIND_UP, BIND_DOWN, BIND_LEFT, BIND_RIGHT,
  BIND_B, BIND_Y, BIND_A, BIND_X, BIND_L, BIND_R, BIND_START, BIND_SELECT,
  BIND_COUNT
};

typedef struct Settings {
  //Video
  int scale;               //initial window scale
  bool fullscreen;
  bool integer_scale;
  int filter;              //FILTER_*
  bool pixel_aspect;       //8:7 SNES pixel aspect ratio
  bool vsync;
  int widescreen;          //WIDE_*: in stages only; menus and cutscenes stay 4:3
  bool wide_hud;           //widescreen: life, weapon and boss meters at the screen edges
  bool no_sprite_limit;    //no 32 sprite / 34 tile per-line limit (less flicker)
  int scanlines;           //0..100 darkening of every other output line
  bool show_fps;

  //Audio
  int volume;              //0..100
  bool mute_unfocused;

  //Input
  int keys[BIND_COUNT];    //SDL scancodes, 0 = unbound
  int keys2[BIND_COUNT];   //second binding per button
  int pad_layout;          //PADLAYOUT_*: preset last applied to pad_map
  int pad_map[BIND_COUNT]; //SDL_GameControllerButton, PAD_TRIGGER_L/R, -1 = unbound
  int deadzone;            //stick deadzone, percent of full travel
  bool pad_hotkeys;        //Back + button: rewind, fast forward, save, load, menu
  bool turbo_fire;         //holding shoot keeps firing (blaster pellets)
  int turbo_rate;          //frames per press/release half-cycle, 2..8
  bool auto_charge;        //blaster charges on its own; press shoot to release
  bool pause_unfocused;
  bool inf_health;         //assist: the hero's health refills (not instant-death hazards)
  bool inf_lives;          //assist: lost lives are given back

  //Emulation
  int ff_speed;            //frames per presented frame while fast-forwarding
  bool rewind;             //hold the rewind key to step back in time
  int rewind_seconds;      //history length, 5..60

  //Save slots (src/port/saveslots.h)
  bool autosave;           //autosave when a stage starts
  int autosave_minutes;    //timed autosave interval, 0 = off
  int save_slot;           //slot for F5/F9: 0 quick, 1..10

  //Battle mode's setup page as last used (BattleSetup); battle_stocks 0:
  //never saved, the frontend's defaults apply.
  int battle_slot[4], battle_fighter[4];
  int battle_stocks, battle_room, battle_music, battle_view, battle_time;
  bool battle_weapons, battle_items;
  int battle_weapon_games;
} Settings;

void settings_default(Settings *s);
bool settings_load(Settings *s, const char *path);
bool settings_save(const Settings *s, const char *path);

//Extra columns per side for the selected aspect at 224 lines. Multiples of
//8, as the stage streamers need (src/game/widescreen.c).
int settings_wide_columns(int widescreen, bool pixel_aspect);
const char *settings_bind_name(int bind);
//Fills pad_map from a PADLAYOUT_* preset.
void settings_pad_preset(Settings *s, int layout);
const char *settings_pad_button_name(int v);

#ifdef __cplusplus
}
#endif

#endif
