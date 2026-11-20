"""Research harness over build/libsnespy.so (tools/snespy.c).

    import sys; sys.path.insert(0, 'tools'); from sp import *
    m = Machine('rom/g1.sfc')
    m.run(600); m.press(START); m.shot('/tmp/a.png')
    m.w8(0x0BAD), m.w16(0x0BAD), m.wram[...]

Pads are SNES bits (B Y SELECT START UP DOWN LEFT RIGHT A X L R).
"""
import ctypes
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIB = ctypes.CDLL(os.environ.get('SNESPY_LIB') or os.path.join(ROOT, 'build', 'libsnespy.so'))

R, L, X, A = 0x0010, 0x0020, 0x0040, 0x0080
RIGHT, LEFT, DOWN, UP = 0x0100, 0x0200, 0x0400, 0x0800
START, SELECT, Y, B = 0x1000, 0x2000, 0x4000, 0x8000


class Hit(ctypes.Structure):
    _fields_ = [('pc', ctypes.c_uint32), ('a', ctypes.c_uint16), ('x', ctypes.c_uint16),
                ('y', ctypes.c_uint16), ('s', ctypes.c_uint16), ('d', ctypes.c_uint16),
                ('db', ctypes.c_uint8), ('p', ctypes.c_uint8), ('addr', ctypes.c_uint32),
                ('val', ctypes.c_uint8), ('frame', ctypes.c_uint64)]

    def __repr__(self):
        return 'Hit(pc=%06X a=%04X x=%04X y=%04X d=%04X db=%02X p=%02X addr=%05X val=%02X f=%d)' % (
            self.pc, self.a, self.x, self.y, self.d, self.db, self.p, self.addr, self.val, self.frame)


_P = ctypes.c_void_p
for name, res, args in [
    ('sp_new', _P, [ctypes.c_char_p, ctypes.c_int]), ('sp_clone', _P, [_P]), ('sp_copy', None, [_P, _P]),
    ('sp_free', None, [_P]), ('sp_run', None, [_P, ctypes.c_int, ctypes.c_int]),
    ('sp_set_pad2', None, [_P, ctypes.c_int]),
    ('sp_wram', ctypes.POINTER(ctypes.c_uint8 * 0x20000), [_P]),
    ('sp_vram', ctypes.POINTER(ctypes.c_uint8 * 0x10000), [_P]),
    ('sp_oam', ctypes.POINTER(ctypes.c_uint8 * 544), [_P]),
    ('sp_cgram', ctypes.POINTER(ctypes.c_uint16 * 256), [_P]),
    ('sp_cx4ram', ctypes.POINTER(ctypes.c_uint8 * 0xC00), [_P]),
    ('sp_aram', ctypes.POINTER(ctypes.c_uint8 * 0x10000), [_P]),
    ('sp_apu_in', ctypes.POINTER(ctypes.c_uint8 * 4), [_P]),
    ('sp_apu_out', ctypes.POINTER(ctypes.c_uint8 * 4), [_P]),
    ('sp_rom', ctypes.POINTER(ctypes.c_uint8), [_P]), ('sp_rom_size', ctypes.c_uint32, [_P]),
    ('sp_frame_count', ctypes.c_uint64, [_P]),
    ('sp_cpu', ctypes.c_int, [_P, ctypes.POINTER(ctypes.c_uint32)]),
    ('sp_break', None, [_P, ctypes.c_uint32]), ('sp_clear_breaks', None, [_P]),
    ('sp_watch', None, [_P, ctypes.c_uint32, ctypes.c_uint32]),
    ('sp_watch_io', None, [_P, ctypes.c_uint32, ctypes.c_uint32]),
    ('sp_hits', ctypes.c_int, [_P, ctypes.POINTER(Hit), ctypes.c_int]),
    ('sp_coverage', None, [_P, ctypes.c_void_p]),
    ('sp_wide', None, [_P, ctypes.c_int]),
    ('sp_shot', None, [_P, ctypes.c_char_p]), ('sp_save', ctypes.c_int, [_P, ctypes.c_char_p]),
    ('sp_load', ctypes.c_int, [_P, ctypes.c_char_p]),
    ('sp_audio', ctypes.c_int, [_P, ctypes.POINTER(ctypes.c_int16), ctypes.c_int]),
    ('sp_call', ctypes.c_int, [_P, ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint16,
                               ctypes.c_uint16, ctypes.c_uint8, ctypes.c_int, ctypes.c_int]),
]:
    fn = getattr(LIB, name)
    fn.restype = res
    fn.argtypes = args


def lorom(a):
    return ((a >> 16 & 0x7F) << 15) | (a & 0x7FFF)


