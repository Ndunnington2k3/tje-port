"""Parse a vasm listing file (-L) of the disassembly.

Gives, for every source line that produced bytes, its address and source text,
plus which addresses carry labels and which lines are instructions (vs data).
"""

import re
from dataclasses import dataclass, field

LINE_RE = re.compile(r'^00:([0-9A-F]{8}) ([0-9A-F]*)\s*\t\s*(\d+): ?(.*)$')
CONT_RE = re.compile(r'^00:([0-9A-F]{8}) ([0-9A-F]+)\s*$')
LABEL_ONLY_RE = re.compile(r'^\s*\t\s*(\d+): ?(.*)$')
LABEL_RE = re.compile(r'^\s*([A-Za-z_.][A-Za-z0-9_.]*):?\s*$')

DIRECTIVES = {'DC', 'DS', 'DCB', 'INCBIN', 'EVEN', 'ALIGN', 'CNOP', 'ORG', 'EQU', 'SET', 'RS', 'RSRESET'}


@dataclass
class Listing:
    code: dict = field(default_factory=dict)      # addr -> source text (instruction lines)
    data: set = field(default_factory=set)        # addrs of data lines (start addresses)
    labels: dict = field(default_factory=dict)    # addr -> [label names] (global first)
    comments: dict = field(default_factory=dict)  # addr -> trailing comment


def _strip_comment(s):
    # comments start with ';' (no string literals in code lines worth worrying about)
    i = s.find(';')
    if i >= 0:
        return s[:i], s[i + 1:].strip()
    return s, ''


def parse(path) -> Listing:
    L = Listing()
    pending = []          # labels waiting for the next addressed line
    scope = ''            # current global label for local (.xxx) labels
    with open(path, errors='replace') as f:
        for raw in f:
            raw = raw.rstrip('\n')
            if raw.startswith('Symbols by name') or raw.startswith('Sections:') and L.code:
                if raw.startswith('Symbols'):
                    break
            m = LINE_RE.match(raw)
            if m:
                addr = int(m.group(1), 16)
                src, com = _strip_comment(m.group(4))
                text = src.strip()
                if not text:
                    continue
                # leading label on same line?
                lm = re.match(r'^([A-Za-z_.][A-Za-z0-9_.]*):\s*(.*)$', text)
                if lm and not src[:1].isspace():
                    pending.append(lm.group(1))
                    text = lm.group(2).strip()
                elif not src[:1].isspace():
                    # label without colon in column 0 followed by an instruction
                    parts = text.split(None, 1)
                    if len(parts) == 2 and parts[1].split()[0].split('.')[0].upper() not in ('EQU', 'SET'):
                        first = parts[1].split()[0].split('.')[0].upper()
                        pending.append(parts[0])
                        text = parts[1].strip()
                for name in pending:
                    if name.startswith('.'):
                        name = scope + name
                    else:
                        scope = name
                    L.labels.setdefault(addr, []).append(name)
                pending = []
                if not text:
                    continue
                tok = text.split()[0].split('.')[0].upper()
                if tok in DIRECTIVES:
                    L.data.add(addr)
                else:
                    L.code[addr] = text
                    if com:
                        L.comments[addr] = com
                continue
            m = LABEL_ONLY_RE.match(raw)
            if m:
                src, _ = _strip_comment(m.group(2))
                lm = LABEL_RE.match(src)
                if lm and src.strip() and not src[:1].isspace():
                    pending.append(lm.group(1))
                elif lm and src.strip().endswith(':'):
                    pending.append(lm.group(1))
    return L
