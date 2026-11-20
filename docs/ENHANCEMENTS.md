# Enhancements

All optional, set in the port menu (F1 or Esc, or the gamepad's Guide
button) and saved to `trio.ini`. The most used ones are also in the game:
OPTION MODE > PORT OPTIONS on the title screen (docs/MENUS.md).

## Game tab

- Save / load the selected slot, reset, quit.
- Fast forward (hold Tab) at 2-16x.
- Rewind (hold R): a snapshot every 2 frames, kept as reverse XOR deltas
  of the machine state without the framebuffer (`src/port/rewind.c`). About
  12 KB per snapshot in stages, 5-60 s of history. Stepping back plays two
  snapshots per frame and emulates one frame to show the picture.
- Pause when the window loses focus.

## Save/Load tab

Quick slot, slots 1-10 and 3 rotating autosaves, each with a thumbnail and
metadata in `saves/` (`src/port/saveslots.c`). The autosave triggers when a
stage starts (the stage detector from docs/WIDESCREEN.md, once the picture
is steady), and optionally every N minutes. States from another build or
ROM are listed as incompatible.

## Gameplay tab

Input helpers applied in stages only; menus always see the real buttons.

- Auto-charge: the game sees shoot held all the time, so the blaster is
  always charging. Pressing shoot releases it for two frames, which fires
  the charged shot.
- Turbo fire: holding shoot presses and releases it every N frames.
- Infinite health: after a hit, the hero's health ($7E:09FF, bits 0-6) is
  put back to the highest value seen this stage, which includes life
  upgrades. Instant-death spikes and pits still count.
- Infinite lives: when the lives counter ($7E:1FB4) drops, it is put back.

The assists change machine state, so `trio-verify` runs are only identical
with them off (the default).

## Video tab

Window scale, fullscreen, integer scaling, sharp or smooth filter, 8:7
pixel aspect, vsync, widescreen (docs/WIDESCREEN.md), HUD at the screen
edges in widescreen, no per-line sprite limit (less flicker), scanlines, FPS
counter.

## Audio tab

Volume, mute when unfocused.

## Controls tab

Two keyboard bindings per SNES button (click, then press a key; Esc
cancels, Backspace clears), and one gamepad binding per button (click, then
press a pad button or trigger), with presets by position (SNES layout) or by
label. Every connected pad drives the player (hot-plug works), with an
adjustable stick deadzone.

Pad hotkeys (optional, on by default) use Back as a modifier: Back + LB
rewind (hold), Back + RB fast forward (hold), Back + Y save, Back + X load,
Back + Start menu. The Guide button always opens the menu. While Back is down
nothing reaches the game; a lone tap of Back is sent as Select. The menu can
be navigated with the pad.

| Hotkey | |
|---|---|
| F1 / Esc, pad Guide | menu |
| Tab (hold) | fast forward |
| R (hold) | rewind |
| P / O | pause / advance one frame |
| F5 / F9 | save / load selected slot |
| F6 / F7 | previous / next slot |
| Shift+F9 | load newest autosave |
| F11, Alt+Enter | fullscreen |
| F12 | screenshot PNG |
| Ctrl+R | reset |

## Test hooks

`TRIO_UI_MENU=1` opens the menu at start. `TRIO_UI_TAB=video` (or `saves`,
`gameplay`, `controls`) selects a tab. `TRIO_UI_SHOT=n:file.bmp` saves the
composed window at the n-th presented frame and quits. With
`SDL_VIDEODRIVER=dummy` this works without a display.
