#!/usr/bin/env python3
"""Finds the three games (USA) in a directory by their ROM headers,
whatever the files are called (.sfc / .smc, with or without a copier
header). Prints "N path" per game found (N = 1, 2, 3); gN.sfc wins.

    python3 tools/find_roms.py rom
"""
import os
import sys

#Header checksum word ($7FDE) of each game.
SUMS = {0x55FB: 1, 0x09B7: 2, 0x4055: 3}


def game_of(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) % 0x8000 == 0x200:
        data = data[0x200:]
    if len(data) < 0x8000:
        return 0
    inv = data[0x7FDC] | data[0x7FDD] << 8
    total = data[0x7FDE] | data[0x7FDF] << 8
    return SUMS.get(total, 0) if inv ^ total == 0xFFFF else 0


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else 'rom'
    found = {}
    names = sorted(os.listdir(d)) if os.path.isdir(d) else []
    for g in (1, 2, 3):
        p = os.path.join(d, 'g%d.sfc' % g)
        if os.path.isfile(p) and game_of(p) == g:
            found[g] = p
    for n in names:
        if not n.lower().endswith(('.sfc', '.smc')):
            continue
        p = os.path.join(d, n)
        g = game_of(p)
        if g and g not in found:
            found[g] = p
    for g in sorted(found):
        print(g, os.path.abspath(found[g]))


if __name__ == '__main__':
    main()
