#!/usr/bin/env python3
"""Coverage of a game's title menus: OPTION MODE (every item, Left/Right on
each), PASS WORD, and back. Merged into tools/gN/coverage.txt.

    tools/cover_menus.py GAME
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sp import *  # noqa

g = int(sys.argv[1])
rom = os.path.join(ROOT, 'rom', 'g%d.sfc' % g)
m = Machine(rom, recomp=False)
m.coverage_on()
# Title menu: START skips the intro (G1 twice).
starts = {1: (900, 1650), 2: (900,), 3: (900,)}[g]
for f in starts:
    m.run(f - m.frame)
    m.press(START)
m.run(150)


def tap(pad, wait=12):
    m.press(pad, hold=2, after=wait)


# OPTION MODE: down twice, START.
tap(DOWN)
tap(DOWN)
tap(START, 60)
for k in range(12):
    tap(LEFT)
    tap(RIGHT)
    tap(RIGHT)
    tap(DOWN)
for k in range(12):
    tap(UP)
# Exit: up from the top wraps to EXIT.
tap(UP)
tap(START, 60)
tap(B, 60)
m.run(120)
# PASS WORD.
tap(DOWN)
tap(START, 80)
for k in range(10):
    tap(RIGHT)
    tap(B)
    tap(DOWN)
tap(START, 80)
m.run(200)
write_coverage(os.path.join(HERE, 'g%d' % g, 'coverage.txt'), m.coverage())
m.shot('/tmp/menus%d.png' % g)
print('covered', sum(1 for v in bytes(m.coverage()) if v))
