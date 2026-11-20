#include "settings.h"
#include <SDL_scancode.h>
#include <SDL_gamecontroller.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *bind_names[BIND_COUNT] = {
  "Up", "Down", "Left", "Right", "B (jump)", "Y (shoot)", "A (dash)", "X (special weapon)",
  "L (weapon -)", "R (weapon +)", "Start", "Select",
};
static const char *bind_keys[BIND_COUNT] = {
  "up", "down", "left", "right", "b", "y", "a", "x", "l", "r", "start", "select",
};

void settings_pad_preset(Settings *s, int layout) {
  //Positional: the SNES diamond on the pad's diamond (south = B, east = A,
  //west = Y, north = X). By label: the button printed A is A, and so on.
  bool label = layout == PADLAYOUT_LABEL;
  int m[BIND_COUNT] = {
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    label ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A,   //B
    label ? SDL_CONTROLLER_BUTTON_Y : SDL_CONTROLLER_BUTTON_X,   //Y
    label ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B,   //A
    label ? SDL_CONTROLLER_BUTTON_X : SDL_CONTROLLER_BUTTON_Y,   //X
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
  };
  memcpy(s->pad_map, m, sizeof(m));
  s->pad_layout = layout;
}

const char *settings_pad_button_name(int v) {
  static const char *names[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = "South (A / Cross)", [SDL_CONTROLLER_BUTTON_B] = "East (B / Circle)",
    [SDL_CONTROLLER_BUTTON_X] = "West (X / Square)", [SDL_CONTROLLER_BUTTON_Y] = "North (Y / Triangle)",
    [SDL_CONTROLLER_BUTTON_BACK] = "Back / Select", [SDL_CONTROLLER_BUTTON_GUIDE] = "Guide",
    [SDL_CONTROLLER_BUTTON_START] = "Start", [SDL_CONTROLLER_BUTTON_LEFTSTICK] = "Left stick click",
    [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "Right stick click", [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "LB / L1",
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "RB / R1", [SDL_CONTROLLER_BUTTON_DPAD_UP] = "D-pad up",
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "D-pad down", [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "D-pad left",
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "D-pad right",
  };
  if (v == PAD_TRIGGER_L) return "LT / L2";
  if (v == PAD_TRIGGER_R) return "RT / R2";
  if (v < 0 || v >= SDL_CONTROLLER_BUTTON_MAX) return "-";
  return names[v] ? names[v] : SDL_GameControllerGetStringForButton((SDL_GameControllerButton)v);
}

const char *settings_bind_name(int bind) { return bind >= 0 && bind < BIND_COUNT ? bind_names[bind] : "?"; }

void settings_default(Settings *s) {
  memset(s, 0, sizeof(*s));
  s->scale = 3;
  s->pixel_aspect = false;
  s->vsync = true;
  s->widescreen = WIDE_OFF;
  s->wide_hud = true;
  s->volume = 100;
  s->turbo_rate = 3;
  s->pause_unfocused = true;
  s->ff_speed = 4;
  s->rewind = true;
  s->rewind_seconds = 20;
  s->autosave = true;
  static const int keys[BIND_COUNT] = {
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
    SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT,
  };
  memcpy(s->keys, keys, sizeof(keys));
  s->keys2[BIND_SELECT] = SDL_SCANCODE_BACKSPACE;
  settings_pad_preset(s, PADLAYOUT_POSITION);
  s->deadzone = 30;
  s->pad_hotkeys = true;
}

int settings_wide_columns(int widescreen, bool pixel_aspect) {
  //Columns per side so the displayed image hits the target aspect at 224
  //lines; with 8:7 pixels one column displays 8/7 wide.
  static const int square[WIDE_COUNT] = {0, 48, 72, 96};  //352, 400, 448 px
  static const int par87[WIDE_COUNT] = {0, 32, 48, 72};   //320, 352, 400 px
  if (widescreen <= WIDE_OFF || widescreen >= WIDE_COUNT) return 0;
  return pixel_aspect ? par87[widescreen] : square[widescreen];
}

typedef struct {
  const char *key;
  enum { T_INT, T_BOOL } type;
  size_t off;
} Field;

#define F(k, t, m) {k, t, offsetof(Settings, m)}
static const Field fields[] = {
  F("scale", T_INT, scale),
  F("fullscreen", T_BOOL, fullscreen),
  F("integer_scale", T_BOOL, integer_scale),
  F("filter", T_INT, filter),
  F("pixel_aspect", T_BOOL, pixel_aspect),
  F("vsync", T_BOOL, vsync),
  F("widescreen", T_INT, widescreen),
  F("wide_hud", T_BOOL, wide_hud),
  F("no_sprite_limit", T_BOOL, no_sprite_limit),
  F("scanlines", T_INT, scanlines),
  F("show_fps", T_BOOL, show_fps),
  F("volume", T_INT, volume),
  F("mute_unfocused", T_BOOL, mute_unfocused),
  F("pad_layout", T_INT, pad_layout),
  F("deadzone", T_INT, deadzone),
  F("pad_hotkeys", T_BOOL, pad_hotkeys),
  F("turbo_fire", T_BOOL, turbo_fire),
  F("turbo_rate", T_INT, turbo_rate),
  F("auto_charge", T_BOOL, auto_charge),
  F("pause_unfocused", T_BOOL, pause_unfocused),
  F("inf_health", T_BOOL, inf_health),
  F("inf_lives", T_BOOL, inf_lives),
  F("ff_speed", T_INT, ff_speed),
  F("rewind", T_BOOL, rewind),
  F("rewind_seconds", T_INT, rewind_seconds),
  F("autosave", T_BOOL, autosave),
  F("autosave_minutes", T_INT, autosave_minutes),
  F("save_slot", T_INT, save_slot),
  F("battle_slot1", T_INT, battle_slot[0]),
  F("battle_slot2", T_INT, battle_slot[1]),
  F("battle_slot3", T_INT, battle_slot[2]),
  F("battle_slot4", T_INT, battle_slot[3]),
  F("battle_fighter1", T_INT, battle_fighter[0]),
  F("battle_fighter2", T_INT, battle_fighter[1]),
  F("battle_fighter3", T_INT, battle_fighter[2]),
  F("battle_fighter4", T_INT, battle_fighter[3]),
  F("battle_lives", T_INT, battle_stocks),
  F("battle_stage", T_INT, battle_room),
  F("battle_music", T_INT, battle_music),
  F("battle_view", T_INT, battle_view),
  F("battle_time", T_INT, battle_time),
  F("battle_weapons", T_BOOL, battle_weapons),
  F("battle_weapon_games", T_INT, battle_weapon_games),
  F("battle_items", T_BOOL, battle_items),
};
#undef F

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void clamp(Settings *s) {
  s->scale = clampi(s->scale, 1, 8);
  if (s->widescreen < 0 || s->widescreen >= WIDE_COUNT) s->widescreen = WIDE_OFF;
  if (s->filter < 0 || s->filter >= FILTER_COUNT) s->filter = FILTER_NEAREST;
  if (s->pad_layout < 0 || s->pad_layout >= PADLAYOUT_COUNT) s->pad_layout = PADLAYOUT_POSITION;
  s->scanlines = clampi(s->scanlines, 0, 100);
  s->volume = clampi(s->volume, 0, 100);
  s->turbo_rate = clampi(s->turbo_rate, 2, 8);
  s->ff_speed = clampi(s->ff_speed, 2, 16);
  s->rewind_seconds = clampi(s->rewind_seconds, 5, 60);
  s->autosave_minutes = clampi(s->autosave_minutes, 0, 60);
  s->save_slot = clampi(s->save_slot, 0, 10);
  s->deadzone = clampi(s->deadzone, 5, 90);
  for (int i = 0; i < BIND_COUNT; i++) {
    if (s->pad_map[i] < -1 || (s->pad_map[i] >= SDL_CONTROLLER_BUTTON_MAX && s->pad_map[i] != PAD_TRIGGER_L &&
                               s->pad_map[i] != PAD_TRIGGER_R))
      s->pad_map[i] = -1;
    s->keys[i] = clampi(s->keys[i], 0, SDL_NUM_SCANCODES - 1);
    s->keys2[i] = clampi(s->keys2[i], 0, SDL_NUM_SCANCODES - 1);
  }
}

bool settings_load(Settings *s, const char *path) {
  settings_default(s);
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char key[64];
    int v = 0;
    if (line[0] == '#' || sscanf(line, " %63[^= ] = %d", key, &v) != 2) continue;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
      if (strcmp(key, fields[i].key)) continue;
      char *p = (char *)s + fields[i].off;
      if (fields[i].type == T_INT) *(int *)p = v;
      else *(bool *)p = v != 0;
    }
    for (int b = 0; b < BIND_COUNT; b++) {
      char k1[32], k2[32];
      snprintf(k1, sizeof(k1), "key_%s", bind_keys[b]);
      snprintf(k2, sizeof(k2), "key2_%s", bind_keys[b]);
      if (!strcmp(key, k1)) s->keys[b] = v;
      if (!strcmp(key, k2)) s->keys2[b] = v;
      char k3[32];
      snprintf(k3, sizeof(k3), "pad_%s", bind_keys[b]);
      if (!strcmp(key, k3)) s->pad_map[b] = v;
    }
  }
  fclose(f);
  clamp(s);
  return true;
}

bool settings_save(const Settings *s, const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) return false;
  fprintf(f, "# the three games port settings (keys are SDL scancodes)\n");
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    const char *p = (const char *)s + fields[i].off;
    int v = fields[i].type == T_INT ? *(const int *)p : *(const bool *)p;
    fprintf(f, "%s = %d\n", fields[i].key, v);
  }
  for (int b = 0; b < BIND_COUNT; b++) {
    fprintf(f, "key_%s = %d\n", bind_keys[b], s->keys[b]);
    fprintf(f, "key2_%s = %d\n", bind_keys[b], s->keys2[b]);
    fprintf(f, "pad_%s = %d\n", bind_keys[b], s->pad_map[b]);
  }
  fclose(f);
  return true;
}
