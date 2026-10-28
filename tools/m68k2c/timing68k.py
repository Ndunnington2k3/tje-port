"""68000 instruction execution times (in clock cycles), after the tables in the
Motorola M68000 User's Manual, section 8. Data-dependent timings (shifts by a
register, MULU/MULS, DIVU/DIVS) use typical values.

Conditional branches are costed as taken; the translator charges a whole
basic block at once, so this is a close approximation of the real timing.
"""

# Effective address calculation time: (byte/word, long)
EA = {
    'dn': (0, 0), 'an': (0, 0), 'ind': (4, 8), 'postinc': (4, 8), 'predec': (6, 10),
    'disp': (8, 12), 'idx': (10, 14), 'absw': (8, 12), 'absl': (12, 16),
    'pcdisp': (8, 12), 'pcidx': (10, 14), 'imm': (4, 8),
}

# MOVE destination write time (byte/word, long), excluding the source EA
MOVE_DST = {
    'dn': (0, 0), 'an': (0, 0), 'ind': (4, 8), 'postinc': (4, 8), 'predec': (4, 8),
    'disp': (8, 12), 'idx': (10, 14), 'absw': (8, 12), 'absl': (12, 16),
}

# Control addressing modes: LEA / PEA / JSR / JMP
LEA = {'ind': 4, 'disp': 8, 'idx': 12, 'absw': 8, 'absl': 12, 'pcdisp': 8, 'pcidx': 12}
PEA = {'ind': 12, 'disp': 16, 'idx': 20, 'absw': 16, 'absl': 20, 'pcdisp': 16, 'pcidx': 20}
JSR = {'ind': 16, 'disp': 18, 'idx': 22, 'absw': 18, 'absl': 20, 'pcdisp': 18, 'pcidx': 22}
JMP = {'ind': 8, 'disp': 10, 'idx': 14, 'absw': 10, 'absl': 12, 'pcdisp': 10, 'pcidx': 14}

# MOVEM base times (n = number of registers): (memory->registers, registers->memory)
MOVEM = {'ind': (12, 8), 'postinc': (12, None), 'predec': (None, 8), 'disp': (16, 12),
         'idx': (18, 14), 'absw': (16, 12), 'absl': (20, 16), 'pcdisp': (16, None), 'pcidx': (18, None)}


def ea(e, size):
    t = EA.get(e.mode)
    if t is None:
        return 0
    return t[1] if size == 'l' else t[0]


def is_reg(e):
    return e.mode in ('dn', 'an')


def cycles(i):
    op, sz, o = i.op, (i.size or 'w'), i.ops
    L = sz == 'l'

    if op in ('move', 'moveq'):
        if op == 'moveq':
            return 4
        src, dst = o
        if dst.mode == 'sr' or dst.mode == 'ccr':
            return 12 + ea(src, 'w')
        if src.mode == 'sr':
            return 6 if dst.mode == 'dn' else 8 + ea(dst, 'w')
        if 'usp' in (src.mode, dst.mode):
            return 4
        d = MOVE_DST.get(dst.mode, (0, 0))
        return 4 + ea(src, sz) + (d[1] if L else d[0])
    if op == 'movea':
        return 4 + ea(o[0], sz)
    if op in ('add', 'sub', 'and', 'or', 'cmp', 'eor'):
        src, dst = o
        if dst.mode == 'dn' and op != 'eor':
            if L:
                return 6 + ea(src, sz) + (2 if src.mode in ('dn', 'an', 'imm') else 0)
            return 4 + ea(src, sz)
        if op == 'eor' and dst.mode == 'dn':
            return 8 if L else 4
        return (12 if L else 8) + ea(dst, sz)
    if op in ('adda', 'suba'):
        if L:
            return 6 + ea(o[0], sz) + (2 if o[0].mode in ('dn', 'an', 'imm') else 0)
        return 8 + ea(o[0], sz)
    if op == 'cmpa':
        return 6 + ea(o[0], sz)
    if op in ('addi', 'subi', 'andi', 'ori', 'eori'):
        dst = o[1]
        if dst.mode in ('sr', 'ccr'):
            return 20
        if dst.mode == 'dn':
            return 16 if L else 8
        return (20 if L else 12) + ea(dst, sz)
    if op == 'cmpi':
        dst = o[1]
        if dst.mode == 'dn':
            return 14 if L else 8
        return (12 if L else 8) + ea(dst, sz)
    if op in ('addq', 'subq'):
        dst = o[1]
        if dst.mode == 'dn':
            return 8 if L else 4
        if dst.mode == 'an':
            return 8
        return (12 if L else 8) + ea(dst, sz)
    if op in ('clr', 'neg', 'negx', 'not'):
        if o[0].mode == 'dn':
            return 6 if L else 4
        return (12 if L else 8) + ea(o[0], sz)
    if op == 'tst':
        return 4 + ea(o[0], sz)
    if op in ('ext', 'swap', 'nop'):
        return 4
    if op == 'exg':
        return 6
    if op == 'lea':
        return LEA.get(o[0].mode, 8)
    if op == 'pea':
        return PEA.get(o[0].mode, 16)
    if op in ('asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'):
        if o[1].mode != 'dn':
            return 8 + ea(o[1], 'w')
        n = o[0].value if o[0].mode == 'imm' else 4  # register counts: assume small
        return (8 if L else 6) + 2 * n
    if op in ('mulu', 'muls'):
        return 54 + ea(o[0], 'w')
    if op == 'divu':
        return 136 + ea(o[0], 'w')
    if op == 'divs':
        return 156 + ea(o[0], 'w')
    if op == 'btst':
        if o[1].mode == 'dn':
            return 10 if o[0].mode == 'imm' else 6
        return (8 if o[0].mode == 'imm' else 4) + ea(o[1], 'b')
    if op in ('bset', 'bclr', 'bchg'):
        if o[1].mode == 'dn':
            return 12 if o[0].mode == 'imm' else 8
        return (12 if o[0].mode == 'imm' else 8) + ea(o[1], 'b')
    if op == 'scc':
        return 6 if o[0].mode == 'dn' else 8 + ea(o[0], 'b')
    if op == 'tas':
        return 4 if o[0].mode == 'dn' else 10 + ea(o[0], 'b')
    if op in ('abcd', 'sbcd'):
        return 6 if o[0].mode == 'dn' else 18
    if op == 'nbcd':
        return 6 if o[0].mode == 'dn' else 8 + ea(o[0], 'b')
    if op in ('addx', 'subx'):
        if o[0].mode == 'dn':
            return 8 if L else 4
        return 30 if L else 18
    if op == 'cmpm':
        return 20 if L else 12
    if op == 'movem':
        mask = [x for x in o if x.mode == 'reglist'][0].value
        n = bin(mask).count('1')
        mem = [x for x in o if x.mode != 'reglist'][0]
        to_mem = o[0].mode == 'reglist'
        base = MOVEM.get(mem.mode, (12, 8))[1 if to_mem else 0] or 12
        return base + n * (8 if L else 4)
    if op == 'movep':
        return 24 if L else 16
    if op == 'link':
        return 16
    if op == 'unlk':
        return 12
    if op == 'bra':
        return 10
    if op == 'bcc':
        return 10
    if op == 'dbcc':
        return 10
    if op == 'bsr':
        return 18
    if op == 'jsr':
        return JSR.get(o[0].mode, 18)
    if op == 'jmp':
        return JMP.get(o[0].mode, 10)
    if op == 'rts':
        return 16
    if op in ('rte', 'rtr'):
        return 20
    if op == 'chk':
        return 10 + ea(o[0], 'w')
    return 4
