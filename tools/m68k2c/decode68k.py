"""Motorola 68000 instruction decoder.

Decodes one instruction from a big-endian byte buffer and returns an Instr
describing the operation and its operands. Covers the full 68000 user and
supervisor instruction set (no 68010+ extensions).
"""

from dataclasses import dataclass, field
from typing import List, Optional


class DecodeError(Exception):
    pass


@dataclass
class EA:
    mode: str                 # dn an ind postinc predec disp idx absw absl pcdisp pcidx imm sr ccr usp reglist
    reg: int = 0
    disp: int = 0             # displacement for disp/idx/pcdisp/pcidx
    xreg: tuple = None        # (kind 'd'|'a', num, size 'w'|'l') for idx/pcidx
    value: int = 0            # absolute address / immediate / reglist mask / pc base
    base: int = 0             # for pc-relative: address of the extension word (PC value)

    def is_mem(self):
        return self.mode in ('ind', 'postinc', 'predec', 'disp', 'idx', 'absw', 'absl', 'pcdisp', 'pcidx')

    def __str__(self):
        m = self.mode
        if m == 'dn': return f'D{self.reg}'
        if m == 'an': return f'A{self.reg}'
        if m == 'ind': return f'(A{self.reg})'
        if m == 'postinc': return f'(A{self.reg})+'
        if m == 'predec': return f'-(A{self.reg})'
        if m == 'disp': return f'{self.disp}(A{self.reg})'
        if m == 'idx': return f'{self.disp}(A{self.reg},{self.xreg[0].upper()}{self.xreg[1]}.{self.xreg[2]})'
        if m == 'absw': return f'${self.value:X}.w'
        if m == 'absl': return f'${self.value:X}.l'
        if m == 'pcdisp': return f'${self.value:X}(PC)'
        if m == 'pcidx': return f'{self.disp}(PC,{self.xreg[0].upper()}{self.xreg[1]}.{self.xreg[2]})'
        if m == 'imm': return f'#${self.value:X}'
        if m == 'reglist': return f'<{self.value:04X}>'
        return m.upper()


@dataclass
class Instr:
    addr: int
    length: int
    op: str                     # canonical mnemonic, lower case (e.g. 'move', 'bcc', 'dbcc')
    size: Optional[str] = None  # 'b' 'w' 'l'
    ops: List[EA] = field(default_factory=list)
    cond: Optional[str] = None  # condition code name for bcc/dbcc/scc
    target: Optional[int] = None  # branch target for bcc/bra/bsr/dbcc
    raw: int = 0

    def __str__(self):
        name = self.op
        if self.cond is not None:
            name = name.replace('cc', self.cond)
        if self.size:
            name += '.' + self.size
        s = ', '.join(str(o) for o in self.ops)
        if self.target is not None:
            s = (s + ', ' if s else '') + f'${self.target:X}'
        return f'{self.addr:06X}: {name} {s}'


CONDS = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']
SIZES = {0: 'b', 1: 'w', 2: 'l'}


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


