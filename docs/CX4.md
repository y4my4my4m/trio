# Cx4 (high-level)

The Cx4 is a Hitachi HG51B DSP with a small program ROM. Rather than running
its microcode, `src/snes/cx4.c` implements each command the games issue. The
chip's own trig tables are computed at start-up, so no extra dump is needed.

## Interface

| Address | Access | Meaning |
|---|---|---|
| $6000-$6BFF | r/w | data RAM (OAM buffer, bitmaps, vertex lists) |
| $7F40-$7F42 | w | DMA source (24-bit, ROM or WRAM) |
| $7F43-$7F44 | w | DMA length |
| $7F45-$7F46 | w | DMA destination in Cx4 RAM |
| $7F47 | w | start DMA |
| $7F4D | w | sub-function for command $00 |
| $7F4F | w | start command |
| $7F5E | r | status; always 0 (commands finish on the write) |
| $7F80-$7FAF | r/w | parameters and results |

## Commands

| $7F4F | $7F4D | Function |
|---|---|---|
| $00 | $00 | build OAM from the object list at $6220 |
| $00 | $03 / $07 | scale + rotate a 4bpp bitmap into tiles |
| $00 | $05 | project a vertex list, build line steps |
| $00 | $08 | draw a wireframe model |
| $00 | $0B | break-up (disintegrate) effect |
| $00 | $0C | bitplane wave |
| $01 | | clear canvas and draw a wireframe |
| $05 $0D $10 $13 $15 $1F $22 $25 $2D | | math: propulsion, vector length, polar to rectangular, distance, atan, trapezoid, multiply, rotate point |
| $40 $54 $5C $89 | | self-test: checksum, square, immediate constants, version |
| <$40, multiple of 4 | $0E | select a register bank |

G3 runs the self-test commands at every boot and shows an error screen if one
of them does not match (the "IMMEDIATE REGISTER ERROR" screen is the $5C test).
The values used here are the ones the game checks for.

## OAM builder

Objects at $6220, 16 bytes each: X (16), Y (16), attributes, base tile,
extra attribute bits, 24-bit pointer to a piece list. The global camera is at
$6621/$6623, the object count at $6620 and the first free OAM slot at $6626.
A piece list is a count followed by 4-byte pieces (flags with size and flip
bits, signed X/Y offsets, tile offset). Flipped objects mirror each piece about
the object origin. Pieces entirely off-screen are dropped, and unused slots get
Y = $E0.
