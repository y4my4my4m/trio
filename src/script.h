#ifndef SR_SCRIPT_H
#define SR_SCRIPT_H

#include <stdint.h>

//Input script: one command per line, '#' comments.
//  <frame> <buttons> [hold_frames]   buttons: A B X Y L R START SELECT UP DOWN LEFT RIGHT NONE, '+'-joined
//  <frame> shot <file.ppm>
//  <frame> save <file.state>
//  <frame> quit
//A pad command holds its buttons for hold_frames (default 1) frames.

typedef enum { CMD_PAD, CMD_SHOT, CMD_SAVE, CMD_QUIT } ScriptKind;

typedef struct {
  int frame;
  ScriptKind kind;
  uint16_t buttons;
  int hold;
  char arg[256];
} ScriptCmd;

//Returns a malloc'd array, NULL (with a message on stderr) if the file cannot be opened.
ScriptCmd *script_load(const char *path, int *count);

#endif