class Decoder:
    def __init__(self, mem: bytes):
        self.mem = mem

    def w(self, a):
        if a + 1 >= len(self.mem):
            raise DecodeError(f'read past end at {a:X}')
        return (self.mem[a] << 8) | self.mem[a + 1]

    def l(self, a):
        return (self.w(a) << 16) | self.w(a + 2)

    # ---- effective address -------------------------------------------------
    def ea(self, mode, reg, size):
        """Decode an EA using extension words at self.p; advances self.p."""
        p = self.p
        if mode == 0: return EA('dn', reg)
        if mode == 1: return EA('an', reg)
        if mode == 2: return EA('ind', reg)
        if mode == 3: return EA('postinc', reg)
        if mode == 4: return EA('predec', reg)
        if mode == 5:
            self.p += 2
            return EA('disp', reg, disp=s16(self.w(p)))
        if mode == 6:
            ext = self.w(p); self.p += 2
            if ext & 0x0700:
                raise DecodeError('full-format index ext on 68000')
            return EA('idx', reg, disp=s8(ext), xreg=('a' if ext & 0x8000 else 'd', (ext >> 12) & 7, 'l' if ext & 0x800 else 'w'))
        if mode == 7:
            if reg == 0:
                self.p += 2
                return EA('absw', value=s16(self.w(p)) & 0xFFFFFFFF)
            if reg == 1:
                self.p += 4
                return EA('absl', value=self.l(p))
            if reg == 2:
                self.p += 2
                d = s16(self.w(p))
                return EA('pcdisp', disp=d, base=p, value=(p + d) & 0xFFFFFFFF)
            if reg == 3:
                ext = self.w(p); self.p += 2
                if ext & 0x0700:
                    raise DecodeError('full-format index ext on 68000')
                return EA('pcidx', disp=s8(ext), base=p, xreg=('a' if ext & 0x8000 else 'd', (ext >> 12) & 7, 'l' if ext & 0x800 else 'w'))
            if reg == 4:
                if size == 'l':
                    self.p += 4
                    return EA('imm', value=self.l(p))
                self.p += 2
                v = self.w(p)
                return EA('imm', value=(v & 0xFF) if size == 'b' else v)
        raise DecodeError(f'bad EA mode {mode} reg {reg}')

    def imm(self, size):
        p = self.p
        if size == 'l':
            self.p += 4
            return EA('imm', value=self.l(p))
        self.p += 2
        v = self.w(p)
        return EA('imm', value=(v & 0xFF) if size == 'b' else v)

    # ---- main decode ---------------------------------------------------------
    def decode(self, addr) -> Instr:
        self.p = addr + 2
        op = self.w(addr)
        ins = self._decode(addr, op)
        ins.addr = addr
        ins.raw = op
        ins.length = self.p - addr
        return ins

    def _decode(self, a, op):
        hi = op >> 12
        mode = (op >> 3) & 7
        reg = op & 7
        reg9 = (op >> 9) & 7
        szb = (op >> 6) & 3
        I = lambda *x, **k: Instr(a, 0, *x, **k)

        if hi == 0:
            if op & 0x100:
                if mode == 1:  # MOVEP
                    d = s16(self.w(self.p)); self.p += 2
                    size = 'l' if op & 0x40 else 'w'
                    mem = EA('disp', reg, disp=d)
                    if op & 0x80:
                        return I('movep', size, [EA('dn', reg9), mem])
                    return I('movep', size, [mem, EA('dn', reg9)])
                bop = ['btst', 'bchg', 'bclr', 'bset'][szb]
                dst = self.ea(mode, reg, 'b')
                return I(bop, 'l' if mode == 0 else 'b', [EA('dn', reg9), dst])
            kind = reg9
            if kind == 4:  # static bit ops
                bop = ['btst', 'bchg', 'bclr', 'bset'][szb]
                bit = self.imm('b')
                dst = self.ea(mode, reg, 'b')
                return I(bop, 'l' if mode == 0 else 'b', [bit, dst])
            names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
            if kind not in names or szb == 3:
                raise DecodeError(f'illegal {op:04X}')
            name = names[kind]
            size = SIZES[szb]
            if mode == 7 and reg == 4 and name in ('ori', 'andi', 'eori'):
                src = self.imm(size)
                return I(name, size, [src, EA('ccr' if size == 'b' else 'sr')])
            src = self.imm(size)
            dst = self.ea(mode, reg, size)
            return I(name, size, [src, dst])

        if hi in (1, 2, 3):
            size = {1: 'b', 3: 'w', 2: 'l'}[hi]
            src = self.ea(mode, reg, size)
            dmode = (op >> 6) & 7
            dst = self.ea(dmode, reg9, size)
            if dmode == 1:
                return I('movea', size, [src, dst])
            return I('move', size, [src, dst])

        if hi == 4:
            if op & 0x100:
                if szb == 3:
                    return I('lea', 'l', [self.ea(mode, reg, 'l'), EA('an', reg9)])
                if szb == 2:
                    return I('chk', 'w', [self.ea(mode, reg, 'w'), EA('dn', reg9)])
                raise DecodeError(f'illegal {op:04X}')
            sub = (op >> 8) & 0xF
            if sub == 0x0:
                if szb == 3: return I('move', 'w', [EA('sr'), self.ea(mode, reg, 'w')])
                return I('negx', SIZES[szb], [self.ea(mode, reg, SIZES[szb])])
            if sub == 0x2:
                if szb == 3: raise DecodeError(f'illegal {op:04X}')
                return I('clr', SIZES[szb], [self.ea(mode, reg, SIZES[szb])])
            if sub == 0x4:
                if szb == 3: return I('move', 'w', [self.ea(mode, reg, 'w'), EA('ccr')])
                return I('neg', SIZES[szb], [self.ea(mode, reg, SIZES[szb])])
            if sub == 0x6:
                if szb == 3: return I('move', 'w', [self.ea(mode, reg, 'w'), EA('sr')])
                return I('not', SIZES[szb], [self.ea(mode, reg, SIZES[szb])])
            if sub == 0x8:
                if szb == 0: return I('nbcd', 'b', [self.ea(mode, reg, 'b')])
                if szb == 1:
                    if mode == 0: return I('swap', 'w', [EA('dn', reg)])
                    return I('pea', 'l', [self.ea(mode, reg, 'l')])
                if mode == 0:
                    return I('ext', 'w' if szb == 2 else 'l', [EA('dn', reg)])
                size = 'w' if szb == 2 else 'l'
                mask = self.w(self.p); self.p += 2
                return I('movem', size, [EA('reglist', value=mask), self.ea(mode, reg, size)])
            if sub == 0xA:
                if op == 0x4AFC: return I('illegal')
                if szb == 3: return I('tas', 'b', [self.ea(mode, reg, 'b')])
                return I('tst', SIZES[szb], [self.ea(mode, reg, SIZES[szb])])
            if sub == 0xC:
                if szb in (2, 3):
                    size = 'w' if szb == 2 else 'l'
                    mask = self.w(self.p); self.p += 2
                    return I('movem', size, [self.ea(mode, reg, size), EA('reglist', value=mask)])
                raise DecodeError(f'illegal {op:04X}')
            if sub == 0xE:
                if (op & 0xFFF0) == 0x4E40: return I('trap', None, [EA('imm', value=op & 0xF)])
                if (op & 0xFFF8) == 0x4E50:
                    d = s16(self.w(self.p)); self.p += 2
                    return I('link', 'w', [EA('an', reg), EA('imm', value=d & 0xFFFF)])
                if (op & 0xFFF8) == 0x4E58: return I('unlk', None, [EA('an', reg)])
                if (op & 0xFFF8) == 0x4E60: return I('move', 'l', [EA('an', reg), EA('usp')])
                if (op & 0xFFF8) == 0x4E68: return I('move', 'l', [EA('usp'), EA('an', reg)])
                simple = {0x4E70: 'reset', 0x4E71: 'nop', 0x4E73: 'rte', 0x4E75: 'rts', 0x4E76: 'trapv', 0x4E77: 'rtr'}
                if op in simple: return I(simple[op])
                if op == 0x4E72:
                    v = self.w(self.p); self.p += 2
                    return I('stop', None, [EA('imm', value=v)])
                if szb == 2: return I('jsr', None, [self.ea(mode, reg, 'l')])
                if szb == 3: return I('jmp', None, [self.ea(mode, reg, 'l')])
            raise DecodeError(f'illegal {op:04X}')

        if hi == 5:
            if szb == 3:
                cond = CONDS[(op >> 8) & 0xF]
                if mode == 1:
                    d = s16(self.w(self.p)); tgt = (self.p + d) & 0xFFFFFFFF; self.p += 2
                    return I('dbcc', 'w', [EA('dn', reg)], cond=cond, target=tgt)
                return I('scc', 'b', [self.ea(mode, reg, 'b')], cond=cond)
            data = reg9 or 8
            size = SIZES[szb]
            return I('subq' if op & 0x100 else 'addq', size, [EA('imm', value=data), self.ea(mode, reg, size)])

        if hi == 6:
            c = (op >> 8) & 0xF
            d8 = op & 0xFF
            if d8 == 0:
                d = s16(self.w(self.p)); tgt = (a + 2 + d) & 0xFFFFFFFF; self.p += 2; bs = 'w'
            elif d8 == 0xFF:
                raise DecodeError('32-bit branch on 68000')
            else:
                tgt = (a + 2 + s8(d8)) & 0xFFFFFFFF; bs = 'b'
            if c == 0: return I('bra', bs, [], target=tgt)
            if c == 1: return I('bsr', bs, [], target=tgt)
            return I('bcc', bs, [], cond=CONDS[c], target=tgt)

        if hi == 7:
            if op & 0x100: raise DecodeError(f'illegal {op:04X}')
            return I('moveq', 'l', [EA('imm', value=s8(op) & 0xFFFFFFFF), EA('dn', reg9)])

        if hi in (8, 9, 0xB, 0xC, 0xD):
            opmode = (op >> 6) & 7
            if hi == 8:
                if opmode == 3: return I('divu', 'w', [self.ea(mode, reg, 'w'), EA('dn', reg9)])
                if opmode == 7: return I('divs', 'w', [self.ea(mode, reg, 'w'), EA('dn', reg9)])
                if opmode == 4 and mode in (0, 1):
                    return I('sbcd', 'b', self._xops(mode, reg, reg9))
                return self._arith('or', op, mode, reg, reg9, a)
            if hi == 0xC:
                if opmode == 3: return I('mulu', 'w', [self.ea(mode, reg, 'w'), EA('dn', reg9)])
                if opmode == 7: return I('muls', 'w', [self.ea(mode, reg, 'w'), EA('dn', reg9)])
                if opmode == 4 and mode in (0, 1):
                    return I('abcd', 'b', self._xops(mode, reg, reg9))
                if opmode == 5 and mode == 0: return I('exg', 'l', [EA('dn', reg9), EA('dn', reg)])
                if opmode == 5 and mode == 1: return I('exg', 'l', [EA('an', reg9), EA('an', reg)])
                if opmode == 6 and mode == 1: return I('exg', 'l', [EA('dn', reg9), EA('an', reg)])
                return self._arith('and', op, mode, reg, reg9, a)
            if hi in (9, 0xD):
                name = 'sub' if hi == 9 else 'add'
                if opmode in (3, 7):
                    size = 'w' if opmode == 3 else 'l'
                    return I(name + 'a', size, [self.ea(mode, reg, size), EA('an', reg9)])
                if opmode >= 4 and mode in (0, 1):
                    return I(name + 'x', SIZES[opmode - 4], self._xops(mode, reg, reg9))
                return self._arith(name, op, mode, reg, reg9, a)
            if hi == 0xB:
                if opmode in (3, 7):
                    size = 'w' if opmode == 3 else 'l'
                    return I('cmpa', size, [self.ea(mode, reg, size), EA('an', reg9)])
                if opmode < 3:
                    size = SIZES[opmode]
                    return I('cmp', size, [self.ea(mode, reg, size), EA('dn', reg9)])
                size = SIZES[opmode - 4]
                if mode == 1:
                    return I('cmpm', size, [EA('postinc', reg), EA('postinc', reg9)])
                return I('eor', size, [EA('dn', reg9), self.ea(mode, reg, size)])

        if hi == 0xE:
            if szb == 3:
                kind = (op >> 9) & 3
                if op & 0x800: raise DecodeError(f'illegal {op:04X}')
                name = ['as', 'ls', 'rox', 'ro'][kind] + ('l' if op & 0x100 else 'r')
                return I(name, 'w', [EA('imm', value=1), self.ea(mode, reg, 'w')])
            kind = (op >> 3) & 3
            name = ['as', 'ls', 'rox', 'ro'][kind] + ('l' if op & 0x100 else 'r')
            size = SIZES[szb]
            if op & 0x20:
                cnt = EA('dn', reg9)
            else:
                cnt = EA('imm', value=reg9 or 8)
            return I(name, size, [cnt, EA('dn', reg)])

        raise DecodeError(f'line-{hi:X} opcode {op:04X} at {a:06X}')

    def _xops(self, mode, reg, reg9):
        if mode == 0:
            return [EA('dn', reg), EA('dn', reg9)]
        return [EA('predec', reg), EA('predec', reg9)]

    def _arith(self, name, op, mode, reg, reg9, a):
        opmode = (op >> 6) & 7
        size = SIZES[opmode & 3]
        if opmode < 4:
            return Instr(a, 0, name, size, [self.ea(mode, reg, size), EA('dn', reg9)])
        return Instr(a, 0, name, size, [EA('dn', reg9), self.ea(mode, reg, size)])
