#!/usr/bin/env python3
"""65816 tracing disassembler for the game 1 games (USA), SNES LoROM (+ Cx4).

Usage:
    disasm.py [--rom ROM] [--hints HINTS] [--trace FILE ...] [--out DIR] [-q]

Static traversal starts at the reset/NMI/IRQ/BRK/COP vectors plus hint entries
and dynamic-trace seeds. Each function is analysed once per entry flag state
(m, x, e); callee exit flags are computed and propagated to a fixpoint.

Flag values: 1, 0, or unknown (None / '?'). With e=1, m=x=1.

Hint file format (one directive per line, '#' starts a comment, addresses are
BB:AAAA or BBAAAA hex, flags are 0/1):
    entry    BB:AAAA [m=0|1] [x=0|1] [e=0|1] [name=Label]
             Extra function entry point with the given entry flags.
    name     BB:AAAA Label
             Label for an address (function, table, location).
    table    BB:AAAA count=N [bank=BB] [type=word|long] [call=0|1] [m=] [x=] [e=]
             Pointer table. word: 16-bit entries, target bank = bank (default:
             table bank). long: 24-bit entries. call=1: targets are functions,
             otherwise jump targets of the dispatching function. A detected
             JMP/JSR (abs,X) dispatch through this address uses count from here.
             m/x/e seed the targets when no dispatch site is found.
    data     BB:AAAA-BB:AAAA | BB:AAAA len=N
             Bytes that are never decoded as code (end inclusive).
    flags    BB:AAAA [m=] [x=] [e=]
             Force flag state on arrival at the address.
    noreturn BB:AAAA
             Function never returns to its caller.
    inline   BB:AAAA len=N
             Callee reads N inline bytes after its JSR/JSL and returns past them.
    stop     BB:AAAA
             End every path at the address.

Dynamic trace format (--trace, repeatable; authoritative seeds):
    Text: one record per line, '[E]BBAAAA MX': optional 'E' (emulation mode)
    immediately followed by the 24-bit PC as 6 hex digits, a space, then M and
    X as two binary digits (e.g. '00E34C 11', 'E00E34C 11'). Also accepted:
    flag field 'EMX' (three digits) or 'E' + 'MX'. '#' comments and blank
    lines ignored. A PC may appear once per distinct flag state.
    Binary: 4-byte records, PC little-endian in bytes 0-2, byte 3 = P-style
    flags: bit5 M, bit4 X, bit0 E. Detected when the file is not ASCII text.
    Traced states fill unknown m/x at the traced PC; traced PCs not reached
    statically are traversed as 'trace' roots. Conflicts with static state
    are listed in map.json.

Outputs in --out: bank_XX.asm (XX = ROM bank index, file offset >> 15),
map.json, and a summary on stdout.

map.json functions[].variants[].code: one record per decoded instruction of
the variant, [addr, m, x, e, successors], successors = [[addr, m, x, e], ...]:
the arrival states of the paths leaving the instruction (branch targets,
fall-through, call continuation, push-RTS targets). A successor absent from
code is a path end (undecodable, stop hint, non-ROM).
"""
import argparse
import json
import os
import sys
from collections import defaultdict, deque

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEF_ROM = os.path.join(ROOT, 'rom', 'g3.sfc')
DEF_HINTS = os.path.join(HERE, 'g3', 'hints.txt')
DEF_OUT = os.path.join(ROOT, 'build', 'disasm')

# ---------------------------------------------------------------- opcodes
OPS = [None] * 256
_ALU = ['ORA', 'AND', 'EOR', 'ADC', 'STA', 'LDA', 'CMP', 'SBC']
_ALUCOL = {0x01: 'dpxi', 0x03: 'sr', 0x05: 'dp', 0x07: 'dpil', 0x09: 'immm',
           0x0D: 'abs', 0x0F: 'long', 0x11: 'dpiy', 0x12: 'dpi', 0x13: 'sriy',
           0x15: 'dpx', 0x17: 'dpily', 0x19: 'absy', 0x1D: 'absx', 0x1F: 'longx'}
for _i, _mn in enumerate(_ALU):
    for _c, _md in _ALUCOL.items():
        OPS[_i * 0x20 + _c] = (_mn, _md)
OPS[0x89] = ('BIT', 'immm')
_REST = {
    0x00: ('BRK', 'imm8'), 0x02: ('COP', 'imm8'), 0x04: ('TSB', 'dp'), 0x06: ('ASL', 'dp'),
    0x08: ('PHP', 'imp'), 0x0A: ('ASL', 'acc'), 0x0B: ('PHD', 'imp'), 0x0C: ('TSB', 'abs'),
    0x0E: ('ASL', 'abs'),
    0x10: ('BPL', 'rel'), 0x14: ('TRB', 'dp'), 0x16: ('ASL', 'dpx'), 0x18: ('CLC', 'imp'),
    0x1A: ('INC', 'acc'), 0x1B: ('TCS', 'imp'), 0x1C: ('TRB', 'abs'), 0x1E: ('ASL', 'absx'),
    0x20: ('JSR', 'abs'), 0x22: ('JSL', 'long'), 0x24: ('BIT', 'dp'), 0x26: ('ROL', 'dp'),
    0x28: ('PLP', 'imp'), 0x2A: ('ROL', 'acc'), 0x2B: ('PLD', 'imp'), 0x2C: ('BIT', 'abs'),
    0x2E: ('ROL', 'abs'),
    0x30: ('BMI', 'rel'), 0x34: ('BIT', 'dpx'), 0x36: ('ROL', 'dpx'), 0x38: ('SEC', 'imp'),
    0x3A: ('DEC', 'acc'), 0x3B: ('TSC', 'imp'), 0x3C: ('BIT', 'absx'), 0x3E: ('ROL', 'absx'),
    0x40: ('RTI', 'imp'), 0x42: ('WDM', 'imm8'), 0x44: ('MVP', 'blk'), 0x46: ('LSR', 'dp'),
    0x48: ('PHA', 'imp'), 0x4A: ('LSR', 'acc'), 0x4B: ('PHK', 'imp'), 0x4C: ('JMP', 'abs'),
    0x4E: ('LSR', 'abs'),
    0x50: ('BVC', 'rel'), 0x54: ('MVN', 'blk'), 0x56: ('LSR', 'dpx'), 0x58: ('CLI', 'imp'),
    0x5A: ('PHY', 'imp'), 0x5B: ('TCD', 'imp'), 0x5C: ('JML', 'long'), 0x5E: ('LSR', 'absx'),
    0x60: ('RTS', 'imp'), 0x62: ('PER', 'rell'), 0x64: ('STZ', 'dp'), 0x66: ('ROR', 'dp'),
    0x68: ('PLA', 'imp'), 0x6A: ('ROR', 'acc'), 0x6B: ('RTL', 'imp'), 0x6C: ('JMP', 'absi'),
    0x6E: ('ROR', 'abs'),
    0x70: ('BVS', 'rel'), 0x74: ('STZ', 'dpx'), 0x76: ('ROR', 'dpx'), 0x78: ('SEI', 'imp'),
    0x7A: ('PLY', 'imp'), 0x7B: ('TDC', 'imp'), 0x7C: ('JMP', 'absxi'), 0x7E: ('ROR', 'absx'),
    0x80: ('BRA', 'rel'), 0x82: ('BRL', 'rell'), 0x84: ('STY', 'dp'), 0x86: ('STX', 'dp'),
    0x88: ('DEY', 'imp'), 0x8A: ('TXA', 'imp'), 0x8B: ('PHB', 'imp'), 0x8C: ('STY', 'abs'),
    0x8E: ('STX', 'abs'),
    0x90: ('BCC', 'rel'), 0x94: ('STY', 'dpx'), 0x96: ('STX', 'dpy'), 0x98: ('TYA', 'imp'),
    0x9A: ('TXS', 'imp'), 0x9B: ('TXY', 'imp'), 0x9C: ('STZ', 'abs'), 0x9E: ('STZ', 'absx'),
    0xA0: ('LDY', 'itrio'), 0xA2: ('LDX', 'itrio'), 0xA4: ('LDY', 'dp'), 0xA6: ('LDX', 'dp'),
    0xA8: ('TAY', 'imp'), 0xAA: ('TAX', 'imp'), 0xAB: ('PLB', 'imp'), 0xAC: ('LDY', 'abs'),
    0xAE: ('LDX', 'abs'),
    0xB0: ('BCS', 'rel'), 0xB4: ('LDY', 'dpx'), 0xB6: ('LDX', 'dpy'), 0xB8: ('CLV', 'imp'),
    0xBA: ('TSX', 'imp'), 0xBB: ('TYX', 'imp'), 0xBC: ('LDY', 'absx'), 0xBE: ('LDX', 'absy'),
    0xC0: ('CPY', 'itrio'), 0xC2: ('REP', 'imm8'), 0xC4: ('CPY', 'dp'), 0xC6: ('DEC', 'dp'),
    0xC8: ('INY', 'imp'), 0xCA: ('DEX', 'imp'), 0xCB: ('WAI', 'imp'), 0xCC: ('CPY', 'abs'),
    0xCE: ('DEC', 'abs'),
    0xD0: ('BNE', 'rel'), 0xD4: ('PEI', 'dpi'), 0xD6: ('DEC', 'dpx'), 0xD8: ('CLD', 'imp'),
    0xDA: ('PHX', 'imp'), 0xDB: ('STP', 'imp'), 0xDC: ('JML', 'absil'), 0xDE: ('DEC', 'absx'),
    0xE0: ('CPX', 'itrio'), 0xE2: ('SEP', 'imm8'), 0xE4: ('CPX', 'dp'), 0xE6: ('INC', 'dp'),
    0xE8: ('INX', 'imp'), 0xEA: ('NOP', 'imp'), 0xEB: ('XBA', 'imp'), 0xEC: ('CPX', 'abs'),
    0xEE: ('INC', 'abs'),
    0xF0: ('BEQ', 'rel'), 0xF4: ('PEA', 'imm16'), 0xF6: ('INC', 'dpx'), 0xF8: ('SED', 'imp'),
    0xFA: ('PLX', 'imp'), 0xFB: ('XCE', 'imp'), 0xFC: ('JSR', 'absxi'), 0xFE: ('INC', 'absx'),
}
for _k, _v in _REST.items():
    assert OPS[_k] is None, hex(_k)
    OPS[_k] = _v
