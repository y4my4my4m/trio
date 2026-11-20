#ifndef PORT_SAVESLOTS_H
#define PORT_SAVESLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "snes/snes.h"

#ifdef __cplusplus
extern "C" {
#endif

//Save-state slots in a directory (docs/enhancements.md "Save slots").
//Per slot: <stem>.state (snes_state_save image), <stem>.png (thumbnail,
//RGB8, half resolution of the frame), <stem>.txt (key = value metadata).
//Slot 0 quick, 1..10 manual, 11..13 autosave; stems quick, slotN, autoN.
enum {
  SS_QUICK = 0,
  SS_MANUAL = 10,
  SS_AUTO_FIRST = SS_MANUAL + 1,
  SS_AUTO_COUNT = 3,
  SS_COUNT = SS_AUTO_FIRST + SS_AUTO_COUNT,
};

enum { SS_OP_SAVE = 1, SS_OP_LOAD, SS_OP_DELETE };

typedef struct SlotInfo {
  bool exists;             //state file present
  bool compatible;         //magic, size and ROM match this build
  const char *why;         //reason when !compatible
  int64_t timestamp;       //unix seconds; file mtime without metadata
  uint64_t frames;         //Snes.frame_count: frames since power-on, carried through states
  char rom_sha1[41];       //empty without metadata
  uint32_t state_size;     //file bytes
  uint32_t state_magic;
  char location[40];
  uint32_t serial;         //save order across slots, 0 without metadata
  uint32_t gen;            //bumped when the slot's files change
} SlotInfo;

//Creates dir and hashes the ROM. Copies a legacy ./quick.state into the
//quick slot when the slot is empty.
void ss_init(const char *dir, const uint8_t *rom, uint32_t rom_size);
const char *ss_rom_sha1(void);
void ss_path(int slot, const char *ext, char *out, size_t n);
const char *ss_name(int slot, char *buf, size_t n);  //"Quick", "Slot 3", "Auto 1"

const SlotInfo *ss_info(int slot);  //cached; rescanned after ss_refresh
void ss_refresh(int slot);          //-1: all

bool ss_save(int slot, Snes *s);
//0 loaded, else a static error string.
const char *ss_load(int slot, Snes *s);
bool ss_delete(int slot);
//Runs SS_OP_* and writes a toast into msg. True when a state was loaded;
//the caller resets frontend state (enh_reset, ss_auto_rebase).
bool ss_do(int op, int slot, Snes *s, char *msg, size_t n);

//Autosave slot to overwrite: first empty, else lowest serial.
int ss_auto_pick(void);
//Newest existing compatible autosave, -1 none.
int ss_auto_newest(void);
//Stage-entry / timed autosave. Call once per emulated frame. Saves only in
//stage gameplay at full brightness without forced blank, after the screen
//has been steady for 30 frames, once per ss_auto_stage_entered (or every
//`minutes`). Returns the slot written, -1 none.
int ss_auto_tick(Snes *s, bool enabled, int minutes, bool in_area);
void ss_auto_stage_entered(void);
//Clears a pending stage-entry autosave and restarts the timer.
void ss_auto_rebase(const Snes *s, bool stage_known);
//Location label stored with the next save ("In stage", "Menu").
void ss_set_location(const char *loc);

//Thumbnail pixels, RGB8, malloc'd; false if absent or unreadable.
bool ss_thumb(int slot, uint8_t **rgb, int *w, int *h);

//PNG subset: 8-bit RGB, no interlace, filter 0, zlib stored blocks. The
//reader accepts exactly what the writer emits.
bool png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);
bool png_read_rgb(const char *path, uint8_t **rgb, int *w, int *h);
void sha1_hex(const uint8_t *data, size_t n, char out[41]);

#ifdef __cplusplus
}
#endif

#endif
