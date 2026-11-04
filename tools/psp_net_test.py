#!/usr/bin/env python3
"""Network play between two emulated PSPs (PPSSPP headless, ad hoc).

Each PSP has its own memory stick (EBOOT.PBP, tje.bin and nettest.on in
PSP/GAME/TJE); with nettest.on the game logs the network there and writes its
memory checksums to net.hash. PSP 1 runs the ad hoc server and hosts a room; PSP 2
joins it from the Join list. Both then play; the checksum logs must agree.

  python tools/psp_net_test.py --ppsspp PPSSPPHeadless --eboot EBOOT.PBP --rom tje.bin
         --dir WORK [--seconds 90] [--shots] [--vlcheck] [--loss PCT] [--delay MS]
"""
import argparse, os, shutil, subprocess, sys

# PSP buttons
START, UP, RIGHT, DOWN, LEFT = 0x8, 0x10, 0x20, 0x40, 0x80
TRI, CIRCLE, CROSS, SQUARE = 0x1000, 0x2000, 0x4000, 0x8000


def presses(lst):
    return ','.join('%.2f:0x%x:0.1' % (t, m) for t, m in lst)


# the title screen from ~10 s: Start there starts the intro, Start again skips it to
# the Who screen; Network Play is its 5th row
TO_NET = [(13, START), (14.5, START), (16.5, DOWN), (16.8, DOWN), (17.1, DOWN), (17.4, DOWN), (17.8, CROSS)]
SHIFT = 0.0   # (all the times above and below move with the title screen's)
HOST = TO_NET + [(19.5, CROSS), (21.5, CROSS),            # Host Game, Open Room
                 (32.0, CROSS),                             # lobby: New game (the joiner is in)
                 (35.0, CROSS)]                             # What: random world
JOIN = TO_NET + [(19.2, DOWN), (19.5, CROSS),             # Join Game
                 (27.0, CROSS)]                             # the room

# then wander: both hold directions now and then
def wander(start, end, seed):
    import random
    r = random.Random(seed)
    out, t = [], start
    while t < end:
        out.append((t, r.choice([UP, DOWN, LEFT, RIGHT, UP | RIGHT, DOWN | LEFT])))
        t += r.uniform(0.3, 1.5)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ppsspp', required=True)
    ap.add_argument('--eboot', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--dir', required=True)
    ap.add_argument('--seconds', type=float, default=90)
    ap.add_argument('--shots', action='store_true')
    ap.add_argument('--loss', type=int, default=0)
    ap.add_argument('--delay', type=int, default=0)
    ap.add_argument('--vlcheck', action='store_true', help='each GE picture compared with the list drawn in software')
    ap.add_argument('--perf', action='store_true', help='perf.log on both (where the time goes)')
    a = ap.parse_args()

    os.makedirs(a.dir, exist_ok=True)
    sync = os.path.join(a.dir, 'sync')
    with open(sync, 'wb') as f:
        f.write(b'\0' * 4096)
    procs = []
    for n, name, script in ((1, 'host', HOST), (2, 'join', JOIN)):
        game = os.path.join(a.dir, name, 'PSP', 'GAME', 'TJE')
        os.makedirs(game, exist_ok=True)
        shutil.copy(a.eboot, os.path.join(game, 'EBOOT.PBP'))
        shutil.copy(a.rom, os.path.join(game, 'tje.bin'))
        open(os.path.join(game, 'nettest.on'), 'w').close()
        if a.vlcheck:
            open(os.path.join(game, 'vlcheck.on'), 'w').close()
        elif os.path.exists(os.path.join(game, 'vlcheck.on')):
            os.remove(os.path.join(game, 'vlcheck.on'))
        if a.perf:
            open(os.path.join(game, 'perf.on'), 'w').close()
        for old in ('net.hash', 'perf.log'):
            if os.path.exists(os.path.join(game, old)):
                os.remove(os.path.join(game, old))
        script = [(t + SHIFT, m) for t, m in script] + wander(40 + SHIFT, a.seconds - 1, n)
        args = [a.ppsspp, '--memstick=' + os.path.join(a.dir, name), '--instance', str(n),
                '--sync-file', sync, '--sync-count', '2', '--nickname', name.upper(),
                '--timeout-emulated=%g' % a.seconds, '--press=' + presses(script)]
        if n == 1:
            args.append('--adhoc-server')
        if a.loss:
            args.append('--adhoc-loss=%d' % a.loss)
        if a.delay:
            args.append('--adhoc-delay=%d' % a.delay)
        if a.shots:
            ts = [12, 14, 15.5, 16, 18.5, 20.5, 23, 28, 31, 34, 37, 45, a.seconds - 2]
            args.append('--screenshot-at=' + ','.join('%g:%s' % (t, os.path.join(a.dir, '%s_%04.1f.png' % (name, t)))
                                                      for t in ts))
        args.append(os.path.join(game, 'EBOOT.PBP'))
        log = open(os.path.join(a.dir, name + '.log'), 'w')
        procs.append(subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT,
                                      env=dict(os.environ, PPSSPP_NO_RELAY='1')))
    for p in procs:
        p.wait()

    def hashes(name):
        try:
            return dict(l.split() for l in open(os.path.join(a.dir, name, 'PSP', 'GAME', 'TJE', 'net.hash')))
        except FileNotFoundError:
            return {}
    ha, hb = hashes('host'), hashes('join')
    common = sorted(int(f) for f in ha if f in hb)
    bad = [f for f in common if ha[str(f)] != hb[str(f)]]
    print('checksums: host %d, joiner %d, compared %d, different %d' % (len(ha), len(hb), len(common), len(bad)))
    ok = len(common) >= 20 and not bad
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