assert all(OPS), 'opcode table incomplete'

MODE_LEN = {'imp': 1, 'acc': 1, 'imm8': 2, 'dp': 2, 'dpx': 2, 'dpy': 2, 'dpi': 2,
            'dpxi': 2, 'dpiy': 2, 'dpil': 2, 'dpily': 2, 'sr': 2, 'sriy': 2, 'rel': 2,
            'abs': 3, 'absx': 3, 'absy': 3, 'absi': 3, 'absxi': 3, 'absil': 3,
            'rell': 3, 'blk': 3, 'imm16': 3, 'long': 4, 'longx': 4}
COND_BRANCH = {'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ'}
# Opcodes that end a plausibility scan as implausible code.
BAD_START = {0x00, 0x02, 0x42, 0xDB, 0xFF}
# Instructions that leave A unchanged.
A_KEEP = {'STA', 'STX', 'STY', 'STZ', 'PHA', 'PHX', 'PHY', 'PHP', 'PHB', 'PHD', 'PHK',
          'PEA', 'PEI', 'PER', 'CMP', 'CPX', 'CPY', 'BIT', 'LDX', 'LDY', 'INX', 'INY',
          'DEX', 'DEY', 'TAX', 'TAY', 'TXS', 'TCS', 'TCD', 'CLC', 'SEC', 'CLI', 'SEI',
          'CLD', 'SED', 'CLV', 'REP', 'SEP', 'NOP', 'TSB', 'TRB', 'PLX', 'PLY', 'PLB',
          'PLD', 'PLP', 'WAI', 'XCE', 'TXY', 'TYX', 'TSX', 'WDM'} | COND_BRANCH
RMW_MEM = {'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'}

# ---------------------------------------------------------------- SNES registers
HWREG = {
    0x2100: 'INIDISP', 0x2101: 'OBSEL', 0x2102: 'OAMADDL', 0x2103: 'OAMADDH',
    0x2104: 'OAMDATA', 0x2105: 'BGMODE', 0x2106: 'MOSAIC', 0x2107: 'BG1SC',
    0x2108: 'BG2SC', 0x2109: 'BG3SC', 0x210A: 'BG4SC', 0x210B: 'BG12NBA',
    0x210C: 'BG34NBA', 0x210D: 'BG1HOFS', 0x210E: 'BG1VOFS', 0x210F: 'BG2HOFS',
    0x2110: 'BG2VOFS', 0x2111: 'BG3HOFS', 0x2112: 'BG3VOFS', 0x2113: 'BG4HOFS',
    0x2114: 'BG4VOFS', 0x2115: 'VMAIN', 0x2116: 'VMADDL', 0x2117: 'VMADDH',
    0x2118: 'VMDATAL', 0x2119: 'VMDATAH', 0x211A: 'M7SEL', 0x211B: 'M7A',
    0x211C: 'M7B', 0x211D: 'M7C', 0x211E: 'M7D', 0x211F: 'M7X', 0x2120: 'M7Y',
    0x2121: 'CGADD', 0x2122: 'CGDATA', 0x2123: 'W12SEL', 0x2124: 'W34SEL',
    0x2125: 'WOBJSEL', 0x2126: 'WH0', 0x2127: 'WH1', 0x2128: 'WH2', 0x2129: 'WH3',
    0x212A: 'WBGLOG', 0x212B: 'WOBJLOG', 0x212C: 'TM', 0x212D: 'TS', 0x212E: 'TMW',
    0x212F: 'TSW', 0x2130: 'CGWSEL', 0x2131: 'CGADSUB', 0x2132: 'COLDATA',
    0x2133: 'SETINI', 0x2134: 'MPYL', 0x2135: 'MPYM', 0x2136: 'MPYH', 0x2137: 'SLHV',
    0x2138: 'RDOAM', 0x2139: 'RDVRAML', 0x213A: 'RDVRAMH', 0x213B: 'RDCGRAM',
    0x213C: 'OPHCT', 0x213D: 'OPVCT', 0x213E: 'STAT77', 0x213F: 'STAT78',
    0x2180: 'WMDATA', 0x2181: 'WMADDL', 0x2182: 'WMADDM', 0x2183: 'WMADDH',
    0x4016: 'JOYSER0', 0x4017: 'JOYSER1',
    0x4200: 'NMITIMEN', 0x4201: 'WRIO', 0x4202: 'WRMPYA', 0x4203: 'WRMPYB',
    0x4204: 'WRDIVL', 0x4205: 'WRDIVH', 0x4206: 'WRDIVB', 0x4207: 'HTIMEL',
    0x4208: 'HTIMEH', 0x4209: 'VTIMEL', 0x420A: 'VTIMEH', 0x420B: 'MDMAEN',
    0x420C: 'HDMAEN', 0x420D: 'MEMSEL', 0x4210: 'RDNMI', 0x4211: 'TIMEUP',
    0x4212: 'HVBJOY', 0x4213: 'RDIO', 0x4214: 'RDDIVL', 0x4215: 'RDDIVH',
    0x4216: 'RDMPYL', 0x4217: 'RDMPYH', 0x4218: 'JOY1L', 0x4219: 'JOY1H',
    0x421A: 'JOY2L', 0x421B: 'JOY2H', 0x421C: 'JOY3L', 0x421D: 'JOY3H',
    0x421E: 'JOY4L', 0x421F: 'JOY4H',
}
for _a in range(0x2140, 0x2180):
    HWREG[_a] = 'APUIO%d' % (_a & 3)
_DMAREG = ['DMAP', 'BBAD', 'A1TL', 'A1TH', 'A1B', 'DASL', 'DASH', 'DASB', 'A2AL',
           'A2AH', 'NTRL', 'UNUSED']
for _ch in range(8):
    for _r, _nm in enumerate(_DMAREG):
        HWREG[0x4300 | (_ch << 4) | _r] = '%s%d' % (_nm, _ch)
    HWREG[0x430F | (_ch << 4)] = 'UNUSED%d' % _ch


def hwname(addr16):
    n = HWREG.get(addr16)
    if n:
        return n
    if HWREG.get(addr16 - 1) and (0x2100 <= addr16 < 0x2200 or 0x4200 <= addr16 < 0x4400):
        return HWREG[addr16 - 1] + '+1'
    return None


# ---------------------------------------------------------------- helpers
def fa(a):
    return '%02X:%04X' % (a >> 16, a & 0xFFFF)


def fs(v):
    return '?' if v is None else str(v)


def parse_addr(s):
    s = s.replace('$', '').replace(':', '')
    return int(s, 16)


def join_state(states):
    states = list(states)
    if not states:
        return None
    out = []
    for i in range(3):
        vals = {s[i] for s in states}
        out.append(vals.pop() if len(vals) == 1 else None)
    return tuple(out)


class Rom:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        if len(self.data) % 0x8000 == 0x200:
            self.data = self.data[0x200:]
        self.size = len(self.data)

    def off(self, a):
        bank, addr = a >> 16, a & 0xFFFF
        if addr < 0x8000 or (0x7E <= bank <= 0x7F):
            return None
        return (((bank & 0x7F) << 15) | (addr & 0x7FFF)) % self.size

    def byte(self, a):
        o = self.off(a)
        return None if o is None else self.data[o]

    def word(self, a):
        lo = self.byte(a)
        hi = self.byte((a & 0xFF0000) | ((a + 1) & 0xFFFF))
        return None if lo is None or hi is None else lo | (hi << 8)

    def long(self, a):
        w = self.word(a)
        b = self.byte((a & 0xFF0000) | ((a + 2) & 0xFFFF))
        return None if w is None or b is None else w | (b << 16)


# ---------------------------------------------------------------- instruction
class Ins:
    __slots__ = ('pc', 'off', 'op', 'mn', 'mode', 'len', 'val', 'raw', 'm', 'x', 'e',
                 'targets', 'note', 'flagchg')

    def __init__(self, pc, off, op, mn, mode, ln, val, raw, m, x, e):
        self.pc, self.off, self.op, self.mn, self.mode = pc, off, op, mn, mode
        self.len, self.val, self.raw, self.m, self.x, self.e = ln, val, raw, m, x, e
        self.targets = []
        self.note = None
        self.flagchg = False


