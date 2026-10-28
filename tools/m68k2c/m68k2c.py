#!/usr/bin/env python3
"""m68k2c - translate the ToeJam & Earl 68000 code into C.

Usage:
    m68k2c.py --source EntireSource.X68 --rom game.bin --out src/generated
    m68k2c.py --listing Listing.txt     --rom game.bin --out src/generated

The disassembly source (mapped onto the ROM by srcmap.py) or a vasm listing
(-L) provides label names, the original source text (kept as comments) and the
code/data layout. Instructions themselves are decoded from the ROM bytes.

Model
-----
* Every routine that is called (JSR/BSR target, interrupt vector, or an
  address the game stores as a pointer) becomes one C function.
* Everything reachable from a function through branches is translated into
  that function, using goto for branches. Shared tails get duplicated.
* CPU registers live in the global R (src/core/m68k_rt.h). Condition flags
  are locals in each function so the C compiler can drop unused ones.
* JSR pushes the real return address onto the emulated stack, so stack-passed
  arguments line up exactly as the original code expects.
"""

import argparse
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode68k as d68  # noqa: E402
import listing as lst    # noqa: E402
import timing68k         # noqa: E402
import srcmap            # noqa: E402

ROM_END = 0x100000

# Labels inside larger routines that the runtime needs to call directly
# (InGameLoop: the main game loop, entered after loading a save state).
EXTRA_ENTRY_LABELS = ['InGameLoop']
MASK = {'b': 0xFF, 'w': 0xFFFF, 'l': 0xFFFFFFFF}
NBYTES = {'b': 1, 'w': 2, 'l': 4}

COND_C = {
    't': '1', 'f': '0',
    'hi': '(!F.c && !F.z)', 'ls': '(F.c || F.z)',
    'cc': '(!F.c)', 'cs': '(F.c)', 'ne': '(!F.z)', 'eq': '(F.z)',
    'vc': '(!F.v)', 'vs': '(F.v)', 'pl': '(!F.n)', 'mi': '(F.n)',
    'ge': '(F.n == F.v)', 'lt': '(F.n != F.v)',
    'gt': '(!F.z && F.n == F.v)', 'le': '(F.z || F.n != F.v)',
}


def cident(name):
    return re.sub(r'[^A-Za-z0-9_]', '_', name)


class TranslateError(Exception):
    pass


