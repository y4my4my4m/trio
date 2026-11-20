#include "script.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snes/snes.h"

#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

static uint16_t parse_buttons(const char *s) {
  static const struct { const char *n; uint16_t b; } names[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"L", PAD_L}, {"R", PAD_R},
    {"START", PAD_START}, {"SELECT", PAD_SELECT}, {"UP", PAD_UP}, {"DOWN", PAD_DOWN},
    {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT}, {"NONE", 0},
  };
  uint16_t r = 0;
  char buf[128];
  snprintf(buf, sizeof(buf), "%s", s);
  for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
    bool found = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      if (!strcasecmp(t, names[i].n)) { r |= names[i].b; found = true; }
    if (!found) fprintf(stderr, "unknown button %s\n", t);
  }
  return r;
}

ScriptCmd *script_load(const char *path, int *count) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "cannot open script %s\n", path); return NULL; }
  ScriptCmd *cmds = NULL;
  int n = 0, cap = 0;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    int frame, hold = 1;
    char a[128] = {0}, b[256] = {0};
    int k = sscanf(line, "%d %127s %255s", &frame, a, b);
    if (k < 2) continue;
    if (n == cap) { cap = cap ? cap * 2 : 64; cmds = realloc(cmds, sizeof(*cmds) * (size_t)cap); }
    ScriptCmd *c = &cmds[n++];
    memset(c, 0, sizeof(*c));
    c->frame = frame;
    if (!strcasecmp(a, "shot")) { c->kind = CMD_SHOT; snprintf(c->arg, sizeof(c->arg), "%s", b); }
    else if (!strcasecmp(a, "save")) { c->kind = CMD_SAVE; snprintf(c->arg, sizeof(c->arg), "%s", b); }
    else if (!strcasecmp(a, "quit")) c->kind = CMD_QUIT;
    else {
      c->kind = CMD_PAD;
      c->buttons = parse_buttons(a);
      if (k == 3) hold = atoi(b);
      c->hold = hold;
    }
  }
  fclose(f);
  *count = n;
  return cmds;
}