def decode(rom, pc, m, x):
    """Returns Ins, or a string reason on failure."""
    off = rom.off(pc)
    if off is None:
        return 'nonrom'
    op = rom.data[off]
    mn, mode = OPS[op]
    if mode == 'immm':
        if m is None:
            return 'unknown_m'
        ln = 3 - m
    elif mode == 'itrio':
        if x is None:
            return 'unknown_x'
        ln = 3 - x
    else:
        ln = MODE_LEN[mode]
    raw = bytes(rom.byte((pc & 0xFF0000) | ((pc + i) & 0xFFFF)) or 0 for i in range(ln))
    if any(rom.off((pc & 0xFF0000) | ((pc + i) & 0xFFFF)) is None for i in range(ln)):
        return 'nonrom'
    val = 0
    for i in range(ln - 1, 0, -1):
        val = (val << 8) | raw[i]
    return Ins(pc, off, op, mn, mode, ln, val, raw, m, x, None)


# ---------------------------------------------------------------- path state
class Path:
    """One traversal path. stack: list of byte cells, top at end; a cell is
    ('b', value|None) or ('P', (m, x)). base: stack bottom equals function-entry S."""
    __slots__ = ('pc', 'm', 'x', 'e', 'c', 'stack', 'base', 'db', 'd', 'a', 'hist',
                 'consumed', 'ssave', 'prev')

    def copy(self, pc=None):
        p = Path()
        p.pc = self.pc if pc is None else pc
        p.m, p.x, p.e, p.c = self.m, self.x, self.e, self.c
        p.stack = list(self.stack)
        p.base, p.db, p.d, p.a = self.base, self.db, self.d, self.a
        p.hist, p.consumed = self.hist, self.consumed
        p.ssave = self.ssave
        p.prev = self.prev
        return p

    def pkey(self):
        return tuple(c[1] for c in self.stack if c[0] == 'P') + (self.base,)

    def push(self, n, val=None):
        # 65816 pushes high byte first.
        for i in range(n - 1, -1, -1):
            self.stack.append(('b', None if val is None else (val >> (8 * i)) & 0xFF))
        if len(self.stack) > 48:
            self.stack = self.stack[-48:]
            self.base = False

    def pull(self, n):
        """Returns (value|None, list_of_cells). Sets consumed on underflow at base."""
        cells = []
        for _ in range(n):
            if self.stack:
                cells.append(self.stack.pop())
            else:
                if self.base:
                    self.consumed = True
                cells.append(('b', None))
        val = 0
        for i, c in enumerate(cells):
            if c[0] != 'b' or c[1] is None:
                return None, cells
            val |= c[1] << (8 * i)
        return val, cells

    def lose_stack(self):
        self.stack = []
        self.base = False


# ---------------------------------------------------------------- function variant
class FV:
    def __init__(self, entry, st, kind='sub'):
        self.entry = entry
        self.st = st
        self.kind = kind
        self.summary = None     # (kind, m, x, e) kind: ret|noreturn|consume|unknown
        self.analyzed = False
        self.reset()
        self.dep_callers = set()   # FVs whose analysis read this summary

    def reset(self):
        self.ins = {}
        self.callees = {}          # call pc -> list of FV keys
        self.exits = []            # (pc, state, consumed)
        self.leaders = set([self.entry])
        self.incomplete = []       # (pc, reason)
        self.dispatch = []         # dicts
        self.unresolved = []       # dicts
        self.tables_used = set()
        self.reads_return = False
        self.consumes_return = False
        self.dyn_tail = False
        self.edges = {}            # (pc, m, x, e) -> set of successor (pc, m, x, e)

    @property
    def key(self):
        return (self.entry,) + self.st


