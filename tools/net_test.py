#!/usr/bin/env python3
"""Network play test: two (or three) headless copies of the game on this machine.

The host opens a room and starts a random world; the other copy joins it from the
Join list. Both then play with random inputs, in lockstep. Every 32 frames each
writes a checksum of the game's memory (TJE_NET_HASHLOG); the logs must agree.

Scenarios:
  plain    host + joiner
  loss     the same with 25% of packets lost and a longer input delay
  jitter   frames take up to JIT ms (environment, default 12) longer at random
  rejoin   the joiner quits part way (no goodbye: a lost connection); a new copy
           joins the game in progress
  outage   nothing gets through for 9 s part way (longer than the 5 s timeout):
           the joiner must get back in by itself
  mend     the joiner's memory is changed part way (a desync): the host must
           notice and mend it

  python tools/net_test.py --exe build/tje_headless --rom tje.bin [scenario ...]
"""
import argparse, os, subprocess, sys, tempfile, time

BOOT = ['--press', '900:3:S', '--press', '1050:3:S'] + \
       sum([['--press', '%d:3:D' % f] for f in (1200, 1210, 1220, 1230)], []) + ['--press', '1260:3:A']
HOST = BOOT + ['--press', '1420:3:A', '--press', '1600:3:A', '--press', '2200:3:A', '--press', '2500:3:A']
JOIN = BOOT + ['--press', '1400:3:D', '--press', '1420:3:A', '--press', '1800:3:A']


def start(exe, rom, name, args, env, d):
    e = dict(os.environ, TJE_NET_LOG='1', TJE_NET_HASHLOG=os.path.join(d, name + '.hash'), **env)
    log = open(os.path.join(d, name + '.log'), 'w')
    return subprocess.Popen([exe, rom, '--realtime', '--window', '480x272'] + args, env=e,
                            stdout=log, stderr=subprocess.STDOUT)


def hashes(d, name):
    try:
        return dict(l.split() for l in open(os.path.join(d, name + '.hash')))
    except FileNotFoundError:
        return {}


def compare(d, a, b, after=0):
    ha, hb = hashes(d, a), hashes(d, b)
    common = sorted((int(f) for f in ha if f in hb and int(f) > after))
    bad = [f for f in common if ha[str(f)] != hb[str(f)]]
    return len(common), bad


def log_has(d, name, text):
    return text in open(os.path.join(d, name + '.log')).read()


def run(sc, exe, rom, d):
    frames = 5000
    env = {}
    jargs = []
    if sc == 'loss':
        env = {'TJE_NET_LOSS': '25', 'TJE_NET_DELAY': '5'}
    if sc == 'jitter':
        env = {'TJE_JITTER': os.environ.get('JIT', '12')}
    if sc == 'rejoin':
        jargs = ['--frames', '3800']
    if sc == 'mend':
        jargs = ['--poke', '4000:FFA24C:7']     # (ToeJam's bucks)
    if sc == 'outage':
        env = {'TJE_NET_OUTAGE': '50000:9000'}
        frames = 7000
    h = start(exe, rom, 'host', HOST + ['--frames', str(frames + (1800 if sc == 'rejoin' else 0)), '--random', '3', '--random-from', '3500'], env, d)
    time.sleep(1)
    j = start(exe, rom, 'join', JOIN + ['--random', '5', '--random-from', '3500'] + (jargs if 'frames' in ' '.join(jargs) else ['--frames', str(frames)] + jargs), env, d)
    j2 = None
    if sc == 'rejoin':
        j.wait()
        time.sleep(8)                         # (the host notices after 5 s)
        j2 = start(exe, rom, 'join2', JOIN + ['--frames', '4000', '--random', '7', '--random-from', '2500'], env, d)
    for p in (h, j, j2):
        if p:
            p.wait()
    ok = True
    if sc == 'outage':
        n, bad = compare(d, 'host', 'join', 0)
        back = log_has(d, 'join', 'restarting from frame') and log_has(d, 'join', 'lost ToeJam') and \
            not log_has(d, 'join', 'Lost the connection')
        print('  %d checksums, %d differ; back in by itself: %s' % (n, len(bad), back))
        ok = n > 40 and not bad and back
    elif sc == 'rejoin':
        n, bad = compare(d, 'host', 'join', 0)
        n2, bad2 = compare(d, 'host', 'join2', 0)
        print('  first joiner: %d checksums, %d differ; second: %d, %d differ' % (n, len(bad), n2, len(bad2)))
        ok = n > 20 and n2 > 10 and not bad and not bad2 and log_has(d, 'host', 'package 1')
    else:
        n, bad = compare(d, 'host', 'join', 0)
        mended = log_has(d, 'host', 'checksums differ')
        if sc == 'mend':
            after = [f for f in bad]
            last_bad = max(bad) if bad else 0
            n_after, bad_after = compare(d, 'host', 'join', last_bad)
            print('  %d checksums, %d differ (frames %s), mended: %s, %d agree after' %
                  (n, len(bad), bad[:4], mended, n_after))
            ok = mended and n_after > 20 and not bad_after
        else:
            print('  %d checksums, %d differ' % (n, len(bad)))
            ok = n > 40 and not bad and not mended
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('scenarios', nargs='*', default=['plain', 'loss', 'jitter', 'rejoin', 'outage', 'mend'])
    a = ap.parse_args()
    fails = 0
    for sc in a.scenarios:
        d = tempfile.mkdtemp(prefix='tje_net_' + sc + '_')
        print('%s (%s)' % (sc, d))
        ok = run(sc, os.path.abspath(a.exe), os.path.abspath(a.rom), d)
        print('  ' + ('ok' if ok else 'FAILED'))
        fails += not ok
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
