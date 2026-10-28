#!/usr/bin/env python3
"""Produce src/generated/ from your copy of the disassembly and ROM.

    python tools/generate.py --disasm <disassembly 'master' folder> --rom <ROM .bin> [--vasm <vasmm68k_mot>]

By default no assembler is needed: tools/m68k2c/srcmap.py walks
Source/EntireSource.X68 and the ROM side by side to find each line's address,
checking every instruction and literal data byte against the ROM.

With --vasm, the source is also assembled and the result must be byte-identical
to the ROM (an extra check; the translation itself still uses the source map):
 1. Copy EntireSource.X68 to a work folder, making INCBIN paths absolute and
    rewriting two PC-relative JSRs that newer vasm versions reject (same bytes).
 2. Assemble with vasm and compare with the ROM.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# SHA-1 of "Toejam & Earl (U) (REV 00) [!].bin", the ROM the disassembly reproduces
KNOWN_ROM_SHA1 = '7f82d8b57fff88bdca5d8aff85b01e231dc1239a'

VASM_CANDIDATES = [
    'vasmm68k_mot', 'vasmm68k_mot.exe', 'vasmm68k_mot_win32.exe',
    r'D:\VASM\vasmm68k_mot_win32.exe', r'C:\VASM\vasmm68k_mot_win32.exe',
]


def find_vasm(explicit):
    if explicit:
        return explicit
    for c in VASM_CANDIDATES:
        p = shutil.which(c) or (c if os.path.isfile(c) else None)
        if p:
            return p
    sys.exit('vasm not found. Install vasm (Motorola syntax, 68k) and pass --vasm <path>.')


def fix_pc_jsr(m):
    disp = -int(m.group(1), 16) & 0xFFFF
    return f'{m.group(0)[:0]}    DC.w $4EBA,${disp:04X}'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disasm', required=True, help="the disassembly folder that contains 'Source'")
    ap.add_argument('--rom', required=True, help='ToeJam & Earl (U) (REV 00) ROM (.bin)')
    ap.add_argument('--vasm', help='also assemble with vasm and check the result matches the ROM')
    ap.add_argument('--out', default=os.path.join(REPO, 'src', 'generated'))
    ap.add_argument('--keep', action='store_true', help='keep the vasm work folder')
    args = ap.parse_args()

    src_dir = os.path.join(args.disasm, 'Source')
    entire = os.path.join(src_dir, 'EntireSource.X68')
    if not os.path.isfile(entire):
        sys.exit(f'{entire} not found')
    sha1 = hashlib.sha1(open(args.rom, 'rb').read()).hexdigest()
    if sha1 != KNOWN_ROM_SHA1:
        print(f'warning: ROM SHA-1 is {sha1}, expected {KNOWN_ROM_SHA1} (US REV 00). '
              'Continuing; the source map will stop if the disassembly does not fit it.')
    if args.vasm:
        verify_with_vasm(entire, src_dir, args.rom, find_vasm(args.vasm), args.keep)

    for f in os.listdir(args.out) if os.path.isdir(args.out) else []:
        if (f.startswith('gen_') and (f.endswith('.c') or f.endswith('.h'))) or f == 'isa_cases.c':
            os.remove(os.path.join(args.out, f))
    rc = subprocess.call([sys.executable, os.path.join(HERE, 'm68k2c', 'm68k2c.py'),
                          '--source', entire, '--rom', args.rom, '--out', args.out,
                          '--isa-test', os.path.join(args.out, 'isa_cases.c')])
    if rc != 0:
        sys.exit(rc)
    print('done: translated code is in', args.out)


def verify_with_vasm(entire, src_dir, rom_path, vasm, keep):
    work = tempfile.mkdtemp(prefix='tje_gen_')
    try:
        text = open(entire, encoding='latin-1').read().replace('\r\n', '\n')
        out_lines = []
        for line in text.split('\n'):
            if re.search(r'\bincbin\b', line, re.I):
                # make INCBIN paths absolute so vasm finds them from the work folder
                line = re.sub(r'"([^"]+)"',
                              lambda m: '"' + os.path.join(src_dir, *re.split(r'[\\/]', m.group(1))).replace('\\', '/') + '"',
                              line)
            line = re.sub(r'^\s*JSR\s+\(-\$([0-9A-Fa-f]+),\s*PC\)\s*$', fix_pc_jsr, line, flags=re.I)
            out_lines.append(line)
        asm = os.path.join(work, 'tje.X68')
        open(asm, 'w', encoding='latin-1').write('\n'.join(out_lines))

        binp = os.path.join(work, 'tje.bin')
        lst = os.path.join(work, 'Listing.txt')
        cmd = [vasm, '-spaces', '-x', '-chklabels', '-nocase', '-maxerrors=50', '-m68000',
               '-L', lst, '-no-opt', '-Fbin', '-o', binp, asm]
        print('assembling with', vasm)
        r = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.isfile(binp):
            print(r.stdout[-3000:], r.stderr[-3000:])
            sys.exit('assembly failed')

        built = open(binp, 'rb').read()
        rom = open(rom_path, 'rb').read()
        if built != rom:
            diff = next((i for i in range(min(len(built), len(rom))) if built[i] != rom[i]), min(len(built), len(rom)))
            sys.exit(f'assembled disassembly does not match the ROM (first difference at {diff:06X}). '
                     'Check you have the (U) (REV 00) ROM and an unmodified disassembly.')
        print('vasm: disassembly assembles to a copy identical to the ROM')
    finally:
        if keep:
            print('work folder:', work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