# ---------------------------------------------------------------- analyser
class Disasm:
    def __init__(self, rom, hints, traces):
        self.rom = rom
        self.fvs = {}
        self.roots = []
        self.queue = deque()
        self.inq = set()
        self.stackset = set()
        self.tables = {}           # addr -> dict
        self.pending_tables = {}   # addr -> dict (unbounded, awaiting scan)
        self.names = dict(hints['names'])
        self.hint_flags = hints['flags']
        self.hint_stop = hints['stop']
        self.hint_noreturn = hints['noreturn']
        self.hint_inline = hints['inline']
        self.hint_indirect = hints['indirect']
        self.data_ranges = hints['data']
        self.trace = traces
        self.trace_conflicts = []
        self.data_offs = set()
        for lo, hi in self.data_ranges:
            a = lo
            while a <= hi:
                o = rom.off(a)
                if o is not None:
                    self.data_offs.add(o)
                a += 1
        for t in hints['tables']:
            self.tables[t['addr']] = dict(t, resolved=False, sites=set(), entries=[],
                                          source='hint')
        self.code_starts = set()
        self.code_cover = {}
        self.labels_pc = set()

    # ---- FV management
    def get_fv(self, entry, st, kind='sub'):
        if st[2] == 1:
            st = (1, 1, 1)
        k = (entry,) + st
        fv = self.fvs.get(k)
        if fv is None:
            fv = FV(entry, st, kind)
            self.fvs[k] = fv
            self.enqueue(fv)
        return fv

    def enqueue(self, fv):
        if fv.key not in self.inq:
            self.inq.add(fv.key)
            self.queue.append(fv)

    def add_root(self, entry, st, kind):
        fv = self.get_fv(entry, st, kind)
        if fv.kind == 'sub':
            fv.kind = kind
        if fv.key not in [r.key for r in self.roots]:
            self.roots.append(fv)
        return fv

    # ---- main driver
    def run(self):
        rounds = 0
        while True:
            self.fixpoint()
            self.refresh_code_map()
            n = self.resolve_pending_tables()
            rounds += 1
            if n == 0 or rounds > 64:
                break
        self.seed_trace_uncovered()
        self.fixpoint()
        self.collect()

    def fixpoint(self):
        guard = 0
        while self.queue:
            fv = self.queue.popleft()
            self.inq.discard(fv.key)
            self.analyze(fv)
            guard += 1
            if guard > 400000:
                print('fixpoint guard hit', file=sys.stderr)
                break

    def analyze(self, fv):
        if fv.key in self.stackset:
            return
        self.stackset.add(fv.key)
        old = fv.summary
        try:
            self._analyze(fv)
        finally:
            self.stackset.discard(fv.key)
        fv.analyzed = True
        if fv.summary != old:
            for c in list(fv.dep_callers):
                self.enqueue(c)

    def callee_summary(self, caller, callee):
        callee.dep_callers.add(caller)
        if not callee.analyzed and callee.key not in self.stackset:
            self.inq.discard(callee.key)
            try:
                self.queue.remove(callee)
            except ValueError:
                pass
            self.analyze(callee)
        return callee.summary

    def apply_arrival(self, p):
        fl = self.hint_flags.get(p.pc)
        if fl:
            for k, v in fl.items():
                setattr(p, k, v)
            if p.e == 1:
                p.m = p.x = 1
        if (p.m is None or p.x is None) and p.pc in self.trace:
            sts = self.trace[p.pc]
            if p.e is not None:
                sts = {s for s in sts if s[2] == p.e} or sts
            if len(sts) == 1:
                m, x, e = next(iter(sts))
                if p.m is None:
                    p.m = m
                if p.x is None:
                    p.x = x
                if p.e is None:
                    p.e = e

    def _analyze(self, fv):
        fv.reset()
        rom = self.rom
        p0 = Path()
        p0.pc = fv.entry
        p0.m, p0.x, p0.e = fv.st
        p0.c = None
        p0.stack = []
        p0.base = True
        p0.db = None
        p0.d = None
        p0.a = None
        p0.hist = ()
        p0.consumed = False
        p0.ssave = {}
        p0.prev = None
        if fv.kind == 'reset':
            p0.db, p0.d = 0, 0
        work = [p0]
        seen = set()
        while work:
            p = work.pop()
            while True:
                self.apply_arrival(p)
                pc = p.pc
                if p.prev is not None:
                    fv.edges.setdefault(p.prev, set()).add((pc, p.m, p.x, p.e))
                if pc in self.hint_stop:
                    break
                off = rom.off(pc)
                if off is None:
                    fv.unresolved.append({'site': pc, 'kind': 'nonrom_target',
                                          'target': pc})
                    fv.incomplete.append((pc, 'nonrom'))
                    break
                if off in self.data_offs:
                    fv.incomplete.append((pc, 'ran_into_data'))
                    break
                key = (pc, p.m, p.x, p.e, p.pkey(), p.consumed)
                if key in seen:
                    break
                seen.add(key)
                ins = decode(rom, pc, p.m, p.x)
                if isinstance(ins, str):
                    fv.incomplete.append((pc, ins))
                    break
                ins.e = p.e
                ik = (pc, p.m, p.x, p.e)
                prev = fv.ins.get(ik)
                if prev is not None:
                    ins = prev
                else:
                    fv.ins[ik] = ins
                p.prev = ik
                cont = self.step(fv, p, ins, work)
                p.hist = (p.hist + ((pc, ins.mn, ins.mode, ins.val),))[-10:]
                if not cont:
                    break
        self.summarize(fv)

    def summarize(self, fv):
        normal = [s for (_, s, c) in fv.exits if not c]
        if fv.entry in self.hint_noreturn:
            fv.summary = ('noreturn', None, None, None)
        elif normal:
            fv.summary = ('ret',) + join_state(normal)
        elif fv.consumes_return:
            fv.summary = ('consume', None, None, None)
        elif fv.dyn_tail:
            fv.summary = ('unknown', None, None, None)
        elif not fv.incomplete and not fv.unresolved and not any(
                d.get('pending') for d in fv.dispatch):
            fv.summary = ('noreturn', None, None, None)
        else:
            fv.summary = ('unknown', None, None, None)

    # ---- single instruction semantics; returns True to continue the path
    def step(self, fv, p, ins, work):
        mn, mode, pc, val = ins.mn, ins.mode, ins.pc, ins.val
        nxt = (pc & 0xFF0000) | ((pc + ins.len) & 0xFFFF)
        bank = pc & 0xFF0000
        carry_before = p.c
        p.c = None
        a_before = p.a
        if mn not in A_KEEP and not (mn in RMW_MEM and mode != 'acc'):
            p.a = None

        # memory-mode stack-relative access to the return address
        if mode in ('sr', 'sriy') and p.base:
            depth = len(p.stack)
            if depth < val <= depth + 3:
                if mn == 'STA':
                    fv.consumes_return = True
                    p.consumed = True
                else:
                    fv.reads_return = True

        if mn == 'REP':
            if val & 0x20 and p.e != 1:
                p.m = 0
            if val & 0x10 and p.e != 1:
                p.x = 0
            if val & 0x01:
                p.c = 0
            ins.flagchg = bool(val & 0x30)
        elif mn == 'SEP':
            if val & 0x20:
                p.m = 1
            if val & 0x10:
                p.x = 1
            if val & 0x01:
                p.c = 1
            ins.flagchg = bool(val & 0x30)
        elif mn == 'CLC':
            p.c = 0
        elif mn == 'SEC':
            p.c = 1
        elif mn == 'XCE':
            old_e = p.e
            p.e = carry_before
            p.c = old_e
            if p.e == 1:
                p.m = p.x = 1
            elif p.e is None:
                p.m = p.x = None
            ins.flagchg = True
        elif mn == 'LDA' and mode == 'immm':
            p.a = (val, 8 if p.m else 16)
        elif mn == 'TCD':
            p.d = a_before[0] if a_before and a_before[0] != 'S' and \
                a_before[1] == 16 else None
        elif mn == 'TSC':
            # A holds S; a later TCS of the same value restores the tracked stack.
            p.a = ('S', tuple(p.stack), p.base)
        elif mn == 'STA' and mode in ('dp', 'abs', 'long') and a_before and \
                a_before[0] == 'S':
            p.ssave = dict(p.ssave)
            p.ssave[(mode == 'dp', val & 0xFFFF)] = a_before
        elif mn == 'LDA' and mode in ('dp', 'abs', 'long') and \
                (mode == 'dp', val & 0xFFFF) in p.ssave:
            p.a = p.ssave[(mode == 'dp', val & 0xFFFF)]
        elif mn == 'TCS' and a_before and a_before[0] == 'S':
            p.stack = list(a_before[1])
            p.base = a_before[2]
        elif mn == 'TCS' or mn == 'TXS':
            p.lose_stack()
        elif mn == 'PHA':
            if p.m is None:
                p.lose_stack()
            else:
                n = 1 if p.m else 2
                v = a_before[0] if a_before and a_before[0] != 'S' and \
                    a_before[1] >= 8 * n else None
                p.push(n, v & (0xFF if n == 1 else 0xFFFF) if v is not None else None)
        elif mn in ('PHX', 'PHY'):
            if p.x is None:
                p.lose_stack()
            else:
                p.push(1 if p.x else 2)
        elif mn == 'PHB':
            p.push(1, p.db)
        elif mn == 'PHK':
            p.push(1, pc >> 16)
        elif mn == 'PHD':
            p.push(2, p.d)
        elif mn == 'PHP':
            p.stack.append(('P', (p.m, p.x)))
        elif mn == 'PEA':
            p.push(2, val)
        elif mn == 'PEI':
            p.push(2)
        elif mn == 'PER':
            p.push(2, (pc + 3 + (val - 0x10000 if val & 0x8000 else val)) & 0xFFFF)
        elif mn == 'PLA':
            if p.m is None:
                p.lose_stack()
            else:
                n = 1 if p.m else 2
                v, _ = p.pull(n)
                p.a = (v, 8 * n) if v is not None else None
        elif mn in ('PLX', 'PLY'):
            if p.x is None:
                p.lose_stack()
            else:
                p.pull(1 if p.x else 2)
        elif mn == 'PLB':
            v, _ = p.pull(1)
            p.db = v
        elif mn == 'PLD':
            v, _ = p.pull(2)
            p.d = v
        elif mn == 'PLP':
            if p.stack:
                c = p.stack.pop()
                if c[0] == 'P':
                    p.m, p.x = c[1]
                elif c[1] is not None:
                    p.m, p.x = (c[1] >> 5) & 1, (c[1] >> 4) & 1
                    p.c = c[1] & 1
                else:
                    p.m = p.x = None
            else:
                if p.base:
                    p.consumed = True
                p.m = p.x = None
            if p.e == 1:
                p.m = p.x = 1
            ins.flagchg = True
        elif mn == 'MVN' or mn == 'MVP':
            p.db = val & 0xFF

        # ---- control flow
        if mn in COND_BRANCH or mn == 'BRA':
            off8 = val - 0x100 if val & 0x80 else val
            tgt = bank | ((pc + 2 + off8) & 0xFFFF)
            self.add_target(ins, tgt)
            fv.leaders.add(tgt)
            if mn == 'BRA':
                p.pc = tgt
                return True
            fv.leaders.add(nxt)
            work.append(p.copy(tgt))
            p.pc = nxt
            return True
        if mn == 'BRL':
            off16 = val - 0x10000 if val & 0x8000 else val
            tgt = bank | ((pc + 3 + off16) & 0xFFFF)
            self.add_target(ins, tgt)
            fv.leaders.add(tgt)
            p.pc = tgt
            return True
        if mn == 'JMP' and mode == 'abs':
            tgt = bank | val
            self.add_target(ins, tgt)
            fv.leaders.add(tgt)
            p.pc = tgt
            return True
        if mn == 'JML' and mode == 'long':
            self.add_target(ins, val)
            fv.leaders.add(val)
            p.pc = val
            return True
        if pc in self.hint_indirect and (mode in ('absi', 'absil', 'absxi') or
                                         mn in ('RTS', 'RTL')):
            return self.hinted_indirect(fv, p, ins, work, nxt)
        if mn == 'JMP' and mode == 'absi':
            return self.indirect_jump(fv, p, ins, bank, 2, work)
        if mn == 'JML' and mode == 'absil':
            return self.indirect_jump(fv, p, ins, bank, 3, work)
        if mode == 'absxi':
            return self.table_dispatch(fv, p, ins, work, call=(mn == 'JSR'), nxt=nxt)
        if mn in ('JSR', 'JSL'):
            tgt = (bank | val) if mn == 'JSR' else val
            self.add_target(ins, tgt)
            return self.do_call(fv, p, ins, [tgt], nxt)
        if mn in ('RTS', 'RTL'):
            return self.do_return(fv, p, ins, work)
        if mn == 'RTI':
            fv.exits.append((pc, (p.m, p.x, p.e), False))
            return False
        if mn == 'STP':
            return False
        if mn == 'BRK':
            fv.incomplete.append((pc, 'brk'))
            return False
        if mn == 'COP':
            p.pc = nxt
            return True
        p.pc = nxt
        return True

    def add_target(self, ins, tgt):
        if tgt not in ins.targets:
            ins.targets.append(tgt)

    def do_call(self, fv, p, ins, targets, nxt):
        pc = ins.pc
        st = (p.m, p.x, p.e)
        sums = []
        consume = False
        inline_len = None
        for tgt in targets:
            if self.rom.off(tgt) is None:
                fv.unresolved.append({'site': pc, 'kind': 'call_nonrom', 'target': tgt})
                continue
            callee = self.get_fv(tgt, st)
            fv.callees.setdefault(pc, [])
            if callee.key not in fv.callees[pc]:
                fv.callees[pc].append(callee.key)
            s = self.callee_summary(fv, callee)
            if tgt in self.hint_inline:
                inline_len = self.hint_inline[tgt]
                cs = [s2 for (_, s2, c) in callee.exits if c]
                sums.append(('ret',) + (join_state(cs) or st))
                continue
            if s is None:
                sums.append(('ret',) + st)
            elif s[0] == 'ret':
                sums.append(s)
            elif s[0] == 'consume':
                consume = True
            elif s[0] == 'unknown':
                if callee.consumes_return:
                    consume = True
                else:
                    sums.append(('ret',) + st)
                    fv.dispatch.append({'site': pc, 'kind': 'assumed_return',
                                        'callee': tgt})
        if inline_len is not None:
            fv.dispatch.append({'site': pc, 'kind': 'inline_args', 'callee': targets[0],
                                'len': inline_len})
            nxt = (nxt & 0xFF0000) | ((nxt + inline_len) & 0xFFFF)
        elif consume:
            fv.dispatch.append({'site': pc, 'kind': 'inline_args', 'callee': targets[0],
                                'len': None})
            fv.unresolved.append({'site': pc, 'kind': 'inline_args',
                                  'target': targets[0]})
            if not sums:
                return False
        if not sums:
            return False
        js = join_state([s[1:] for s in sums])
        p.m, p.x, p.e = js
        if p.e == 1:
            p.m = p.x = 1
        p.a = None
        p.db = None
        p.c = None
        fv.leaders.add(nxt)
        p.pc = nxt
        return True

    def hinted_indirect(self, fv, p, ins, work, nxt):
        h = self.hint_indirect[ins.pc]
        pc = ins.pc
        if h['dynamic']:
            fv.dispatch.append({'site': pc, 'kind': 'dynamic'})
            if ins.mn in ('RTS', 'RTL'):
                p.pull(2 if ins.mn == 'RTS' else 3)
            self.push_return_continuation(fv, p, ins, work)
            fv.dyn_tail = True
            return False
        targets = list(h.get('targets', []))
        if 'table' in h:
            t = self.tables.get(h['table'])
            if t is None:
                sys.exit('indirect %s: table %s has no table hint' % (fa(pc), fa(h['table'])))
            if not t['resolved']:
                self.resolve_table(t, (p.m, p.x, p.e))
            fv.tables_used.add(h['table'])
            t['sites'].add((pc, (p.m, p.x, p.e), h['call']))
            targets += t['entries']
        for tg in targets:
            self.add_target(ins, tg)
        fv.dispatch.append({'site': pc, 'kind': 'hint_call' if h['call'] else 'hint_jump',
                            'count': len(targets)})
        is_call = ins.mn == 'JSR'
        if h['call']:
            if not self.do_call(fv, p, ins, targets, nxt):
                return False
            if is_call:
                return True
            # tail call: callee's return is this function's return
            fv.exits.append((pc, (p.m, p.x, p.e), p.consumed))
            return False
        for tg in targets:
            fv.leaders.add(tg)
            work.append(p.copy(tg))
        if is_call:
            p.pc = nxt
            return True
        return False

    def do_return(self, fv, p, ins, work):
        pc = ins.pc
        bank = pc & 0xFF0000
        if p.stack:
            if ins.mn == 'RTS':
                v, _ = p.pull(2)
                tgt = None if v is None else bank | ((v + 1) & 0xFFFF)
            else:
                v, _ = p.pull(3)
                tgt = None if v is None else (v & 0xFF0000) | ((v + 1) & 0xFFFF)
            if tgt is None:
                self.push_return_continuation(fv, p, ins, work)
                fv.unresolved.append({'site': pc, 'kind': 'return_to_pushed_unknown'})
                fv.incomplete.append((pc, 'push_ret_unknown'))
                return False
            fv.dispatch.append({'site': pc, 'kind': 'push_' + ins.mn.lower(),
                                'target': tgt})
            self.add_target(ins, tgt)
            fv.leaders.add(tgt)
            p.pc = tgt
            return True
        fv.exits.append((pc, (p.m, p.x, p.e), p.consumed))
        return False

    def indirect_jump(self, fv, p, ins, bank, size, work):
        pc, val = ins.pc, ins.val
        if val >= 0x8000:
            ptr = self.rom.word(val) if size == 2 else self.rom.long(val)
            if ptr is not None:
                tgt = (bank | ptr) if size == 2 else ptr
                self.add_target(ins, tgt)
                fv.leaders.add(tgt)
                fv.dispatch.append({'site': pc, 'kind': 'rom_indirect', 'target': tgt})
                p.pc = tgt
                return True
        # pushed return address before an indirect jump: record continuation
        self.push_return_continuation(fv, p, ins, work)
        u = {'site': pc, 'kind': 'indirect_ram', 'ptr': val,
             'insn': '%s %s' % (ins.mn, fmt_operand(ins, None))}
        pt = self.probable_table(p.hist, val)
        if pt is not None:
            u['probable_table'] = pt
        fv.unresolved.append(u)
        fv.incomplete.append((pc, 'indirect'))
        return False

    def push_return_continuation(self, fv, p, ins, work):
        # PEA/PER ret-1 ; JMP (...) -> handler returns to ret.
        if len(p.stack) >= 2 and all(c[0] == 'b' and c[1] is not None for c in p.stack[-2:]):
            v = p.stack[-1][1] | (p.stack[-2][1] << 8)
            tgt = (ins.pc & 0xFF0000) | ((v + 1) & 0xFFFF)
            if self.rom.off(tgt) is not None:
                fv.dispatch.append({'site': ins.pc, 'kind': 'pushed_return', 'target': tgt})
                q = p.copy(tgt)
                q.pull(2)
                work.append(q)
                return tgt
        return None

    def probable_table(self, hist, ptr):
        # LDA tbl,X ... STA ptr  (tbl in ROM)
        stored = False
        for (hpc, mn, mode, v) in reversed(hist):
            if mn == 'STA' and mode in ('dp', 'abs') and (v == ptr or v == ptr + 1 or
                                                         (v & 0xFF) == (ptr & 0xFF)):
                stored = True
                continue
            if stored and mn == 'LDA' and mode in ('absx', 'absy', 'longx'):
                t = v if mode == 'longx' else (hpc & 0xFF0000) | v
                if self.rom.off(t) is not None:
                    return t
        return None

    def table_bound(self, hist):
        """Entry count from the index computation before a (abs,X) dispatch:
        CMP/CPX #n + BCC/BCS guard, or AND #n mask, feeding X."""
        h = list(hist)
        for i in range(len(h) - 1, -1, -1):
            _, mn, mode, v = h[i]
            after = [x[1] for x in h[i + 1:]]
            if mn == 'CMP' and mode == 'immm':
                if 'TAX' not in after or 'LDX' in after or \
                        not any(b in ('BCS', 'BCC') for b in after):
                    return None
                if 'ASL' in after:
                    return v
                return (v + 1) // 2
            if mn == 'CPX' and mode == 'itrio':
                if 'TAX' in after or 'LDX' in after or \
                        not any(b in ('BCS', 'BCC') for b in after):
                    return None
                return (v + 1) // 2
            if mn == 'AND' and mode == 'immm':
                if 'TAX' not in after or 'LDX' in after or any(
                        a in ('LDA', 'PLA', 'TYA', 'TXA', 'ORA', 'ADC', 'SBC', 'EOR')
                        for a in after):
                    return None
                return v + 1 if 'ASL' in after else v // 2 + 1
            if mn in ('JSR', 'JSL', 'RTS', 'RTL', 'LDA', 'PLA', 'TYA', 'TXA'):
                return None
        return None

    def table_dispatch(self, fv, p, ins, work, call, nxt):
        pc = ins.pc
        taddr = (pc & 0xFF0000) | ins.val
        st = (p.m, p.x, p.e)
        fv.tables_used.add(taddr)
        t = self.tables.get(taddr)
        if t is None:
            if self.rom.off(taddr) is None:
                fv.unresolved.append({'site': pc, 'kind': 'table_nonrom', 'table': taddr})
                fv.incomplete.append((pc, 'table_nonrom'))
                return False
            t = {'addr': taddr, 'type': 'word', 'bank': taddr >> 16, 'call': call,
                 'count': None, 'resolved': False, 'sites': set(), 'entries': [],
                 'source': 'detected'}
            bound = self.table_bound(p.hist)
            if bound:
                t['count'] = min(bound, 256)
                t['source'] = 'detected_bounded'
            self.tables[taddr] = t
        t['sites'].add((pc, st, call))
        if not t['resolved']:
            if t['count']:
                self.resolve_table(t, st)
            else:
                self.pending_tables[taddr] = t
                fv.dispatch.append({'site': pc, 'kind': 'jsr_table' if call else
                                    'jmp_table', 'table': taddr, 'pending': True})
                if not call:
                    self.push_return_continuation(fv, p, ins, work)
                    return False
                p.pc = nxt
                p.a = p.db = None
                return True
        fv.dispatch.append({'site': pc, 'kind': 'jsr_table' if call else 'jmp_table',
                            'table': taddr, 'count': len(t['entries'])})
        targets = list(t['entries'])
        for tg in targets:
            self.add_target(ins, tg)
        if call:
            if not targets:
                p.pc = nxt
                return True
            return self.do_call(fv, p, ins, targets, nxt)
        self.push_return_continuation(fv, p, ins, work)
        for tg in targets:
            fv.leaders.add(tg)
            work.append(p.copy(tg))
        return False

    # ---- tables
    def table_targets_raw(self, t, count):
        out = []
        esz = 3 if t['type'] == 'long' else 2
        for i in range(count):
            ea = t['addr'] + esz * i
            if esz == 3:
                v = self.rom.long(ea)
                out.append(v)
            else:
                v = self.rom.word(ea)
                out.append(None if v is None else (t['bank'] << 16) | v)
        return out

    def resolve_table(self, t, st):
        """Bounded or hinted table."""
        if t['source'] == 'hint':
            t['entries'] = [tg for tg in self.table_targets_raw(t, t['count'])
                            if tg is not None and self.rom.off(tg) is not None]
            t['span'] = t['count']
        else:
            t['entries'] = self.scan_entries(t, st, t['count'], use_map=False)
        t['resolved'] = True
        self.pending_tables.pop(t['addr'], None)

    def scan_entries(self, t, st, maxcount, use_map):
        """Reads entries until an invalid one. Index 0 and 0000 entries may be
        null slots. Stops where the table would run into code it points to,
        and (use_map) into known code, labels or other tables."""
        addr = t['addr']
        esz = 3 if t['type'] == 'long' else 2
        if use_map:
            tstarts = {self.rom.off(a) for a in self.tables if a != addr}
            label_offs = {self.rom.off(a) for a in self.labels_pc}
        ents = []
        nulls = 0
        lowest = None
        t['span'] = 0
        for i in range(maxcount):
            ea = addr + esz * i
            eo = self.rom.off(ea)
            if eo is None or (ea & 0xFFFF) < (addr & 0xFFFF):
                break
            if lowest is not None and addr < lowest <= ea + esz - 1 and \
                    (lowest >> 16) == (addr >> 16):
                break
            if any((eo + k) in self.data_offs for k in range(esz)):
                break
            if use_map and i > 0 and (eo in tstarts or eo in label_offs or any(
                    (eo + k) in self.code_cover for k in range(esz))):
                break
            raw = self.table_targets_raw(dict(t, addr=ea), 1)[0]
            ok = raw is not None and (raw & 0xFFFF) >= 0x8000 and \
                self.rom.off(raw) is not None and \
                not (addr <= raw < addr + esz * (i + 1)) and self.plausible(raw, st)
            if not ok:
                if raw is not None and ((raw & 0xFFFF) == 0 or i == 0):
                    nulls += 1
                    if nulls > 2:
                        break
                    continue
                break
            nulls = 0
            ents.append(raw)
            t['span'] = i + 1
            if raw > addr and (lowest is None or raw < lowest):
                lowest = raw
        return ents

    def plausible(self, tgt, st):
        o = self.rom.off(tgt)
        if o is None or o in self.data_offs:
            return False
        if o in self.code_cover and o not in self.code_starts:
            return False
        m, x, e = st
        pc = tgt
        for _ in range(6):
            o = self.rom.off(pc)
            if o is None or o in self.data_offs:
                return False
            op = self.rom.data[o]
            if op in BAD_START:
                return False
            ins = decode(self.rom, pc, m, x)
            if isinstance(ins, str):
                return ins != 'nonrom'
            if ins.mn in ('RTS', 'RTL', 'RTI', 'JMP', 'JML', 'BRA', 'BRL'):
                return True
            if ins.mn == 'REP':
                m = 0 if ins.val & 0x20 else m
                x = 0 if ins.val & 0x10 else x
            if ins.mn == 'SEP':
                m = 1 if ins.val & 0x20 else m
                x = 1 if ins.val & 0x10 else x
            pc = (pc & 0xFF0000) | ((pc + ins.len) & 0xFFFF)
        return True

    def resolve_pending_tables(self):
        n = 0
        for addr in sorted(self.pending_tables):
            t = self.pending_tables[addr]
            sites = sorted(t['sites'], key=str)
            st = sites[0][1] if sites else (1, 1, None)
            t['entries'] = self.scan_entries(t, st, 256, use_map=True)
            t['count'] = len(t['entries'])
            t['resolved'] = True
            t['source'] = 'detected_scan'
            n += 1
            for fv in self.fvs.values():
                if addr in fv.tables_used:
                    self.enqueue(fv)
        self.pending_tables = {}
        return n

    def refresh_code_map(self):
        self.code_starts = set()
        self.code_cover = {}
        self.labels_pc = set()
        for fv in self.fvs.values():
            self.labels_pc.add(fv.entry)
            for ins in fv.ins.values():
                self.code_starts.add(ins.off)
                for k in range(ins.len):
                    self.code_cover[ins.off + k] = ins.pc
                for tg in ins.targets:
                    self.labels_pc.add(tg)

    def seed_trace_uncovered(self):
        # Lowest uncovered traced PC becomes a root; its reach is removed before
        # the next pick, so one root stands for each statically unreached region.
        self.refresh_code_map()
        for pc, sts in sorted(self.trace.items()):
            o = self.rom.off(pc)
            if o is None or o in self.code_starts:
                continue
            for st in sts:
                self.add_root(pc, st, 'trace')
            self.fixpoint()
            self.refresh_code_map()

    # ---- final collection over live variants
    def collect(self):
        live = set()
        stack = [r.key for r in self.roots]
        for t in self.tables.values():
            if t['source'] == 'hint' and not t['sites'] and t.get('st'):
                self.resolve_table(t, t['st'])
                for tg in t['entries']:
                    stack.append(self.add_root(tg, t['st'], 'table').key)
        self.fixpoint()
        while stack:
            k = stack.pop()
            if k in live:
                continue
            live.add(k)
            for keys in self.fvs[k].callees.values():
                stack.extend(keys)
        self.live = [self.fvs[k] for k in sorted(live, key=lambda k: (k[0], str(k[1:])))]
        self.ins = {}
        for fv in self.live:
            for ik, ins in fv.ins.items():
                self.ins.setdefault(ik, ins)
        # callers
        self.callers = defaultdict(set)
        for fv in self.live:
            for cpc, keys in fv.callees.items():
                for k in keys:
                    self.callers[k].add(cpc)
        # live tables
        used = set()
        for fv in self.live:
            used |= fv.tables_used
        self.live_tables = {a: t for a, t in self.tables.items()
                            if a in used or t['source'] == 'hint'}
        # trace conflicts
        static_at = defaultdict(set)
        for (pc, m, x, e) in self.ins:
            static_at[pc].add((m, x, e))
        def compat(a, b):
            return all(u is None or v is None or u == v for u, v in zip(a, b))
        for pc, sts in self.trace.items():
            if pc in static_at and not any(compat(a, b) for a in static_at[pc] for b in sts):
                self.trace_conflicts.append({'pc': pc, 'static': sorted(map(str, static_at[pc])),
                                             'trace': sorted(map(str, sts))})


