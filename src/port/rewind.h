#ifndef PORT_REWIND_H
#define PORT_REWIND_H

#include <stdbool.h>
#include <stddef.h>
#include "snes/snes.h"

//Rewind history: a snapshot every RW_INTERVAL frames, kept as reverse
//deltas (older = newer XOR delta), zero-run encoded. The framebuffer is not
//part of a snapshot; the frontend emulates one frame after each step back
//to show the picture.

#define RW_INTERVAL 2

void rw_init(void);
void rw_clear(void);                  //after reset / state load
void rw_set_capacity(int seconds);
//Call after every emulated frame; records every RW_INTERVAL frames.
void rw_record(Snes *s);
//Restores the previous snapshot into s. False when history is empty.
bool rw_step_back(Snes *s);
int rw_count(void);
size_t rw_bytes(void);

#endif
