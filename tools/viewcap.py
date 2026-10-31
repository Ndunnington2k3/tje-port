#!/usr/bin/env python3
"""Read a view capture (tje_captureN.bin, written with --view-log / F12).

    viewcap.py CAPTURE.bin OUTDIR [first last]

Writes each frame as OUTDIR/fNNNN.png and its RAM as OUTDIR/fNNNN.ram, and
prints one line per frame (size, camera, gameview or not, snapshot age)."""
import os, struct, sys
import numpy as np
from PIL import Image

def frames(path):
    with open(path, 'rb') as f:
        assert f.read(8) == b'TJEVCAP1'
        (n,) = struct.unpack('<I', f.read(4))
        for _ in range(n):
            fr, w, h, cx, cy, isv, age, _z, mlo, mhi = struct.unpack('<IHHhhBBHII', f.read(24))
            px = np.frombuffer(f.read(w * h * 2), '<u2').reshape(h, w)
            ram = f.read(65536)
            vram = f.read(65536)
            rgb = np.stack([(px >> 10) & 31, (px >> 5) & 31, px & 31], -1).astype(np.uint8) * 8
            yield dict(frame=fr, w=w, h=h, cam=(cx, cy), view=isv, age=age, mask=mlo | (mhi << 32),
                       rgb=rgb, ram=ram, vram=vram)

if __name__ == '__main__':
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for fr in frames(sys.argv[1]):
        print(fr['frame'], fr['w'], fr['h'], 'cam', fr['cam'], 'view', fr['view'], 'age', fr['age'],
              'mask %010X' % fr['mask'])
        Image.fromarray(fr['rgb']).save(os.path.join(out, 'f%05d.png' % fr['frame']))
        open(os.path.join(out, 'f%05d.ram' % fr['frame']), 'wb').write(fr['ram'])
