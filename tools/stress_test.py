#!/usr/bin/env python3
"""Stress-test the translated game with the headless build.

Runs tje_headless from a fresh boot and from every Gens save state in a folder,
with random input and (optionally) forced elevator rides, in parallel. Reports
any fatal error, bad jump or stack mismatch, and - with a coverage build
(-DTJE_COVERAGE) - which translated functions never ran.

    python tools/stress_test.py --exe build_cov/tje_headless --rom tje.bin \\
        --states "<disassembly>/Resources/Save States" --frames 20000 --seeds 3
"""

import argparse
import concurrent.futures as cf
import glob
import os
import re
import subprocess
import sys
import tempfile


def run_one(exe, rom, state, seed, frames, elevator, workdir, audio):
    tag = (os.path.splitext(os.path.basename(state))[0] if state else 'boot') + f' seed{seed}'
    cov = os.path.join(workdir, re.sub(r'[^A-Za-z0-9]+', '_', tag) + '.cov')
    cmd = [exe, rom, '--frames', str(frames), '--random', str(seed)]
    if state:
        cmd += ['--state', state]
    else:
        cmd += ['--press', '600:5:S', '--press', '700:5:S', '--press', '800:5:S', '--press', '900:5:S']
    if elevator:
        cmd += ['--elevator', str(elevator)]
    if audio:
        cmd += ['--audio']
    env = dict(os.environ, TJE_COVERAGE_OUT=cov)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800, env=env)
        out, rc = r.stderr, r.returncode
    except subprocess.TimeoutExpired:
        out, rc = 'TIMEOUT (game hung?)', -1
    problems = [l for l in out.splitlines()
                if 'FATAL' in l or 'stack mismatch' in l or 'exception' in l or 'TIMEOUT' in l]
    levels = sorted({int(m) for m in re.findall(r'on level (\d+)', out)})
    return tag, rc, problems, levels, cov


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--states', help='folder of .gsx save states')
    ap.add_argument('--frames', type=int, default=20000)
    ap.add_argument('--seeds', type=int, default=2)
    ap.add_argument('--elevator', type=int, default=1500, help='force an elevator ride every N frames (0 = off)')
    ap.add_argument('--audio', action='store_true', help='emulate sound too (Z80 + FM + PSG)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    ap.add_argument('--coverage-out', help='write merged coverage here')
    args = ap.parse_args()

    states = [None]
    if args.states:
        states += sorted(glob.glob(os.path.join(args.states, '*.gs*')),
                         key=lambda p: [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', p)])
    work = tempfile.mkdtemp(prefix='tje_stress_')
    jobs = [(args.exe, args.rom, st, seed, args.frames, args.elevator, work, args.audio)
            for st in states for seed in range(1, args.seeds + 1)]
    failures = 0
    merged = {}
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for tag, rc, problems, levels, cov in ex.map(lambda j: run_one(*j), jobs):
            status = 'ok' if rc == 0 and not problems else 'FAIL'
            if status == 'FAIL':
                failures += 1
            print(f'{status:4s} {tag:34s} levels {levels}')
            for p in problems[:5]:
                print('       ', p)
            if os.path.exists(cov):
                for line in open(cov):
                    a, name, n = line.split()
                    merged.setdefault(a, [name, 0])[1] += int(n)
    if merged:
        ran = sum(1 for v in merged.values() if v[1])
        print(f'coverage: {ran}/{len(merged)} translated functions ran')
        if args.coverage_out:
            with open(args.coverage_out, 'w') as f:
                for a in sorted(merged):
                    f.write(f'{a} {merged[a][0]} {merged[a][1]}\n')
    print(f'{len(jobs) - failures}/{len(jobs)} runs passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
