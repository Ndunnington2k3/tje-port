"""Map the disassembly source onto the ROM without an assembler.

Because the disassembly assembles to a byte-identical copy of the ROM, each
source line's address can be found by walking the source and the ROM together:
instructions take the length the decoder finds at that address, DC.B/W/L take
their item sizes, INCBIN takes the file size. The result is the same Listing
structure that listing.py builds from a vasm listing.

Checks made along the way (any failure raises SourceMapError):
  * every instruction's mnemonic matches what the decoder finds in the ROM,
  * every literal DC byte/word/long equals the ROM contents,
  * the walk ends exactly at the end of the ROM.
"""

import os
import re

import decode68k as d68
from listing import Listing


class SourceMapError(Exception):
    pass


LABEL_COL0 = re.compile(r'^([A-Za-z_.][A-Za-z0-9_.]*):?(\s+|$)(.*)$')


def split_comment(line):
    out, q = [], None
    for i, ch in enumerate(line):
        if q:
            if ch == q:
                q = None
        elif ch in '\'"':
            q = ch
        elif ch == ';':
            return line[:i], line[i + 1:].strip()
        out.append(ch)
    return line, ''


def split_operands(s):
    items, cur, q, depth = [], [], None, 0
    for ch in s:
        if q:
            cur.append(ch)
            if ch == q:
                q = None
            continue
        if ch in '\'"':
            q = ch
            cur.append(ch)
        elif ch == '(':
            depth += 1
            cur.append(ch)
        elif ch == ')':
            depth -= 1
            cur.append(ch)
        elif ch == ',' and depth == 0:
            items.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur or items:
        items.append(''.join(cur).strip())
    return [x for x in items if x != '']


def literal(item):
    """Value of a plain numeric literal, or None if it's an expression/symbol."""
    item = item.strip()
    try:
        if item.startswith('$'):
            return int(item[1:], 16)
        if item.startswith('%'):
            return int(item[1:], 2)
        if re.fullmatch(r'-?\d+', item):
            return int(item)
    except ValueError:
        return None
    return None


# decoder op -> source mnemonics it can come from
def mnemonic_ok(src, ins):
    s = src.upper().split('.')[0]
    op = ins.op
    if op == 'bcc':
        return s.startswith('B') and s not in ('BRA', 'BSR', 'BTST', 'BSET', 'BCLR', 'BCHG')
    if op == 'dbcc':
        return s.startswith('DB')
    if op == 'scc':
        return s.startswith('S') and s not in ('SUB', 'SUBA', 'SUBI', 'SUBQ', 'SUBX', 'SWAP', 'SBCD', 'STOP')
    family = {
        'move': ('MOVE',), 'movea': ('MOVEA', 'MOVE'), 'moveq': ('MOVEQ', 'MOVE'),
        'add': ('ADD',), 'adda': ('ADDA', 'ADD'), 'addi': ('ADDI', 'ADD'), 'addq': ('ADDQ', 'ADD'),
        'sub': ('SUB',), 'suba': ('SUBA', 'SUB'), 'subi': ('SUBI', 'SUB'), 'subq': ('SUBQ', 'SUB'),
        'cmp': ('CMP',), 'cmpa': ('CMPA', 'CMP'), 'cmpi': ('CMPI', 'CMP'), 'cmpm': ('CMPM', 'CMP'),
        'and': ('AND',), 'andi': ('ANDI', 'AND'), 'or': ('OR',), 'ori': ('ORI', 'OR'),
        'eor': ('EOR',), 'eori': ('EORI', 'EOR'),
    }.get(op, (op.upper(),))
    return s in family


class Walker:
    def __init__(self, src_path, rom):
        self.src_path = src_path
        self.src_dir = os.path.dirname(os.path.abspath(src_path))
        self.rom = rom
        self.dec = d68.Decoder(rom)

    def incbin_size(self, name):
        path = os.path.join(self.src_dir, *re.split(r'[\\/]', name))
        if not os.path.isfile(path):
            raise SourceMapError(f'INCBIN file not found: {path}')
        return os.path.getsize(path)

    def run(self) -> Listing:
        L = Listing()
        addr = 0
        pending = []
        scope = ''
        with open(self.src_path, encoding='latin-1') as f:
            lines = f.read().replace('\r\n', '\n').split('\n')
        for lineno, raw in enumerate(lines, 1):
            body, com = split_comment(raw)
            if not body.strip():
                continue
            text = body
            if not body[:1].isspace():
                m = LABEL_COL0.match(body)
                if not m:
                    raise SourceMapError(f'line {lineno}: cannot parse label: {body!r}')
                rest = m.group(3).strip()
                if rest.split()[:1] and rest.split()[0].upper() in ('EQU', 'SET', '='):
                    continue
                pending.append(m.group(1))
                text = rest
            text = text.strip()
            # a label may also be indented, as long as it ends with a colon
            m = re.match(r'^([A-Za-z_.][A-Za-z0-9_.]*):(\s+|$)(.*)$', text)
            if m:
                pending.append(m.group(1))
                text = m.group(3).strip()
            if not text:
                continue
            for name in pending:
                if name.startswith('.'):
                    name = scope + name
                else:
                    scope = name
                L.labels.setdefault(addr, []).append(name)
            pending = []
            parts = text.split(None, 1)
            mnem = parts[0]
            ops = parts[1] if len(parts) > 1 else ''
            m_up = mnem.upper()
            if m_up == 'INCBIN':
                name = re.search(r'"([^"]+)"', ops).group(1)
                L.data.add(addr)
                addr += self.incbin_size(name)
                continue
            if m_up in ('DC.B', 'DC.W', 'DC.L', 'DC'):
                size = {'DC.B': 1, 'DC.W': 2, 'DC.L': 4, 'DC': 2}[m_up]
                # (vasm does not align DC.W/DC.L, and the source relies on that)
                L.data.add(addr)
                for item in split_operands(ops):
                    if item[:1] in '\'"':
                        qc = item[0]
                        sval = item[1:-1].replace(qc + qc, qc).encode('latin-1')  # '' = escaped quote
                        n = len(sval)
                        if size > 1:
                            n = (n + size - 1) // size * size
                        if self.rom[addr:addr + len(sval)] != sval:
                            raise SourceMapError(f'line {lineno}: string does not match ROM at {addr:06X}')
                        addr += n
                        continue
                    v = literal(item)
                    if v is not None:
                        want = (v & ((1 << (8 * size)) - 1)).to_bytes(size, 'big')
                        if self.rom[addr:addr + size] != want:
                            raise SourceMapError(f'line {lineno}: {m_up} {item} does not match ROM at {addr:06X}')
                    addr += size
                continue
            if m_up.split('.')[0] in ('DS', 'DCB', 'EVEN', 'ALIGN', 'CNOP', 'ORG', 'SECTION', 'INCLUDE'):
                raise SourceMapError(f'line {lineno}: directive {mnem} not supported by srcmap')
            # an instruction
            try:
                ins = self.dec.decode(addr)
            except d68.DecodeError as e:
                raise SourceMapError(f'line {lineno}: {text!r} does not decode at {addr:06X}: {e}')
            if not mnemonic_ok(mnem, ins):
                raise SourceMapError(f'line {lineno}: source {mnem} but ROM has {ins.op} at {addr:06X}')
            L.code[addr] = text
            if com:
                L.comments[addr] = com
            addr += ins.length
        if addr != len(self.rom):
            raise SourceMapError(f'source ends at {addr:06X} but the ROM is {len(self.rom):06X} bytes')
        return L


def build(src_path, rom) -> Listing:
    return Walker(src_path, rom).run()