# --------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------
class Program:
    def __init__(self, rom, listing):
        self.rom = rom
        self.L = listing
        self.dec = d68.Decoder(rom)
        self.cache = {}
        self.entries = {}        # addr -> reason
        self.funcs = {}          # addr -> Function
        self.warnings = []

    def ins(self, a):
        if a not in self.cache:
            self.cache[a] = self.dec.decode(a)
        return self.cache[a]

    def is_code_addr(self, a):
        return a in self.L.code

    def name_for(self, a):
        names = self.L.labels.get(a)
        if names:
            # prefer a meaningful global name over LOC_xxx
            good = [n for n in names if not n.upper().startswith('LOC_') and '.' not in n]
            if good:
                return cident(good[0])
            good = [n for n in names if not n.upper().startswith('LOC_')]
            if good:
                return cident(good[0])
        return None

    def fname(self, a):
        n = self.name_for(a)
        return f'f_{a:06X}_{n}' if n else f'f_{a:06X}'

    # ---- entry discovery -------------------------------------------------
    def label_addr(self, name):
        for a, names in self.L.labels.items():
            if name in names:
                return a
        return None

    def seed_entries(self):
        for name in EXTRA_ENTRY_LABELS:
            a = self.label_addr(name)
            if a is not None:
                self.entries.setdefault(a, f'runtime entry point {name}')
        vec = [int.from_bytes(self.rom[i * 4:i * 4 + 4], 'big') for i in range(64)]
        for i, v in enumerate(vec[1:], 1):
            if v < ROM_END and v in self.L.code:
                self.entries.setdefault(v, f'vector {i}')
        # 32-bit values in data that equal the address of a labelled code line
        # (or of a labelled spot inside a data block - code the disassembly hasn't
        # separated out yet; those are tried as code and dropped if they don't decode)
        labelled_code = {a for a in self.L.labels if a in self.L.code}
        labelled_data = {a for a in self.L.labels if a not in self.L.code}
        rom = self.rom
        for a in range(0, ROM_END - 3, 2):
            if a in self.L.code:
                continue
            v = (rom[a] << 24) | (rom[a + 1] << 16) | (rom[a + 2] << 8) | rom[a + 3]
            if v < 0x200 or v >= ROM_END or v & 1:
                continue
            if v in labelled_code:
                self.entries.setdefault(v, f'pointer in data at {a:06X}')
            elif v in labelled_data and v not in self.entries:
                self.entries[v] = f'pointer in data at {a:06X} (code hidden in data?)'
                self.speculative.add(v)
            elif v not in self.entries and v < 0x80000 and self.looks_like_function(v):
                # unlabelled pointer to what starts like a compiled C function
                self.entries[v] = f'pointer in data at {a:06X} (function prologue)'
                self.speculative.add(v)

    def looks_like_function(self, v):
        try:
            i = self.ins(v)
        except d68.DecodeError:
            return False
        if i.op == 'link':
            return True
        if i.op == 'movem' and i.ops[1].mode == 'predec' and i.ops[1].reg == 7:
            return True
        return (i.op == 'move' and i.size == 'l' and i.ops[0].mode == 'dn'
                and i.ops[1].mode == 'predec' and i.ops[1].reg == 7)

    def walk(self, entry):
        """Return (instrs dict, jump tables) reachable from entry via branches."""
        seen = {}
        tables = {}
        work = [entry]
        while work:
            a = work.pop()
            while a not in seen:
                if a >= ROM_END or a & 1:
                    raise TranslateError(f'flow to bad address {a:06X} in {entry:06X}')
                try:
                    i = self.ins(a)
                except d68.DecodeError as e:
                    raise TranslateError(f'decode error at {a:06X} (from {entry:06X}): {e}')
                seen[a] = i
                op = i.op
                nxt = a + i.length
                if op in ('jsr', 'bsr'):
                    tgt = self.call_target(i)
                    if tgt is not None:
                        self.add_entry(tgt, f'called from {a:06X}')
                    a = nxt
                    continue
                if op == 'bra':
                    work.append(i.target)
                    break
                if op in ('bcc', 'dbcc'):
                    work.append(i.target)
                    a = nxt
                    continue
                if op == 'jmp':
                    ea = i.ops[0]
                    if ea.mode in ('absw', 'absl', 'pcdisp'):
                        work.append(ea.value)
                    elif ea.mode == 'pcidx':
                        t = self.jump_table(i, seen)
                        tables[a] = t
                        work.extend(t)
                    break
                if op in ('rts', 'rte', 'rtr', 'illegal', 'stop'):
                    break
                self.note_refs(i)
                a = nxt
        return seen, tables

    def call_target(self, i):
        if i.op == 'bsr':
            return i.target
        ea = i.ops[0]
        if ea.mode in ('absw', 'absl', 'pcdisp'):
            return ea.value
        return None

    def add_entry(self, a, why):
        if a not in self.entries:
            self.entries[a] = why
            self.pending.append(a)
            if self.cur_speculative:
                self.speculative.add(a)

    def note_refs(self, i):
        # addresses of code loaded into registers / pushed: potential call targets
        if i.op in ('lea', 'pea', 'move', 'movea'):
            for ea in i.ops[:1]:
                v = None
                if ea.mode == 'imm' and i.size == 'l':
                    v = ea.value
                elif ea.mode in ('absw', 'absl', 'pcdisp') and i.op in ('lea', 'pea'):
                    v = ea.value
                if v is None or v < 0x200 or v >= ROM_END or v & 1:
                    continue
                if v in self.L.code:
                    self.add_entry(v, f'address taken at {i.addr:06X}')
                elif not self.L.labels.get(v):
                    # Unlabelled bytes the disassembly treats as data but code points at:
                    # try it as code; dropped if it doesn't decode cleanly.
                    if v not in self.entries:
                        self.speculative.add(v)
                    self.add_entry(v, f'address taken at {i.addr:06X} (code hidden in data)')

    def jump_table(self, j, seen):
        """Resolve a compiler switch: MOVE.w (tbl,PC,Dn.w),Dn ; JMP (base,PC,Dn.w)."""
        ea = j.ops[0]
        jbase = ea.base + ea.disp
        # find the MOVE that loads the offset (the instruction right before)
        prev = [x for x in seen.values() if x.addr + x.length == j.addr]
        mv = prev[0] if prev else None
        if not mv or mv.op != 'move' or mv.ops[0].mode != 'pcidx':
            raise TranslateError(f'unrecognised jump table at {j.addr:06X}')
        tbase = mv.ops[0].base + mv.ops[0].disp
        idx = mv.ops[0].xreg[1]
        # scan back for bounds on the index register (before it was doubled)
        lo, hi = None, None
        a = mv.addr
        for _ in range(12):
            p = [x for x in seen.values() if x.addr + x.length == a]
            if not p:
                break
            x = p[0]
            if x.op == 'cmpi' and x.ops[1].mode == 'dn' and x.ops[1].reg == idx:
                nx = self.ins(x.addr + x.length)
                v = d68.s16(x.ops[0].value) if x.size == 'w' else d68.s32(x.ops[0].value)
                if nx.op == 'bcc' and nx.cond in ('gt', 'hi') and hi is None:
                    hi = v
                elif nx.op == 'bcc' and nx.cond in ('lt', 'cs') and lo is None:
                    lo = v
            elif x.ops and x.ops[-1].mode == 'dn' and x.ops[-1].reg == idx and x.op not in (
                    'cmp', 'cmpi', 'tst', 'btst', 'ext') and not (
                    x.op == 'add' and x.ops[0].mode == 'dn' and x.ops[0].reg == idx):
                break  # index register (re)loaded here: earlier compares don't apply
            a = x.addr
        if lo is None:
            lo = 0
        if hi is None:
            raise TranslateError(f'no upper bound for jump table at {j.addr:06X}')
        targets = []
        for k in range(lo, hi + 1):
            off = d68.s16(self.dec.w(tbase + 2 * k))
            targets.append((jbase + off) & 0xFFFFFFFF)
        return targets

    def analyse(self):
        self.speculative = set()
        self.cur_speculative = False
        self.seed_entries()
        self.pending = list(self.entries)
        bodies = {}
        while self.pending:
            a = self.pending.pop()
            if a in bodies:
                continue
            self.cur_speculative = a in self.speculative
            try:
                bodies[a] = self.walk(a)
            except TranslateError as e:
                if a not in self.speculative:
                    self.warnings.append(str(e))
                bodies[a] = None
            if not self.pending:
                # Listing code never reached from any known entry is reached through a
                # computed pointer at runtime: make each such run an entry of its own.
                covered = set()
                for b in bodies.values():
                    if b:
                        covered.update(b[0])
                for c in sorted(self.L.code):
                    if c not in covered and c not in self.entries:
                        self.cur_speculative = False
                        self.add_entry(c, 'listing code not reached statically')
                        covered.add(c)
                        # skip the rest of this run; the walk will cover it
                        n = c
                        while n in self.L.code:
                            covered.add(n)
                            n += self.ins(n).length
        self.bodies = bodies