# ---------------------------------------------------------------- formatting
def fmt_operand(ins, labeler):
    mode, v = ins.mode, ins.val

    def lab(t):
        if labeler:
            n = labeler(t)
            if n:
                return n
        return '$%06X' % t
    if mode in ('imp',):
        return ''
    if mode == 'acc':
        return 'A'
    if mode in ('immm', 'itrio'):
        return '#$%0*X' % (2 if ins.len == 2 else 4, v)
    if mode == 'imm8':
        return '#$%02X' % v
    if mode == 'imm16':
        return '$%04X' % v
    if mode == 'dp':
        return '$%02X' % v
    if mode == 'dpx':
        return '$%02X,X' % v
    if mode == 'dpy':
        return '$%02X,Y' % v
    if mode == 'dpi':
        return '($%02X)' % v
    if mode == 'dpxi':
        return '($%02X,X)' % v
    if mode == 'dpiy':
        return '($%02X),Y' % v
    if mode == 'dpil':
        return '[$%02X]' % v
    if mode == 'dpily':
        return '[$%02X],Y' % v
    if mode == 'sr':
        return '$%02X,S' % v
    if mode == 'sriy':
        return '($%02X,S),Y' % v
    if mode == 'rel' or mode == 'rell':
        if ins.mn == 'PER':
            off16 = v - 0x10000 if v & 0x8000 else v
            return '$%04X' % ((ins.pc + 3 + off16) & 0xFFFF)
        return lab(ins.targets[0]) if ins.targets else '$%04X' % v
    if mode == 'abs':
        if ins.mn in ('JMP', 'JSR'):
            return lab((ins.pc & 0xFF0000) | v)
        return '$%04X' % v
    if mode == 'absx':
        return '$%04X,X' % v
    if mode == 'absy':
        return '$%04X,Y' % v
    if mode == 'absi':
        return '($%04X)' % v
    if mode == 'absxi':
        t = (ins.pc & 0xFF0000) | v
        n = labeler(t) if labeler else None
        return '(%s,X)' % (n or '$%04X' % v)
    if mode == 'absil':
        return '[$%04X]' % v
    if mode == 'long':
        if ins.mn in ('JML', 'JSL'):
            return lab(v)
        return '$%06X' % v
    if mode == 'longx':
        return '$%06X,X' % v
    if mode == 'blk':
        return '$%02X,$%02X' % (v >> 8, v & 0xFF)
    return '?'


