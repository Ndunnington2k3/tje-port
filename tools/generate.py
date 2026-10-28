#!/usr/bin/env python3
"""Produce src/generated/ from your copy of the disassembly and ROM.

    python tools/generate.py --disasm <disassembly 'master' folder> --rom <ROM .bin> [--vasm <vasmm68k_mot>]

Steps:
 1. Copy Source/EntireSource.X68 to a work folder, normalising INCBIN paths and
    two PC-relative JSRs that newer vasm versions reject (bytes are unchanged).
 2. Assemble it with vasm to get a listing (addresses of every line).
 3. Check the assembled binary is byte-identical to your ROM.
 4. Run tools/m68k2c to write the C translation into src/generated/.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

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
    ap.add_argument('--vasm', help='path to vasmm68k_mot')
    ap.add_argument('--out', default=os.path.join(REPO, 'src', 'generated'))
    ap.add_argument('--keep', action='store_true', help='keep the work folder')
    args = ap.parse_args()

    src_dir = os.path.join(args.disasm, 'Source')
    entire = os.path.join(src_dir, 'EntireSource.X68')
    if not os.path.isfile(entire):
        sys.exit(f'{entire} not found')
    vasm = find_vasm(args.vasm)

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
        rom = open(args.rom, 'rb').read()
        if built != rom:
            diff = next((i for i in range(min(len(built), len(rom))) if built[i] != rom[i]), min(len(built), len(rom)))
            sys.exit(f'assembled disassembly does not match the ROM (first difference at {diff:06X}). '
                     'Check you have the (U) (REV 00) ROM and an unmodified disassembly.')
        print('disassembly matches ROM')

        for f in os.listdir(args.out) if os.path.isdir(args.out) else []:
            if f.startswith('gen_') and (f.endswith('.c') or f.endswith('.h')):
                os.remove(os.path.join(args.out, f))
        rc = subprocess.call([sys.executable, os.path.join(HERE, 'm68k2c', 'm68k2c.py'),
                              '--listing', lst, '--rom', args.rom, '--out', args.out])
        if rc != 0:
            sys.exit(rc)
        print('done: translated code is in', args.out)
    finally:
        if args.keep:
            print('work folder:', work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