# --------------------------------------------------------------------------
# C emission
# --------------------------------------------------------------------------
class FuncEmitter:
    def __init__(self, prog, entry, instrs, tables):
        self.P = prog
        self.entry = entry
        self.instrs = instrs
        self.tables = tables
        self.out = []
        self.tmp = 0
        self.targets = set()
        self.back_targets = set()
        for i in instrs.values():
            if i.op in ('bra', 'bcc', 'dbcc'):
                self.targets.add(i.target)
                if i.target <= i.addr:
                    self.back_targets.add(i.target)
            if i.op == 'jmp' and i.ops[0].mode in ('absw', 'absl', 'pcdisp'):
                self.targets.add(i.ops[0].value)
                if i.ops[0].value <= i.addr:
                    self.back_targets.add(i.ops[0].value)
        for t in tables.values():
            self.targets.update(t)

    def t(self):
        self.tmp += 1
        return f't{self.tmp}'

    def line(self, s):
        self.out.append('    ' + s)

    # ---- operands --------------------------------------------------------
    def xreg(self, x):
        kind, n, sz = x
        r = f'R.{"d" if kind == "d" else "a"}[{n}]'
        return f'(uint32_t)(int16_t){r}' if sz == 'w' else r

    def addr(self, ea, size):
        """C expression for the address of a memory operand (emits side effects)."""
        m = ea.mode
        inc = NBYTES[size]
        if m in ('postinc', 'predec') and size == 'b' and ea.reg == 7:
            inc = 2
        if m == 'ind':
            return f'R.a[{ea.reg}]'
        if m == 'postinc':
            v = self.t()
            self.line(f'uint32_t {v} = R.a[{ea.reg}]; R.a[{ea.reg}] += {inc};')
            return v
        if m == 'predec':
            v = self.t()
            self.line(f'uint32_t {v} = (R.a[{ea.reg}] -= {inc});')
            return v
        if m == 'disp':
            return f'(R.a[{ea.reg}] + {ea.disp})'
        if m == 'idx':
            return f'(R.a[{ea.reg}] + {ea.disp} + {self.xreg(ea.xreg)})'
        if m in ('absw', 'absl', 'pcdisp'):
            return f'0x{ea.value & 0xFFFFFFFF:06X}u'
        if m == 'pcidx':
            return f'(0x{(ea.base + ea.disp) & 0xFFFFFFFF:06X}u + {self.xreg(ea.xreg)})'
        raise TranslateError(f'not a memory operand: {m}')

    def fixed_addr(self, ea, size):
        """Address evaluated once into a temp (for read-modify-write)."""
        a = self.addr(ea, size)
        if re.fullmatch(r't\d+|0x[0-9A-F]+u', a):
            return a
        v = self.t()
        self.line(f'uint32_t {v} = {a};')
        return v

    def read(self, ea, size, addr=None):
        m = ea.mode
        if m == 'dn':
            return f'R.d[{ea.reg}]' if size == 'l' else f'(R.d[{ea.reg}] & 0x{MASK[size]:X}u)'
        if m == 'an':
            return f'R.a[{ea.reg}]' if size == 'l' else f'(R.a[{ea.reg}] & 0x{MASK[size]:X}u)'
        if m == 'imm':
            return f'0x{ea.value & MASK[size]:X}u'
        if addr is None:
            addr = self.addr(ea, size)
        return f'rd{NBYTES[size] * 8}({addr})'

    def write(self, ea, size, val, addr=None):
        m = ea.mode
        if m == 'dn':
            if size == 'l':
                self.line(f'R.d[{ea.reg}] = {val};')
            elif size == 'w':
                self.line(f'R.d[{ea.reg}] = (R.d[{ea.reg}] & 0xFFFF0000u) | (({val}) & 0xFFFFu);')
            else:
                self.line(f'R.d[{ea.reg}] = (R.d[{ea.reg}] & 0xFFFFFF00u) | (({val}) & 0xFFu);')
            return
        if m == 'an':
            self.line(f'R.a[{ea.reg}] = {val};')
            return
        if addr is None:
            addr = self.addr(ea, size)
        self.line(f'wr{NBYTES[size] * 8}({addr}, {val});')

    def rmw(self, ea, size):
        """Prepare a read-modify-write operand. Returns (read_expr, addr)."""
        if ea.mode in ('dn', 'an'):
            return self.read(ea, size), None
        a = self.fixed_addr(ea, size)
        return self.read(ea, size, a), a

    # ---- control helpers --------------------------------------------------
    def goto(self, tgt):
        return f'goto L_{tgt:06X};'

    def call(self, i, tgt_expr, direct):
        ret = i.addr + i.length
        self.line('{ uint32_t sp0 = R.a[7];')
        self.line(f'  push32(0x{ret:06X}u);')
        if direct is not None:
            if direct in self.P.funcs:
                self.line(f'  {self.P.fname(direct)}();')
            else:
                self.line(f'  rt_call(0x{direct:06X}u);')
        else:
            self.line(f'  rt_call({tgt_expr});')
        self.line(f'  RT_SPCHECK(sp0, 0x{i.addr:06X}u); }}')

    # ---- instruction ------------------------------------------------------
    def emit_ins(self, i):
        op, sz, o = i.op, i.size, i.ops
        nb = NBYTES.get(sz, 0)

        if op in ('move', 'moveq'):
            if o[1].mode == 'sr':
                v = self.t(); self.line(f'uint32_t {v} = {self.read(o[0], "w")};')
                self.line(f'rt_set_sr(&F, {v});')
                return
            if o[1].mode == 'ccr':
                v = self.t(); self.line(f'uint32_t {v} = {self.read(o[0], "w")};')
                self.line(f'set_ccr(&F, {v});')
                return
            if o[0].mode == 'sr':
                self.write(o[1], 'w', 'rt_get_sr(&F)')
                return
            if o[0].mode == 'usp':
                self.line(f'R.a[{o[1].reg}] = R.usp;')
                return
            if o[1].mode == 'usp':
                self.line(f'R.usp = R.a[{o[0].reg}];')
                return
            v = self.t()
            self.line(f'uint32_t {v} = {self.read(o[0], sz)};')
            self.line(f'logic(&F, {v}, {nb});')
            self.write(o[1], sz, v)
            return
        if op == 'movea':
            v = self.t()
            src = self.read(o[0], sz)
            if sz == 'w':
                src = f'(uint32_t)(int16_t){src}'
            self.line(f'uint32_t {v} = {src};')
            self.line(f'R.a[{o[1].reg}] = {v};')
            return
        if op == 'lea':
            v = self.t()
            self.line(f'uint32_t {v} = {self.addr(o[0], "l")};')
            self.line(f'R.a[{o[1].reg}] = {v};')
            return
        if op == 'pea':
            v = self.t()
            self.line(f'uint32_t {v} = {self.addr(o[0], "l")};')
            self.line(f'push32({v});')
            return
        if op == 'clr':
            if o[0].is_mem():
                # 68000 CLR reads before writing; reads of I/O ports may matter, keep it simple
                a = self.fixed_addr(o[0], sz)
                self.write(o[0], sz, '0', a)
            else:
                self.write(o[0], sz, '0')
            self.line('F.n = 0; F.z = 1; F.v = 0; F.c = 0;')
            return
        if op == 'tst':
            v = self.t()
            self.line(f'uint32_t {v} = {self.read(o[0], sz)};')
            self.line(f'logic(&F, {v}, {nb});')
            return
        if op in ('add', 'sub', 'addi', 'subi', 'addq', 'subq'):
            base = 'add' if op.startswith('add') else 'sub'
            if o[1].mode == 'an':
                s = self.read(o[0], sz)
                sign = '+' if base == 'add' else '-'
                self.line(f'R.a[{o[1].reg}] {sign}= {s};')
                return
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], sz)};')
            d, a = self.rmw(o[1], sz)
            r = self.t()
            self.line(f'uint32_t {r} = op_{base}(&F, {s}, {d}, {nb});')
            self.write(o[1], sz, r, a)
            return
        if op in ('adda', 'suba'):
            s = self.read(o[0], sz)
            if sz == 'w':
                s = f'(uint32_t)(int16_t){s}'
            v = self.t()
            self.line(f'uint32_t {v} = {s};')
            self.line(f'R.a[{o[1].reg}] {"+" if op == "adda" else "-"}= {v};')
            return
        if op in ('cmp', 'cmpi', 'cmpm'):
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], sz)};')
            d = self.t()
            self.line(f'uint32_t {d} = {self.read(o[1], sz)};')
            self.line(f'op_cmp(&F, {s}, {d}, {nb});')
            return
        if op == 'cmpa':
            s = self.read(o[0], sz)
            if sz == 'w':
                s = f'(uint32_t)(int16_t){s}'
            v = self.t()
            self.line(f'uint32_t {v} = {s};')
            self.line(f'op_cmp(&F, {v}, R.a[{o[1].reg}], 4);')
            return
        if op in ('and', 'or', 'eor', 'andi', 'ori', 'eori'):
            base = op.rstrip('i') if op.endswith('i') else op
            cop = {'and': '&', 'or': '|', 'eor': '^'}[base]
            if o[1].mode == 'ccr':
                self.line(f'set_ccr(&F, get_ccr(&F) {cop} 0x{o[0].value & 0xFF:X}u);')
                return
            if o[1].mode == 'sr':
                self.line(f'rt_set_sr(&F, rt_get_sr(&F) {cop} 0x{o[0].value & 0xFFFF:X}u);')
                return
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], sz)};')
            d, a = self.rmw(o[1], sz)
            r = self.t()
            self.line(f'uint32_t {r} = ({d} {cop} {s}) & 0x{MASK[sz]:X}u;')
            self.line(f'logic(&F, {r}, {nb});')
            self.write(o[1], sz, r, a)
            return
        if op in ('neg', 'negx', 'not'):
            d, a = self.rmw(o[0], sz)
            r = self.t()
            if op == 'not':
                self.line(f'uint32_t {r} = (~{d}) & 0x{MASK[sz]:X}u;')
                self.line(f'logic(&F, {r}, {nb});')
            else:
                self.line(f'uint32_t {r} = op_{op}(&F, {d}, {nb});')
            self.write(o[0], sz, r, a)
            return
        if op in ('addx', 'subx'):
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], sz)};')
            d, a = self.rmw(o[1], sz)
            r = self.t()
            self.line(f'uint32_t {r} = op_{op}(&F, {s}, {d}, {nb});')
            self.write(o[1], sz, r, a)
            return
        if op in ('abcd', 'sbcd'):
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], "b")};')
            d, a = self.rmw(o[1], 'b')
            r = self.t()
            self.line(f'uint32_t {r} = op_{op}(&F, {s}, {d});')
            self.write(o[1], 'b', r, a)
            return
        if op == 'nbcd':
            d, a = self.rmw(o[0], 'b')
            r = self.t()
            self.line(f'uint32_t {r} = op_sbcd(&F, {d}, 0);')
            self.write(o[0], 'b', r, a)
            return
        if op == 'ext':
            r = o[0].reg
            if sz == 'w':
                self.line(f'R.d[{r}] = (R.d[{r}] & 0xFFFF0000u) | ((uint32_t)(int8_t)R.d[{r}] & 0xFFFFu);')
                self.line(f'logic(&F, R.d[{r}] & 0xFFFFu, 2);')
            else:
                self.line(f'R.d[{r}] = (uint32_t)(int16_t)R.d[{r}];')
                self.line(f'logic(&F, R.d[{r}], 4);')
            return
        if op == 'swap':
            r = o[0].reg
            self.line(f'R.d[{r}] = (R.d[{r}] >> 16) | (R.d[{r}] << 16);')
            self.line(f'logic(&F, R.d[{r}], 4);')
            return
        if op == 'exg':
            a, b = o
            ra = f'R.{"d" if a.mode == "dn" else "a"}[{a.reg}]'
            rb = f'R.{"d" if b.mode == "dn" else "a"}[{b.reg}]'
            v = self.t()
            self.line(f'uint32_t {v} = {ra}; {ra} = {rb}; {rb} = {v};')
            return
        if op in ('mulu', 'muls'):
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], "w")};')
            r = o[1].reg
            if op == 'mulu':
                self.line(f'R.d[{r}] = (R.d[{r}] & 0xFFFFu) * {s};')
            else:
                self.line(f'R.d[{r}] = (uint32_t)((int32_t)(int16_t)R.d[{r}] * (int32_t)(int16_t){s});')
            self.line(f'logic(&F, R.d[{r}], 4);')
            return
        if op in ('divu', 'divs'):
            s = self.t()
            self.line(f'uint32_t {s} = {self.read(o[0], "w")};')
            self.line(f'op_{op}(&F, &R.d[{o[1].reg}], {s}, 0x{i.addr:06X}u);')
            return
        if op in ('btst', 'bchg', 'bclr', 'bset'):
            bit = self.t()
            mod = 31 if o[1].mode == 'dn' else 7
            if o[0].mode == 'imm':
                self.line(f'uint32_t {bit} = {o[0].value & mod}u;')
            else:
                self.line(f'uint32_t {bit} = R.d[{o[0].reg}] & {mod}u;')
            bsz = 'l' if o[1].mode == 'dn' else 'b'
            if op == 'btst':
                d = self.read(o[1], bsz)
                self.line(f'F.z = (({d} >> {bit}) & 1u) == 0;')
                return
            d, a = self.rmw(o[1], bsz)
            v = self.t()
            self.line(f'uint32_t {v} = {d};')
            self.line(f'F.z = (({v} >> {bit}) & 1u) == 0;')
            expr = {'bchg': f'{v} ^ (1u << {bit})', 'bclr': f'{v} & ~(1u << {bit})', 'bset': f'{v} | (1u << {bit})'}[op]
            self.write(o[1], bsz, expr, a)
            return
        if op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
            cnt, dst = o
            if cnt.mode == 'imm':
                c = f'{cnt.value}u'
            else:
                c = self.t()
                self.line(f'uint32_t {c} = R.d[{cnt.reg}] & 63u;')
            d, a = self.rmw(dst, sz)
            r = self.t()
            self.line(f'uint32_t {r} = op_{op}(&F, {d}, {c}, {nb});')
            self.write(dst, sz, r, a)
            return
        if op == 'scc':
            a = self.fixed_addr(o[0], 'b') if o[0].is_mem() else None
            self.write(o[0], 'b', f'({COND_C[i.cond]} ? 0xFFu : 0u)', a)
            return
        if op == 'tas':
            d, a = self.rmw(o[0], 'b')
            v = self.t()
            self.line(f'uint32_t {v} = {d};')
            self.line(f'logic(&F, {v}, 1);')
            self.write(o[0], 'b', f'{v} | 0x80u', a)
            return
        if op == 'movem':
            self.emit_movem(i)
            return
        if op == 'movep':
            if o[0].mode == 'dn':
                self.line(f'movep_store(R.d[{o[0].reg}], R.a[{o[1].reg}] + {o[1].disp}, {nb});')
            else:
                self.line(f'movep_load(&R.d[{o[1].reg}], R.a[{o[0].reg}] + {o[0].disp}, {nb});')
            return
        if op == 'link':
            r = o[0].reg
            disp = d68.s16(o[1].value)
            self.line(f'push32(R.a[{r}]); R.a[{r}] = R.a[7]; R.a[7] += {disp};')
            return
        if op == 'unlk':
            r = o[0].reg
            self.line(f'R.a[7] = R.a[{r}]; R.a[{r}] = pop32();')
            return
        if op == 'chk':
            return  # bounds trap never expected in this game
        if op == 'nop':
            return
        if op in ('trap', 'illegal', 'reset', 'stop', 'trapv'):
            self.line(f'rt_exception("{op}", 0x{i.addr:06X}u);')
            return
        raise TranslateError(f'unhandled op {op} at {i.addr:06X}')

    def emit_movem(self, i):
        mask_ea = [x for x in i.ops if x.mode == 'reglist'][0]
        mem = [x for x in i.ops if x.mode != 'reglist'][0]
        to_mem = i.ops[0].mode == 'reglist'
        nb = NBYTES[i.size]
        regs = [f'R.d[{k}]' for k in range(8)] + [f'R.a[{k}]' for k in range(8)]
        mask = mask_ea.value
        if to_mem and mem.mode == 'predec':
            # mask bit 0 = A7 ... bit 15 = D0; store from A7 down to D0
            order = [regs[15 - b] for b in range(16) if mask & (1 << b)]
            r = mem.reg
            for reg in order:
                v = reg if i.size == 'l' else f'({reg} & 0xFFFFu)'
                self.line(f'R.a[{r}] -= {nb}; wr{nb * 8}(R.a[{r}], {v});')
            return
        order = [regs[b] for b in range(16) if mask & (1 << b)]
        if mem.mode == 'postinc':
            r = mem.reg
            for reg in order:
                if i.size == 'l':
                    self.line(f'{reg} = rd32(R.a[{r}]); R.a[{r}] += 4;')
                else:
                    self.line(f'{reg} = (uint32_t)(int16_t)rd16(R.a[{r}]); R.a[{r}] += 2;')
            return
        base = self.fixed_addr(mem, i.size)
        off = 0
        for reg in order:
            if to_mem:
                v = reg if i.size == 'l' else f'({reg} & 0xFFFFu)'
                self.line(f'wr{nb * 8}({base} + {off}, {v});')
            elif i.size == 'l':
                self.line(f'{reg} = rd32({base} + {off});')
            else:
                self.line(f'{reg} = (uint32_t)(int16_t)rd16({base} + {off});')
            off += nb

    def emit_control(self, i):
        """Emit control-flow instructions. Returns True if handled."""
        op = i.op
        if op == 'bra':
            self.line(self.goto(i.target))
            return True
        if op == 'bcc':
            self.line(f'if {COND_C[i.cond]} {self.goto(i.target)}')
            return True
        if op == 'dbcc':
            r = i.ops[0].reg
            cond = COND_C[i.cond]
            pre = f'if (!{cond}) ' if i.cond != 'f' else ''
            self.line(f'{pre}{{ uint32_t c = (R.d[{r}] - 1) & 0xFFFFu; R.d[{r}] = (R.d[{r}] & 0xFFFF0000u) | c; '
                      f'if (c != 0xFFFFu) {self.goto(i.target)} }}')
            return True
        if op == 'bsr':
            self.call(i, None, i.target)
            return True
        if op == 'jsr':
            ea = i.ops[0]
            if ea.mode in ('absw', 'absl', 'pcdisp'):
                self.call(i, None, ea.value)
            else:
                v = self.t()
                self.line(f'uint32_t {v} = {self.addr(ea, "l")};')
                self.call(i, v, None)
            return True
        if op == 'jmp':
            ea = i.ops[0]
            if ea.mode in ('absw', 'absl', 'pcdisp'):
                self.line(self.goto(ea.value))
            elif ea.mode == 'pcidx':
                v = self.t()
                self.line(f'uint32_t {v} = {self.addr(ea, "l")};')
                self.line(f'switch ({v}) {{')
                for t in sorted(set(self.tables[i.addr])):
                    self.line(f'  case 0x{t:06X}u: {self.goto(t)}')
                self.line(f'  default: rt_bad_jump({v}, 0x{i.addr:06X}u); return;')
                self.line('}')
            else:
                v = self.t()
                self.line(f'uint32_t {v} = {self.addr(ea, "l")};')
                self.line(f'rt_call({v}); return;')
            return True
        if op == 'rts':
            self.line('R.a[7] += 4; return;')
            return True
        if op == 'rtr':
            self.line('set_ccr(&F, pop16()); R.a[7] += 4; return;')
            return True
        if op == 'rte':
            self.line('rt_rte(); return;')
            return True
        return False

    def emit(self):
        P = self.P
        name = P.fname(self.entry)
        addrs = sorted(self.instrs)
        # basic block leaders for cycle accounting
        leaders = {self.entry} | self.targets
        for i in self.instrs.values():
            if i.op in ('bra', 'bcc', 'dbcc', 'jsr', 'bsr', 'jmp', 'rts', 'rte', 'rtr'):
                leaders.add(i.addr + i.length)
        o = []
        why = P.entries.get(self.entry, '')
        o.append(f'/* {why} */')
        o.append(f'void {name}(void)')
        o.append('{')
        o.append('    Flags F = {0, 0, 0, 0, 0};')
        o.append(f'    RT_TRACE(0x{self.entry:06X}u);')
        o.append(f'    goto L_{self.entry:06X};')
        prev_end = None
        for a in addrs:
            i = self.instrs[a]
            if prev_end is not None and prev_end != a:
                # previous instruction fell through to an address outside the run;
                # only reachable by an explicit goto, so nothing to do
                pass
            o.append(f'L_{a:06X}:')
            self.out = []
            if a in leaders:
                cyc = self.block_cycles(a, leaders)
                self.out.append(f'    CY({cyc});')
                if a in self.back_targets or a == self.entry:
                    self.out.append('    RT_POLL();')
            src = P.L.code.get(a, '')
            if not src:
                src = str(i).split(': ', 1)[1] + '   [not in listing]'
            com = P.L.comments.get(a)
            c = f'{a:06X}  {src}' + (f'   ; {com}' if com else '')
            self.out.append('    /* ' + c.replace('*/', '* /') + ' */')
            self.out.append('    {')
            body_start = len(self.out)
            if not self.emit_control(i):
                self.emit_ins(i)
                nxt = a + i.length
                if nxt not in self.instrs:
                    self.line(f'goto L_{nxt:06X}; /* unreachable fallthrough */')
            # indent body by one level
            self.out[body_start:] = ['    ' + s for s in self.out[body_start:]]
            self.out.append('    }')
            o.extend(self.out)
            prev_end = a + i.length
        # labels referenced but never decoded would be a bug; guard
        o.append('}')
        return '\n'.join(o)

    def block_cycles(self, a, leaders):
        total = 0
        while a in self.instrs:
            i = self.instrs[a]
            total += cycles(i)
            if i.op in ('bra', 'bcc', 'dbcc', 'jsr', 'bsr', 'jmp', 'rts', 'rte', 'rtr'):
                break
            a += i.length
            if a in leaders:
                break
        return total