def hw_comment(ins):
    mode, v = ins.mode, ins.val
    if mode in ('abs', 'absx', 'absy') and ins.mn not in ('JMP', 'JSR'):
        return hwname(v)
    if mode in ('long', 'longx') and ins.mn not in ('JML', 'JSL'):
        b = v >> 16
        if b <= 0x3F or 0x80 <= b <= 0xBF:
            return hwname(v & 0xFFFF)
    return None


# ---------------------------------------------------------------- hints / trace
def load_hints(path):
    h = {'entries': [], 'names': {}, 'tables': [], 'data': [], 'flags': {},
         'noreturn': set(), 'inline': {}, 'stop': set(), 'indirect': {}}
    if not path or not os.path.exists(path):
        return h
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            tok = line.split()
            kw = tok[0].lower()
            kv = {}
            pos = []
            for t in tok[1:]:
                if '=' in t:
                    k, v = t.split('=', 1)
                    kv[k.lower()] = v
                else:
                    pos.append(t)
            try:
                def fl():
                    d = {}
                    for k in ('m', 'x', 'e'):
                        if k in kv:
                            d[k] = int(kv[k])
                    return d
                if kw == 'entry':
                    a = parse_addr(pos[0])
                    d = fl()
                    h['entries'].append((a, (d.get('m'), d.get('x'), d.get('e', 0))))
                    if 'name' in kv:
                        h['names'][a] = kv['name']
                elif kw == 'name':
                    h['names'][parse_addr(pos[0])] = pos[1]
                elif kw == 'table':
                    a = parse_addr(pos[0])
                    d = fl()
                    t = {'addr': a, 'count': int(kv.get('count', '0'), 0),
                         'bank': int(kv.get('bank', '%X' % (a >> 16)), 16),
                         'type': kv.get('type', 'word'), 'call': kv.get('call', '0') == '1'}
                    if d:
                        t['st'] = (d.get('m'), d.get('x'), d.get('e', 0))
                    h['tables'].append(t)
                    if 'name' in kv:
                        h['names'][a] = kv['name']
                elif kw == 'data':
                    if '-' in pos[0]:
                        lo, hi = pos[0].split('-')
                        h['data'].append((parse_addr(lo), parse_addr(hi)))
                    else:
                        lo = parse_addr(pos[0])
                        h['data'].append((lo, lo + int(kv['len'], 0) - 1))
                elif kw == 'flags':
                    h['flags'][parse_addr(pos[0])] = fl()
                elif kw == 'noreturn':
                    h['noreturn'].add(parse_addr(pos[0]))
                elif kw == 'inline':
                    h['inline'][parse_addr(pos[0])] = int(kv['len'], 0)
                elif kw == 'indirect':
                    a = parse_addr(pos[0])
                    ih = {'call': kv.get('call', '0') == '1', 'dynamic': 'dynamic' in pos[1:]}
                    if 'table' in kv:
                        ih['table'] = parse_addr(kv['table'])
                    if 'targets' in kv:
                        ih['targets'] = [parse_addr(t) for t in kv['targets'].split(',')]
                    h['indirect'][a] = ih
                elif kw == 'stop':
                    h['stop'].add(parse_addr(pos[0]))
                else:
                    raise ValueError('unknown directive')
            except (ValueError, IndexError, KeyError) as ex:
                sys.exit('%s:%d: %s: %s' % (path, ln, ex, line))
    return h


