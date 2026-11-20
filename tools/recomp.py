#!/usr/bin/env python3
"""65816 -> C static recompiler for the game 1 games (USA).

Usage:
    recomp.py [--map build/disasm/map.json] [--rom ROM] [--hints HINTS] [--out src/game/gen/g3]
              [--prefix x3]

--prefix names everything generated for one game, so several games link
into one binary: functions PREFIX_BBAAAA..., table rt_dispatch_PREFIX.

Input: map.json from tools/disasm.py (functions[].variants[].code lists the
decoded instructions of each variant with their arrival states and successor
states). Output in --out:
    funcs.h        prototypes
    code_N.c       N = 0..7: one C function per native-mode function variant,
                   split by instruction count (fixed file set for the build)
    dispatch.c     sorted table (pc24, m, x) -> (function, entry index)

Overrides (--overrides, default tools/overrides.txt): every label at a listed
PC starts with a call to the named hand-written C function (src/game/). It
returns false to run the original instruction, or true after moving the
machine to a new PB:PC, where execution continues (RT_CONTINUE).

Generated function: void PREFIX_BBAAAA[_Name][_mxMX](Snes *s, uint32_t ret, uint32_t ent)
    ret  expected 24-bit return continuation, or RT_ANY when entered from the
         hook; RTS/RTL/RTI compare the popped address against it.
    ent  entry index; 0 is the function entry, others are instruction labels
         registered in the dispatch table.

Each instruction is the opcode fetch plus cpu_op_XX from src/snes/cpu_ops.h
(the interpreter's own semantics), followed by the interrupt/frame poll.
Control flow is resolved statically where the successor PC and flag state are
known; other transfers compare PB:PC and P.M/P.X at run time and fall back to
rt_dyn (table lookup) or rt_exit (resume in the interpreter).
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import disasm  # noqa: E402

DEF_MAP = os.path.join(ROOT, 'build', 'disasm', 'map.json')
DEF_OUT = os.path.join(ROOT, 'src', 'game', 'gen', 'g3')
DEF_OVERRIDES = os.path.join(ROOT, 'tools', 'g3', 'overrides.txt')

COND_BRANCH = {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0}
EXIT_OPS = {0x00, 0x02, 0xCB, 0xDB, 0xFB}      # BRK COP WAI STP XCE
NFILES = 8                  # fixed; CMakeLists.txt lists code_0..7.c


def h24(s):
    return int(s, 16)


def st_str(m, x):
    return '%s%s' % ('u' if m is None else m, 'u' if x is None else x)


def cident(s):
    return ''.join(ch if ch.isalnum() or ch == '_' else '_' for ch in s)


class Variant:
    def __init__(self, entry, v, name, multi, prefix):
        self.entry = entry
        self.m, self.x, self.e = v['m'], v['x'], v['e']
        self.st = (self.m, self.x)
        self.kind = v['kind']
        suffix = ''
        if name:
            suffix += '_' + cident(name)
        if multi or not name:
            suffix += '_mx' + st_str(self.m, self.x)
        self.fname = '%s_%06X%s' % (prefix, entry, suffix)
        self.name = name
        self.code = {}          # (pc, m, x) -> successor list [(pc, m, x, e)]
        for pc, m, x, e, succ in v['code']:
            if e != 0:
                continue
            self.code[(h24(pc), m, x)] = [(h24(s[0]), s[1], s[2], s[3]) for s in succ]
        self.labels_at = {}
        for (pc, m, x) in self.code:
            self.labels_at.setdefault(pc, []).append((m, x))
        self.entries = [None]   # ent index -> code key; 0 is the function entry


def label(k):
    return 'L_%06X_%s' % (k[0], st_str(k[1], k[2]))


def concrete_combos(m, x):
    ms = [0, 1] if m is None else [m]
    xs = [0, 1] if x is None else [x]
    return [(a, b) for a in ms for b in xs]


def flag_test(m, x):
    """C condition that P matches the concrete bits of (m, x); None when unconstrained."""
    mask = (0x20 if m is not None else 0) | (0x10 if x is not None else 0)
    if not mask:
        return None
    val = (0x20 if m == 1 else 0) | (0x10 if x == 1 else 0)
    return '(c->p & 0x%02X) == 0x%02X' % (mask, val)


def load_overrides(path):
    """'BB:AAAA name' per line, '#' comments -> {pc24: name}."""
    ovr = {}
    if not path or not os.path.exists(path):
        return ovr
    with open(path) as f:
        for ln, line in enumerate(f, 1):
            line = line.split('#', 1)[0].split()
            if not line:
                continue
            if len(line) != 2:
                raise SystemExit('%s:%d: expected "BB:AAAA name"' % (path, ln))
            pc = int(line[0].replace(':', ''), 16)
            if pc in ovr or not line[1].isidentifier():
                raise SystemExit('%s:%d: duplicate PC or bad name' % (path, ln))
            ovr[pc] = line[1]
    return ovr


class Gen:
    def __init__(self, mp, rom, names, overrides=None, prefix='x3'):
        self.prefix = prefix
        self.rom = rom
        self.names = names
        self.overrides = overrides or {}
        self.itargets = {}
        for i in mp['instructions']:
            self.itargets[(h24(i['addr']), i['m'], i['x'], i['e'])] = [h24(t) for t in i['targets']]
        self.variants = []
        for f in mp['functions']:
            entry = h24(f['entry'])
            vs = [v for v in f['variants'] if v['e'] == 0]
            for v in vs:
                self.variants.append(Variant(entry, v, names.get(entry), len(vs) > 1, prefix))
        self.variants.sort(key=lambda v: (v.entry, str(v.st)))
        self.by_key = {(v.entry, v.m, v.x): v for v in self.variants}
        present = {k[0] for v in self.variants for k in v.code}
        for pc in self.overrides:
            if pc not in present:
                raise SystemExit('override %06X %s: not a recompiled instruction' % (pc, self.overrides[pc]))
        self.assign_dispatch()

    def assign_dispatch(self):
        """Owner of each concrete (pc, m, x): function entry first, then exact
        instruction state, then an instruction whose state has unknown bits."""
        best = {}
        for vi, v in enumerate(self.variants):
            for k in v.code:
                pc, m, x = k
                exact = m is not None and x is not None
                for cm, cx in concrete_combos(m, x):
                    if pc == v.entry and (m, x) == v.st:
                        pri = 0
                    elif exact:
                        pri = 1
                    else:
                        pri = 2
                    key = (pc, cm, cx)
                    cand = (pri, vi, k)
                    if key not in best or cand < best[key]:
                        best[key] = cand
        self.dispatch = []
        ent_of = {}
        for key in sorted(best):
            pri, vi, k = best[key]
            v = self.variants[vi]
            if pri == 0 and k == (v.entry, v.m, v.x):
                ent = 0
            else:
                ent = ent_of.get((vi, k))
                if ent is None:
                    ent = len(v.entries)
                    v.entries.append(k)
                    ent_of[(vi, k)] = ent
            self.dispatch.append((key, v.fname, ent))

    # ---- emission helpers
    def goto(self, v, pc, st, nextlab):
        """Transfer to pc with computed flag state st (None bits: unknown)."""
        m, x = st
        out = []
        if m is not None and x is not None and (pc, m, x) in v.code:
            lab = label((pc, m, x))
            if lab != nextlab:
                out.append('goto %s;' % lab)
            return out
        cands = []
        for (lm, lx) in sorted(v.labels_at.get(pc, []), key=str):
            # A label is reachable when its concrete bits do not contradict st.
            if (m is not None and lm is not None and lm != m) or \
                    (x is not None and lx is not None and lx != x):
                continue
            cands.append((lm, lx))
        for lm, lx in cands:
            t = flag_test(lm if m is None else None, lx if x is None else None)
            lab = label((pc, lm, lx))
            if t is None:
                out.append('goto %s;' % lab)
                return out
            out.append('if (%s) goto %s;' % (t, lab))
        out.append('rt_exit();')
        return out

    def fn_for(self, tgt, st):
        return self.by_key.get((tgt, st[0], st[1]))

    def emit_variant(self, v):
        mx = st_str(v.m, v.x)
        L = ['//%06X %s m=%s x=%s%s' % (v.entry, v.name or '', mx[0], mx[1],
                                       ' (%s)' % v.kind if v.kind != 'sub' else '')]
        L.append('void %s(Snes *s, uint32_t ret, uint32_t ent) {' % v.fname)
        L.append('  Cpu *const c = &s->cpu;')
        L.append('  switch (ent) {')
        for i, k in enumerate(v.entries):
            if i:
                L.append('  case %d: goto %s;' % (i, label(k)))
        L.append('  default:')
        L.extend('    ' + g for g in self.goto(v, v.entry, v.st, None))
        L.append('  }')
        keys = sorted(v.code, key=lambda k: (k[0], str(k[1:])))
        for i, k in enumerate(keys):
            nextlab = label(keys[i + 1]) if i + 1 < len(keys) else None
            L.extend(self.emit_insn(v, k, nextlab))
        L.append('}')
        return L

    def emit_insn(self, v, k, nextlab):
        pc, m, x = k
        ins = disasm.decode(self.rom, pc, m, x)
        if isinstance(ins, str):
            raise SystemExit('decode failed at %06X: %s' % (pc, ins))
        ins.targets = self.itargets.get((pc, m, x, 0), [])
        opnd = disasm.fmt_operand(ins, self.names.get)
        asm = ('%s %s' % (ins.mn, opnd)).strip()
        op = ins.op
        bank = pc & 0xFF0000
        nxt = bank | ((pc + ins.len) & 0xFFFF)
        st = (m, x)
        succ = v.code[k]
        out = ['%s:  //%06X  %s' % (label(k), pc, asm)]
        body = []
        if pc in self.overrides:
            body.append('if (rt_overrides_on && %s(s)) RT_CONTINUE();' % self.overrides[pc])
        if op in COND_BRANCH or op in (0x80, 0x82):
            off = ins.val
            if op == 0x82:
                tgt = bank | ((pc + 3 + (off - 0x10000 if off & 0x8000 else off)) & 0xFFFF)
            else:
                tgt = bank | ((pc + 2 + (off - 0x100 if off & 0x80 else off)) & 0xFFFF)
            body.append('OP(%02X);' % op)
            if op in COND_BRANCH and tgt != nxt:
                g = self.goto(v, tgt, st, None)
                if len(g) == 1:
                    body.append('if (c->pc == 0x%04X) %s' % (tgt & 0xFFFF, g[0]))
                else:
                    body.append('if (c->pc == 0x%04X) {' % (tgt & 0xFFFF))
                    body.extend('  ' + s for s in g)
                    body.append('}')
                body.extend(self.goto(v, nxt, st, nextlab))
            else:
                body.extend(self.goto(v, tgt, st, nextlab))
        elif op == 0x4C or op == 0x5C:
            tgt = (bank | ins.val) if op == 0x4C else ins.val
            body.append('OP(%02X);' % op)
            body.extend(self.goto(v, tgt, st, nextlab))
        elif op in (0x20, 0x22):
            tgt = (bank | ins.val) if op == 0x20 else ins.val
            body.append('OP(%02X);' % op)
            f = self.fn_for(tgt, st)
            if f is not None:
                body.append('%s(s, 0x%06X, 0);' % (f.fname, nxt))
            else:
                body.append('rt_dyn(s, 0x%06X);' % nxt)
            body.extend(self.goto(v, nxt, (None, None), nextlab))
        elif op == 0xFC:
            body.append('OP(FC);')
            body.append('switch (c->pc) {')
            for t in sorted(set(ins.targets)):
                f = self.fn_for(t, st)
                if f is not None and (t & 0xFF0000) == bank:
                    body.append('case 0x%04X: %s(s, 0x%06X, 0); break;' % (t & 0xFFFF, f.fname, nxt))
            body.append('default: rt_dyn(s, 0x%06X); break;' % nxt)
            body.append('}')
            body.extend(self.goto(v, nxt, (None, None), nextlab))
        elif op in (0x6C, 0x7C, 0xDC):
            body.append('OP(%02X);' % op)
            tg = sorted(set(ins.targets) | {s_[0] for s_ in succ})
            cases = []
            for t in tg:
                if st[0] is not None and st[1] is not None and (t, st[0], st[1]) in v.code:
                    cases.append('case 0x%06X: goto %s;' % (t, label((t, st[0], st[1]))))
                    continue
                f = self.fn_for(t, st)
                if f is not None:
                    cases.append('case 0x%06X: %s(s, ret, 0); return;' % (t, f.fname))
            if cases:
                body.append('switch (PC24) {')
                body.extend(cases)
                body.append('default: rt_dyn(s, ret); return;')
                body.append('}')
            else:
                body.append('rt_dyn(s, ret);')
                body.append('return;')
        elif op in (0x60, 0x6B, 0x40):
            body.append('OP(%02X);' % op)
            if op != 0x40:
                for t in sorted({s_[0] for s_ in succ}):
                    if st[0] is not None and st[1] is not None and (t, st[0], st[1]) in v.code:
                        body.append('if (PC24 == 0x%06X) goto %s;' % (t, label((t, st[0], st[1]))))
            body.append('RET();')
        elif op == 0x28:
            body.append('OP(28);')
            body.extend(self.goto(v, nxt, (None, None), nextlab))
        elif op in (0xC2, 0xE2):
            body.append('OP(%02X);' % op)
            nm, nx = m, x
            if op == 0xC2:
                if ins.val & 0x20:
                    nm = 0
                if ins.val & 0x10:
                    nx = 0
            else:
                if ins.val & 0x20:
                    nm = 1
                if ins.val & 0x10:
                    nx = 1
            body.extend(self.goto(v, nxt, (nm, nx), nextlab))
        elif op in EXIT_OPS:
            body.append('OP(%02X);' % op)
            body.append('rt_exit();')
        elif op in (0x44, 0x54):
            body.append('OP(%02X);' % op)
            body.append('if (c->pc == 0x%04X) goto %s;' % (pc & 0xFFFF, label(k)))
            body.extend(self.goto(v, nxt, st, nextlab))
        else:
            body.append('OP(%02X);' % op)
            body.extend(self.goto(v, nxt, st, nextlab))
        out.extend('  ' + b for b in body)
        return out

    def write(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        for fn in os.listdir(outdir):
            if fn.startswith('code_') and fn.endswith('.c') and \
                    fn not in ['code_%d.c' % i for i in range(NFILES)]:
                os.remove(os.path.join(outdir, fn))
        guard = 'GAME_GEN_%s_FUNCS_H' % self.prefix.upper()
        hdr = ['//Generated by tools/recomp.py. Do not edit.',
               '#ifndef ' + guard, '#define ' + guard, '',
               '#include <stdbool.h>', '#include "snes/snes.h"', '']
        for v in self.variants:
            hdr.append('void %s(Snes *s, uint32_t ret, uint32_t ent);' % v.fname)
        if self.overrides:
            hdr += ['', '//Overrides (tools/overrides.txt), implemented in src/game/.']
            for pc in sorted(self.overrides):
                hdr.append('bool %s(Snes *s);  //%06X' % (self.overrides[pc], pc))
        hdr += ['', '#endif', '']
        self.write_file(outdir, 'funcs.h', hdr)

        total = sum(len(v.code) for v in self.variants)
        files = [[] for _ in range(NFILES)]
        n = 0
        for v in self.variants:
            files[min(NFILES - 1, n * NFILES // max(total, 1))].append(v)
            n += len(v.code)
        for i, vs in enumerate(files):
            L = ['//Generated by tools/recomp.py from build/disasm/map.json. Do not edit.',
                 '#include "game/recomp_gen.h"', '#include "funcs.h"', '']
            for v in vs:
                L.extend(self.emit_variant(v))
                L.append('')
            self.write_file(outdir, 'code_%d.c' % i, L)

        L = ['//Generated by tools/recomp.py. Do not edit.',
             '//key = pc24 << 2 | m << 1 | x, ascending.',
             '#include "game/recomp_rt.h"', '#include "funcs.h"', '',
             'const RtEntry rt_dispatch_%s[] = {' % self.prefix]
        for (pc, m, x), fname, ent in self.dispatch:
            L.append('  {0x%08Xu, %s, %d},' % (pc << 2 | m << 1 | x, fname, ent))
        L += ['};', '', 'const uint32_t rt_dispatch_%s_n = %d;' % (self.prefix, len(self.dispatch)), '']
        self.write_file(outdir, 'dispatch.c', L)
        return len(files)

    @staticmethod
    def write_file(outdir, name, lines):
        path = os.path.join(outdir, name)
        text = '\n'.join(lines) + '\n'
        with open(path, 'w') as f:
            f.write(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--map', default=DEF_MAP)
    ap.add_argument('--rom', default=disasm.DEF_ROM)
    ap.add_argument('--hints', default=disasm.DEF_HINTS)
    ap.add_argument('--out', default=DEF_OUT)
    ap.add_argument('--overrides', default=DEF_OVERRIDES)
    ap.add_argument('--prefix', default='x3')
    a = ap.parse_args()
    with open(a.map) as f:
        mp = json.load(f)
    rom = disasm.Rom(a.rom)
    hints = disasm.load_hints(a.hints)
    g = Gen(mp, rom, hints['names'], load_overrides(a.overrides), a.prefix)
    nfiles = g.write(a.out)
    ninsn = sum(len(v.code) for v in g.variants)
    print('%d functions, %d instructions, %d dispatch entries, %d overrides, %d code files -> %s' %
          (len(g.variants), ninsn, len(g.dispatch), len(g.overrides), nfiles, a.out))


if __name__ == '__main__':
    main()
