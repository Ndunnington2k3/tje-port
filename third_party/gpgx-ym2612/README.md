# YM2612 from Genesis Plus GX

`ym2612.c` / `ym2612.h` from [Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX)
(`core/sound/`, commit in `UPSTREAM_COMMIT`), MAME's fm.c with Eke-Eke's fixes. A lighter
FM chip than Nuked-OPN2 (one calculation per sample instead of per internal clock), for
PSP/3DS and as a sound option on PC.

License: `LICENSE.txt` (Genesis Plus GX): non-commercial; modified sources must be
distributed with the complete source code.

Changes for this port (marked "TJE port"):
- `ym2612_port.h` replaces Genesis Plus GX's `shared.h` (types only).
- `YM2612SetDivider(d)`: one output sample every d chip samples; oscillators, LFO,
  envelopes and timers still advance every chip sample, operators are calculated once
  per output sample.
