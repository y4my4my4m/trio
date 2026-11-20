#ifndef PORT_SAVESLOTS_UI_H
#define PORT_SAVESLOTS_UI_H

#include <SDL.h>
#include <stdbool.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

//Save/Load tab body (ImGui). Returns true when settings changed.
bool ssui_tab(Settings *cfg, SDL_Renderer *ren);
//Pending SS_OP_* request from the tab; false if none.
bool ssui_take_request(int *op, int *slot);
void ssui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