def cycles(i):
    return timing68k.cycles(i)


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------
HEADER = '''/* Generated by tools/m68k2c from the ToeJam & Earl disassembly. Do not edit or commit. */
#include "core/m68k_rt.h"
#include "gen_funcs.h"
'''


ISA_SKIP = {'bsr', 'jsr', 'jmp', 'rts', 'rte', 'rtr', 'trap', 'illegal', 'reset', 'stop', 'trapv',
            'chk', 'nop', 'bra'}


def write_isa_tests(P, addrs, path):
    """One C function per distinct instruction, containing exactly the code the
    translator emits for it. Branches set *taken instead of jumping."""
    seen = {}
    for a in sorted(addrs):
        i = P.ins(a)
        if i.op in ISA_SKIP:
            continue
        if any(e.mode in ('sr', 'usp') for e in i.ops):
            continue
        # leave out instructions that address I/O directly (no reference model for it)
        if any(e.mode in ('absw', 'absl') and 0x400000 <= (e.value & 0xFFFFFF) < 0xE00000 for e in i.ops):
            continue
        raw = bytes(P.rom[a:a + i.length])
        key = (raw, a if any(e.mode in ('pcdisp', 'pcidx') for e in i.ops) or i.target is not None else None)
        if key not in seen:
            seen[key] = i
    out = ['/* Generated by tools/m68k2c --isa-test. Do not edit or commit. */',
           '#include "core/m68k_rt.h"', '#include "isa_test.h"', '']
    table = []
    for (raw, _), i in sorted(seen.items(), key=lambda kv: kv[1].addr):
        em = FuncEmitter(P, i.addr, {i.addr: i}, {})
        em.out = []
        if not em.emit_control(i):
            em.emit_ins(i)
        body = []
        for line in em.out:
            line = re.sub(r'goto L_[0-9A-F]{6};', '{ *taken = 1; return; }', line)
            body.append('    ' + line)
        name = f't_{i.addr:06X}'
        out.append(f'static void {name}(Flags *Fp, int *taken)')
        out.append('{')
        out.append('#define F (*Fp)')
        out.append('    *taken = 0;')
        out.extend(body)
        out.append('#undef F')
        out.append('}')
        text = str(i).split(': ', 1)[1].replace('\\', '\\\\').replace('"', '\\"')
        table.append(f'    {{0x{i.addr:06X}u, {i.length}, "{i.op}", {name}, "{text}"}},')
    out.append('')
    out.append('const IsaTest isa_tests[] = {')
    out.extend(table)
    out.append('};')
    out.append(f'const unsigned isa_test_count = {len(table)};')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    return len(table)


