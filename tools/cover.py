#!/usr/bin/env python3
"""Records executed PCs of one game into tools/gN/coverage.txt (seeds for
tools/disasm.py): the attract mode, then a new game played with random
input and health refilled. Extra phases take a state file.

    tools/cover.py GAME [--frames N] [--state FILE] [--seed S]
"""
import argparse
import random
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sp import *  # noqa

PLAYER = {1: 0x0BA8, 2: 0x09D8, 3: 0x09D8}

ap = argparse.ArgumentParser()
ap.add_argument('game', type=int)
ap.add_argument('--frames', type=int, default=20000)
ap.add_argument('--state')
ap.add_argument('--attract', type=int, default=6000)
ap.add_argument('--seed', type=int, default=1)
a = ap.parse_args()
rnd = random.Random(a.seed)
rom = os.path.join(ROOT, 'rom', 'g%d.sfc' % a.game)
out = os.path.join(HERE, 'g%d' % a.game, 'coverage.txt')
m = Machine(rom, recomp=False)
m.coverage_on()
if a.state:
    m.load(a.state)
else:
    if a.attract:
        m.run(a.attract)
        write_coverage(out, m.coverage())
        m = Machine(rom, recomp=False)
        m.coverage_on()
    for f in range(0, 3000, 300):
        m.run(298)
        m.press(START)
hp = PLAYER[a.game] + 0x27
pads = [B, Y, A, LEFT, RIGHT, UP, DOWN, L, R, X]
held = 0
for f in range(a.frames):
    if f % rnd.choice([4, 8, 16, 30]) == 0:
        held = 0
        for _ in range(rnd.randint(0, 3)):
            held |= rnd.choice(pads)
        if rnd.random() < 0.6:
            held |= RIGHT
        if rnd.random() < 0.01:
            held = START
        if held & LEFT and held & RIGHT:
            held &= ~LEFT
    if (m.w8(hp) & 0x7F) < 8:
        m.set8(hp, (m.w8(hp) & 0x80) | 0x10)
    m.run(1, held)
write_coverage(out, m.coverage())
print('frame', m.frame, 'covered', sum(1 for v in bytes(m.coverage()) if v), '->', out)