def load_trace(paths):
    tr = defaultdict(set)
    for path in paths or []:
        with open(path, 'rb') as f:
            blob = f.read()
        try:
            text = blob.decode('ascii')
            is_text = all(c in '0123456789abcdefABCDEF \t\r\n#Ee:$' or c.isprintable()
                          for c in text[:4096])
        except UnicodeDecodeError:
            is_text = False
        if is_text:
            for line in text.splitlines():
                line = line.split('#', 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                e = 0
                if parts[0][:1] in ('E', 'e') and len(parts[0]) == 7:
                    e, parts[0] = 1, parts[0][1:]
                pc = parse_addr(parts[0])
                fl = parts[1] if len(parts) > 1 else ''
                if fl[:1] in ('E', 'e'):
                    e, fl = 1, fl[1:]
                elif len(fl) == 3:
                    e, fl = int(fl[0]), fl[1:]
                m, x = (int(fl[0]), int(fl[1])) if len(fl) == 2 else (None, None)
                if e == 1:
                    m = x = 1
                tr[pc].add((m, x, e))
        else:
            for i in range(0, len(blob) - 3, 4):
                pc = blob[i] | (blob[i + 1] << 8) | (blob[i + 2] << 16)
                f = blob[i + 3]
                e = f & 1
                m, x = (1, 1) if e else ((f >> 5) & 1, (f >> 4) & 1)
                tr[pc].add((m, x, e))
    return dict(tr)


# ---------------------------------------------------------------- output
def write_outputs(d, rom, outdir, hdr):
    os.makedirs(outdir, exist_ok=True)
    fv_by_entry = defaultdict(list)
    for fv in d.live:
        fv_by_entry[fv.entry].append(fv)

    # labels
    labels = {}   # pc -> name
    for a, n in d.names.items():
        labels[a] = n
    for fv in d.live:
        labels.setdefault(fv.entry, ('sub_%06X' if fv.kind != 'trace' else 'trc_%06X')
                          % fv.entry)
    for a in d.live_tables:
        labels.setdefault(a, 'tbl_%06X' % a)
    for ins in d.ins.values():
        for tg in ins.targets:
            labels.setdefault(tg, 'loc_%06X' % tg)
    for fv in d.live:
        for dd in fv.dispatch:
            if 'target' in dd and isinstance(dd['target'], int):
                labels.setdefault(dd['target'], 'loc_%06X' % dd['target'])

    def labeler(a):
        return labels.get(a)

    # per-offset maps
    starts = defaultdict(list)
    for ins in d.ins.values():
        starts[ins.off].append(ins)
    cover = set()
    for ins in d.ins.values():
        cover.update(range(ins.off, ins.off + ins.len))
    table_bytes = {}
    for a, t in d.live_tables.items():
        esz = 3 if t['type'] == 'long' else 2
        n = t.get('span', 0) if t['resolved'] else 0
        for k in range(esz * n):
            o = rom.off(a + k)
            if o is not None:
                table_bytes[o] = a
    lab_by_off = defaultdict(list)
    for a, n in labels.items():
        o = rom.off(a)
        if o is not None and n not in lab_by_off[o]:
            lab_by_off[o].append(n)

    ent_by_off = defaultdict(list)
    for a in sorted(fv_by_entry):
        ent_by_off[rom.off(a)].append(a)

    nbanks = rom.size >> 15
    bank_stats = []
    overlaps = []
    for b in range(nbanks):
        base = b << 15
        dbank = 0x80 | b
        lines = ['; game 3 (USA) ROM bank $%02X (file 0x%05X-0x%05X), data shown as bank $%02X'
                 % (b, base, base + 0x7FFF, dbank), '']
        ncode = ndata = ntab = 0
        prev_flags = None
        labelled = False
        covered_until = base
        dbuf = []
        dstart = None

        def flush():
            nonlocal dbuf, dstart
            if dbuf:
                for i in range(0, len(dbuf), 16):
                    chunk = dbuf[i:i + 16]
                    a = (dbank << 16) | (0x8000 | ((dstart + i) & 0x7FFF))
                    lines.append('  %06X  .db %s' % (a, ','.join('$%02X' % c for c in chunk)))
            dbuf = []
            dstart = None

        for o in range(base, base + 0x8000):
            here = starts.get(o)
            if o in lab_by_off and (here or o >= covered_until):
                flush()
                ents = ent_by_off.get(o, ())
                for a in ents:
                    cs = set()
                    for fv in fv_by_entry[a]:
                        cs |= d.callers.get(fv.key, set())
                    vs = ', '.join('m=%s x=%s e=%s -> %s' % (
                        fs(fv.st[0]), fs(fv.st[1]), fs(fv.st[2]), summ_str(fv.summary))
                        for fv in fv_by_entry[a])
                    lines.append('')
                    lines.append('; ---- %s  entry %s' % (labels[a], vs))
                    if cs:
                        cl = sorted(cs)
                        lines.append('; callers (%d): %s%s' % (
                            len(cl), ' '.join('%06X' % c for c in cl[:16]),
                            ' ...' if len(cl) > 16 else ''))
                for n in lab_by_off[o]:
                    lines.append('%s:' % n)
                labelled = True
            if here:
                flush()
                here.sort(key=lambda i: (i.pc, str((i.m, i.x, i.e))))
                groups = {}
                for ins in here:
                    txt = '%06X %s %s' % (ins.pc, ins.mn, fmt_operand(ins, labeler))
                    groups.setdefault(txt, []).append(ins)
                allst = frozenset((i.m, i.x, i.e) for i in here)
                show = allst != prev_flags or labelled or len(groups) > 1
                for k, (txt, grp) in enumerate(groups.items()):
                    ins = grp[0]
                    cm = []
                    if k > 0:
                        cm.append('mirror' if ins.pc != here[0].pc else 'variant')
                    if o < covered_until and k == 0:
                        cm.append('OVERLAP')
                        overlaps.append(ins.pc)
                    if show:
                        cm.append(' | '.join(fmt_state(st) for st in sorted(
                            {(i.m, i.x, i.e) for i in grp}, key=str)))
                    hw = hw_comment(ins)
                    if hw:
                        cm.append(hw)
                    if ins.mode in ('long', 'longx') and ins.mn not in ('JML', 'JSL') \
                            and ins.val in labels:
                        cm.append(labels[ins.val])
                    if ins.mode == 'absxi' or ins.mn in ('JMP', 'JML') and ins.mode in (
                            'absi', 'absil'):
                        if not ins.targets:
                            cm.append('UNRESOLVED')
                    if len(ins.targets) > 1:
                        cm.append('%d targets' % len(ins.targets))
                    line = '  %06X  %-12s %-5s%-22s' % (
                        ins.pc, ' '.join('%02X' % c for c in ins.raw), ins.mn,
                        ' ' + fmt_operand(ins, labeler) if ins.mode != 'imp' else '')
                    if cm:
                        line = line.rstrip().ljust(50) + ' ; ' + ' '.join(cm)
                    lines.append(line.rstrip())
                prev_flags = allst
                labelled = False
                covered_until = max(covered_until, max(i.off + i.len for i in here))
                continue
            if o < covered_until:
                continue
            if o in table_bytes:
                flush()
                a = table_bytes[o]
                t = d.live_tables[a]
                esz = 3 if t['type'] == 'long' else 2
                v = rom.data[o] | (rom.data[o + 1] << 8) | (
                    (rom.data[o + 2] << 16) if esz == 3 else 0)
                tg = v if esz == 3 else (t['bank'] << 16) | v
                ea = (dbank << 16) | (0x8000 | (o & 0x7FFF))
                lines.append('  %06X  %s %s' % (ea, '.dl' if esz == 3 else '.dw',
                                                labels.get(tg, '$%0*X' % (esz * 2, v))))
                covered_until = o + esz
                continue
            if dstart is None:
                dstart = o
            dbuf.append(rom.data[o])
            if len(dbuf) >= 16:
                flush()
        flush()
        for o in range(base, base + 0x8000):
            if o in cover:
                ncode += 1
            elif o in table_bytes:
                ntab += 1
            elif o in d.data_offs:
                ndata += 1
        bank_stats.append({'bank': b, 'code': ncode, 'table': ntab, 'data_hint': ndata,
                           'unknown': 0x8000 - ncode - ntab - ndata})
        with open(os.path.join(outdir, 'bank_%02X.asm' % b), 'w') as f:
            f.write('\n'.join(lines) + '\n')

    # map.json
    instrs = []
    for (pc, m, x, e), ins in sorted(d.ins.items(), key=lambda kv: (kv[0][0], str(kv[0][1:]))):
        instrs.append({'addr': '%06X' % pc, 'len': ins.len, 'opcode': '%02X' % ins.op,
                       'mn': ins.mn, 'm': m, 'x': x, 'e': e,
                       'targets': ['%06X' % t for t in ins.targets]})
    funcs = []
    for a in sorted(fv_by_entry):
        vs = []
        for fv in fv_by_entry[a]:
            vs.append({
                'm': fv.st[0], 'x': fv.st[1], 'e': fv.st[2], 'kind': fv.kind,
                'exit': summ_json(fv.summary),
                'blocks': ['%06X' % b for b in sorted(fv.leaders)
                           if any(k[0] == b for k in fv.ins)],
                'exits': [{'pc': '%06X' % pc, 'm': s[0], 'x': s[1], 'e': s[2],
                           'return_consumed': c} for (pc, s, c) in fv.exits],
                'callers': ['%06X' % c for c in sorted(d.callers.get(fv.key, ()))],
                'callees': sorted({'%06X' % k[0] for ks in fv.callees.values() for k in ks}),
                'incomplete': [{'pc': '%06X' % pc, 'reason': r} for (pc, r) in fv.incomplete],
                'reads_return_addr': fv.reads_return,
                'consumes_return_addr': fv.consumes_return,
                'n_instr': len(fv.ins),
                'code': [['%06X' % k[0], k[1], k[2], k[3],
                          [['%06X' % t[0], t[1], t[2], t[3]]
                           for t in sorted(fv.edges.get(k, ()), key=str)]]
                         for k in sorted(fv.ins, key=lambda k: (k[0], str(k[1:])))],
            })
        funcs.append({'entry': '%06X' % a, 'name': labels.get(a), 'variants': vs})
    tables = []
    for a in sorted(d.live_tables):
        t = d.live_tables[a]
        tables.append({'addr': '%06X' % a, 'type': t['type'], 'bank': '%02X' % t['bank'],
                       'source': t['source'], 'count': len(t['entries']),
                       'sites': sorted({'%06X' % s[0] for s in t['sites']}),
                       'call': any(s[2] for s in t['sites']) or t.get('call', False),
                       'entries': ['%06X' % e for e in t['entries']]})
    unresolved = {}
    dispatch = {}
    for fv in d.live:
        for u in fv.unresolved:
            unresolved[(u['site'], u['kind'])] = u
        for dd in fv.dispatch:
            dispatch[(dd['site'], dd['kind'])] = dd

    def hexify(dct):
        out = {}
        for k, v in dct.items():
            out[k] = ('%06X' % v) if isinstance(v, int) and not isinstance(v, bool) and \
                k in ('site', 'target', 'callee', 'table', 'ptr', 'probable_table') else v
        return out
    unresolved_l = [hexify(u) for _, u in sorted(unresolved.items())]
    dispatch_l = [hexify(u) for _, u in sorted(dispatch.items())]
    data_regions = [{'start': '%06X' % lo, 'end': '%06X' % hi} for lo, hi in d.data_ranges]
    unknown_flag = defaultdict(set)
    for fv in d.live:
        for pc, r in fv.incomplete:
            if r.startswith('unknown_'):
                unknown_flag[pc].add(r)
    mp = {'rom': hdr, 'instructions': instrs, 'functions': funcs, 'jump_tables': tables,
          'dispatch_sites': dispatch_l, 'unresolved_indirect': unresolved_l,
          'unknown_flag_stops': [{'pc': '%06X' % pc, 'reason': sorted(r)}
                                 for pc, r in sorted(unknown_flag.items())],
          'data_regions': data_regions, 'bank_stats': bank_stats,
          'overlaps': ['%06X' % o for o in sorted(set(overlaps))],
          'trace_conflicts': [{'pc': '%06X' % c['pc'], 'static': c['static'],
                               'trace': c['trace']} for c in d.trace_conflicts]}
    with open(os.path.join(outdir, 'map.json'), 'w') as f:
        json.dump(mp, f, indent=1)
    return mp, labels


def fmt_state(st):
    m, x, e = st
    return 'm=%s x=%s%s' % (fs(m), fs(x), ' e=1' if e == 1 else (' e=?' if e is None else ''))


def summ_str(s):
    if s is None:
        return 'pending'
    if s[0] == 'ret':
        return 'ret m=%s x=%s' % (fs(s[1]), fs(s[2]))
    return s[0]


def summ_json(s):
    if s is None:
        return None
    return {'kind': s[0], 'm': s[1], 'x': s[2], 'e': s[3]}


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--rom', default=DEF_ROM)
    ap.add_argument('--hints', default=DEF_HINTS)
    ap.add_argument('--trace', action='append', default=[])
    ap.add_argument('--out', default=DEF_OUT)
    ap.add_argument('-q', '--quiet', action='store_true')
    args = ap.parse_args()
    sys.setrecursionlimit(20000)

    rom = Rom(args.rom)
    hints = load_hints(args.hints)
    traces = load_trace(args.trace)
    hbase = 0x7FC0
    title = rom.data[hbase:hbase + 21].decode('ascii', 'replace').rstrip()
    hdr = {'title': title, 'map_mode': '%02X' % rom.data[hbase + 0x15],
           'cart_type': '%02X' % rom.data[hbase + 0x16], 'size': rom.size}
    vec = {}
    for name, a in (('n_cop', 0xFFE4), ('n_brk', 0xFFE6), ('n_abort', 0xFFE8),
                    ('n_nmi', 0xFFEA), ('n_irq', 0xFFEE), ('e_cop', 0xFFF4),
                    ('e_abort', 0xFFF8), ('e_nmi', 0xFFFA), ('e_reset', 0xFFFC),
                    ('e_irqbrk', 0xFFFE)):
        vec[name] = rom.word(a)
    hdr['vectors'] = {k: '%04X' % v for k, v in vec.items()}

    d = Disasm(rom, hints, traces)
    d.add_root(vec['e_reset'], (1, 1, 1), 'reset')
    for k in ('n_nmi', 'n_irq', 'n_cop', 'n_brk'):
        if vec[k] >= 0x8000:
            d.add_root(vec[k], (None, None, 0), 'interrupt')
    for k in ('e_nmi', 'e_irqbrk', 'e_cop'):
        if vec[k] >= 0x8000:
            d.add_root(vec[k], (1, 1, 1), 'interrupt')
    d.names.setdefault(vec['e_reset'], 'Reset')
    d.names.setdefault(vec['n_nmi'], 'NMI')
    for a, st in hints['entries']:
        d.add_root(a, st, 'hint')
    d.run()
    mp, labels = write_outputs(d, rom, args.out, hdr)

    if args.quiet:
        return
    print('ROM %s map=%s type=%s vectors %s' % (title, hdr['map_mode'], hdr['cart_type'],
                                                 ' '.join('%s=%s' % kv for kv in
                                                          hdr['vectors'].items())))
    nvar = len(d.live)
    nfunc = len({fv.entry for fv in d.live})
    multi = sum(1 for f in mp['functions'] if len(f['variants']) > 1)
    print('functions: %d entries, %d variants (%d entries with >1 flag variant)' %
          (nfunc, nvar, multi))
    print('instructions: %d (unique PCs %d)' % (len(d.ins), len({k[0] for k in d.ins})))
    print('jump tables: %d, dispatch sites: %d' % (len(mp['jump_tables']),
                                                   len(mp['dispatch_sites'])))
    tc = tt = tu = td = 0
    print('bank  code   table  data   unknown')
    for s in mp['bank_stats']:
        print('  %02X  %5d  %5d  %5d  %5d' % (s['bank'], s['code'], s['table'],
                                              s['data_hint'], s['unknown']))
        tc += s['code']
        tt += s['table']
        td += s['data_hint']
        tu += s['unknown']
    tot = rom.size
    print('total code %d (%.1f%%) table %d data %d unknown %d (%.1f%%)' %
          (tc, 100.0 * tc / tot, tt, td, tu, 100.0 * tu / tot))
    print('unknown-flag stops: %d, overlaps: %d, trace conflicts: %d' %
          (len(mp['unknown_flag_stops']), len(mp['overlaps']), len(mp['trace_conflicts'])))
    print('unresolved indirect: %d' % len(mp['unresolved_indirect']))
    for u in mp['unresolved_indirect']:
        print('  ' + ' '.join('%s=%s' % kv for kv in u.items()))


if __name__ == '__main__':
    main()
