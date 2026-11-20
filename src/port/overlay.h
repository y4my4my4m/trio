#ifndef PORT_OVERLAY_H
#define PORT_OVERLAY_H

#include <SDL.h>
#include <stdbool.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

//Port menu (Dear ImGui), toggled with F1 / Esc.

enum {
  OV_RESET = 1 << 0,
  OV_SAVE_STATE = 1 << 1,
  OV_LOAD_STATE = 1 << 2,
  OV_QUIT = 1 << 3,
  OV_SETTINGS = 1 << 4,    //settings changed; caller applies and saves
  OV_WINDOW = 1 << 5,      //window size or mode changed
  OV_GAME_SELECT = 1 << 6, //back to the game select screen
};

//Live values shown in the menu.
typedef struct OverlayInfo {
  float fps;
  const char *status;      //toast, may be empty
  bool widescreen_ok;      //ROM + recompiled build support widescreen
  bool in_stage;
  int rewind_states;
  size_t rewind_bytes;
  bool rewinding;
  bool can_game_select;    //the game select screen is available
} OverlayInfo;

bool overlay_init(SDL_Window *win, SDL_Renderer *ren);
void overlay_shutdown(void);
//True if the event belongs to the menu (menu open, or a key is being bound).
bool overlay_event(const SDL_Event *ev, Settings *cfg);
void overlay_toggle(void);
bool overlay_is_open(void);
bool overlay_capturing_key(void);
//Builds and renders the UI on top of the current frame. Returns OV_* flags.
int overlay_frame(Settings *cfg, const OverlayInfo *info);

#ifdef __cplusplus
}
#endif

#endif
