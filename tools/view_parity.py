#!/usr/bin/env python3
"""Check that the window size never changes how the game plays.

Runs tje_headless with the same input (random, from a fixed seed) and the same
starting point at several window sizes, then compares the game's whole work RAM
at the end. Any difference means something in the game can tell how big the
screen is, which must never happen (docs/RENDERER.md).

    python tools/view_parity.py --exe build/tje_headless --rom tje.bin \\
        --states "<disassembly>/Resources/Save States" --frames 3000
"""

import argparse
import concurrent.futures as cf
import glob
import os
import subprocess
import sys
import tempfile

WINDOWS = [None, '320x224', '1920x1080', '400x240', '480x272', '256x192', '5120x1440', '333x777']


def run(exe, rom, state, window, seed, frames, out):
    cmd = [exe, rom, '--frames', str(frames + 1), '--random', str(seed), '--dumpram', f'{frames}:{out}']
    if state:
        cmd += ['--state', state]
    else:
        cmd += ['--press', '600:5:S', '--press', '700:5:S', '--press', '800:5:S', '--press', '900:5:S']
    if window:
        cmd += ['--window', window]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if r.returncode != 0 and not os.path.exists(out):
        raise RuntimeError(f'{cmd}: exit {r.returncode}\n{r.stderr[-2000:]}')
    with open(out, 'rb') as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--states', help='folder of .gsx save states (default: only a fresh boot)')
    ap.add_argument('--frames', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=7)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()

    starts = [None]
    if args.states:
        starts += sorted(glob.glob(os.path.join(args.states, '*.gsx')))
    failures = 0
    with tempfile.TemporaryDirectory() as tmp, cf.ThreadPoolExecutor(args.jobs) as pool:
        jobs = {}
        for si, state in enumerate(starts):
            for wi, window in enumerate(WINDOWS):
                out = os.path.join(tmp, f's{si}_w{wi}.ram')
                jobs[(si, wi)] = pool.submit(run, args.exe, args.rom, state, window, args.seed, args.frames, out)
        for si, state in enumerate(starts):
            name = os.path.basename(state) if state else 'boot'
            ref = jobs[(si, 0)].result()
            bad = []
            for wi, window in enumerate(WINDOWS[1:], 1):
                ram = jobs[(si, wi)].result()
                if ram != ref:
                    diff = [i for i in range(len(ram)) if ram[i] != ref[i]]
                    bad.append(f'{window}: {len(diff)} bytes differ, first at $FF{diff[0]:04X}')
            if bad:
                failures += 1
                print(f'FAIL {name}: ' + '; '.join(bad))
            else:
                print(f'ok   {name}: identical at {len(WINDOWS)} window sizes')
    print('view_parity: ' + ('all identical' if not failures else f'{failures} start(s) differ'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