class Machine:
    def __init__(self, rom=None, recomp=True, _h=None):
        self.h = _h if _h else LIB.sp_new(rom.encode(), 1 if recomp else 0)
        if not self.h:
            raise IOError(rom)
        self.wram = LIB.sp_wram(self.h).contents
        self.vram = LIB.sp_vram(self.h).contents
        self.oam = LIB.sp_oam(self.h).contents
        self.cgram = LIB.sp_cgram(self.h).contents
        self.rom_size = LIB.sp_rom_size(self.h)
        self.rom = ctypes.cast(LIB.sp_rom(self.h), ctypes.POINTER(ctypes.c_uint8 * self.rom_size)).contents

    def clone(self):
        return Machine(_h=LIB.sp_clone(self.h))

    def copy_from(self, other):
        LIB.sp_copy(self.h, other.h)

    def run(self, frames=1, pad=0):
        LIB.sp_run(self.h, pad, frames)

    def press(self, pad, hold=2, after=0):
        self.run(hold, pad)
        if after:
            self.run(after)

    @property
    def frame(self):
        return LIB.sp_frame_count(self.h)

    def w8(self, a):
        return self.wram[a & 0x1FFFF]

    def w16(self, a):
        return self.wram[a & 0x1FFFF] | self.wram[(a + 1) & 0x1FFFF] << 8

    def s16(self, a):
        v = self.w16(a)
        return v - 0x10000 if v & 0x8000 else v

    def set8(self, a, v):
        self.wram[a & 0x1FFFF] = v & 0xFF

    def set16(self, a, v):
        self.wram[a & 0x1FFFF] = v & 0xFF
        self.wram[(a + 1) & 0x1FFFF] = (v >> 8) & 0xFF

    def r8(self, a24):
        return self.rom[lorom(a24) % self.rom_size]

    def r16(self, a24):
        return self.r8(a24) | self.r8(a24 + 1) << 8

    def r24(self, a24):
        return self.r16(a24) | self.r8(a24 + 2) << 16

    def snap(self):
        return bytes(self.wram)

    def cpu(self):
        out = (ctypes.c_uint32 * 8)()
        LIB.sp_cpu(self.h, out)
        return dict(zip(['pc', 'a', 'x', 'y', 's', 'd', 'db', 'p'], list(out)))

    def brk(self, pc):
        LIB.sp_break(self.h, pc)

    def watch(self, lo, hi=None):
        LIB.sp_watch(self.h, lo, (hi if hi is not None else lo + 1))

    def watch_io(self, lo, hi=None):
        """Records CPU writes to I/O registers [lo, hi) (e.g. 0x2140-0x2144); Hit.addr = 0x100000 | reg."""
        LIB.sp_watch_io(self.h, lo, (hi if hi is not None else lo + 1))

    def clear(self):
        LIB.sp_clear_breaks(self.h)

    def hits(self):
        buf = (Hit * 4096)()
        n = LIB.sp_hits(self.h, buf, 4096)
        return list(buf[:n])

    def coverage_on(self):
        self._cov = (ctypes.c_uint8 * (1 << 24))()
        LIB.sp_coverage(self.h, ctypes.cast(self._cov, ctypes.c_void_p))

    def coverage(self):
        return self._cov

    def wide(self, w):
        """Widescreen game logic for w extra columns per side (0 = off)."""
        LIB.sp_wide(self.h, w)

    def shot(self, path, scale=2):
        ppm = path.rsplit('.', 1)[0] + '.ppm'
        LIB.sp_shot(self.h, ppm.encode())
        if path.endswith('.png'):
            subprocess.run(['convert', ppm, '-scale', '%d%%' % (scale * 100), path], check=False)
            os.remove(ppm)

    def save(self, path):
        return LIB.sp_save(self.h, path.encode())

    def load(self, path):
        return LIB.sp_load(self.h, path.encode())

    def call(self, pc24, a=0, x=0, y=0, d=0, db=0, m16=False, x16=False):
        return LIB.sp_call(self.h, pc24, a, x, y, d, db, int(m16), int(x16))

    def audio(self, maxf=8192):
        buf = (ctypes.c_int16 * (maxf * 2))()
        n = LIB.sp_audio(self.h, buf, maxf)
        return buf[:n * 2]

    def drain_audio(self):
        while self.audio():
            pass


def write_coverage(path, cov):
    """Merges a coverage buffer (per PC: bit e*4 + m*2 + x) into a tools/gN/coverage.txt file."""
    allc = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            e = 0
            if line[0] == 'E':
                e, line = 1, line[1:]
            pc, mx = line.split()
            allc[int(pc, 16)] = allc.get(int(pc, 16), 0) | 1 << (e * 4 + int(mx[0]) * 2 + int(mx[1]))
    raw = bytes(cov)
    for pc in range(len(raw)):
        if raw[pc]:
            allc[pc] = allc.get(pc, 0) | raw[pc]
    with open(path, 'w') as f:
        for pc in sorted(allc):
            for st in range(8):
                if allc[pc] >> st & 1:
                    f.write('%s%06X %d%d\n' % ('E' if st & 4 else '', pc, st >> 1 & 1, st & 1))


def diff(a, b, lo=0, hi=0x20000):
    return [i for i in range(lo, hi) if a[i] != b[i]]