def main():
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--source', help='Source/EntireSource.X68 of the disassembly')
    src.add_argument('--listing', help='vasm listing of the disassembly')
    ap.add_argument('--rom', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--per-file', type=int, default=120)
    ap.add_argument('--isa-test', help='also write a per-instruction test file (see tools/isa_test)')
    args = ap.parse_args()

    rom = open(args.rom, 'rb').read()
    try:
        L = srcmap.build(args.source, rom) if args.source else lst.parse(args.listing)
    except srcmap.SourceMapError as e:
        sys.exit(f'The disassembly does not line up with this ROM: {e}\n'
                 'Check you have "Toejam & Earl (U) (REV 00) [!].bin" and an unmodified disassembly.')
    P = Program(rom, L)
    P.analyse()
    ok = {a: b for a, b in P.bodies.items() if b is not None}
    P.funcs = ok
    os.makedirs(args.out, exist_ok=True)

    names = {a: P.fname(a) for a in ok}
    # emit
    files = []
    chunk, idx = [], 0
    total_ins = 0
    failed = []
    for a in sorted(ok):
        instrs, tables = ok[a]
        total_ins += len(instrs)
        try:
            code = FuncEmitter(P, a, instrs, tables).emit()
        except TranslateError as e:
            failed.append(str(e))
            continue
        chunk.append(code)
        if len(chunk) >= args.per_file:
            files.append(chunk); chunk = []
    if chunk:
        files.append(chunk)
    for n, ch in enumerate(files):
        with open(os.path.join(args.out, f'gen_{n:03d}.c'), 'w') as f:
            f.write(HEADER + '\n' + '\n\n'.join(ch) + '\n')
    with open(os.path.join(args.out, 'gen_funcs.h'), 'w') as f:
        f.write('/* Generated by tools/m68k2c. */\n#pragma once\n')
        for a in sorted(ok):
            f.write(f'void {names[a]}(void);\n')
    with open(os.path.join(args.out, 'gen_table.c'), 'w') as f:
        f.write(HEADER)
        f.write('const RtFunc rt_func_table[] = {\n')
        for a in sorted(ok):
            f.write(f'    {{0x{a:06X}u, {names[a]}, "{names[a]}"}},\n')
        f.write('};\n')
        f.write(f'const unsigned rt_func_count = {len(ok)};\n')
        vec = [int.from_bytes(rom[i * 4:i * 4 + 4], 'big') for i in range(64)]
        f.write(f'const uint32_t rt_reset_pc = 0x{vec[1]:06X}u;\n')
        f.write(f'const uint32_t rt_reset_sp = 0x{vec[0]:06X}u;\n')
        f.write(f'const uint32_t rt_vector_hint = 0x{vec[28]:06X}u;\n')
        f.write(f'const uint32_t rt_vector_vint = 0x{vec[30]:06X}u;\n')
        igl = P.label_addr('InGameLoop')
        f.write(f'const uint32_t rt_entry_ingameloop = 0x{(igl or 0):06X}u;\n')
    uniq = set()
    for a in ok:
        uniq.update(ok[a][0])
    print(f'functions: {len(ok)}  translated instructions: {total_ins} (unique {len(uniq)} of {len(L.code)} in listing)')
    if args.isa_test:
        n = write_isa_tests(P, uniq, args.isa_test)
        print(f'isa tests: {n} distinct instructions -> {args.isa_test}')
    print(f'files: {len(files)}')
    for w in P.warnings + failed:
        print('warning:', w)


if __name__ == '__main__':
    main()
