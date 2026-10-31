/*
 * Runtime support for code translated from 68000 by tools/m68k2c.
 *
 * The translated game code reads and writes CPU registers in R, accesses the
 * Genesis memory map through rd8/rd16/rd32 and wr8/wr16/wr32, and keeps the
 * condition flags in a local Flags struct per function.
 */
#ifndef TJE_M68K_RT_H
#define TJE_M68K_RT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t usp;
    uint32_t sr;          /* system byte (S, interrupt mask); CCR lives in the caller's Flags */
    uint32_t cycles;      /* 68000 cycles elapsed (wraps) */
    uint32_t next_event;  /* cycle count of the next scanline event */
} M68KState;

typedef struct {
    uint8_t x, n, z, v, c;
} Flags;

typedef void (*RtFn)(void);
typedef struct {
    uint32_t addr;
    RtFn fn;
    const char *name;
} RtFunc;

extern M68KState R;
extern uint8_t g_ram[0x10000];
extern const uint8_t *g_rom;
extern uint32_t g_rom_size;

/* Generated tables (src/generated/gen_table.c) */
extern const RtFunc rt_func_table[];
extern const unsigned rt_func_count;
extern const uint32_t rt_reset_pc, rt_reset_sp, rt_vector_hint, rt_vector_vint;
extern const uint32_t rt_entry_ingameloop;

/* I/O space (everything that is not ROM or RAM) - src/core/io.c */
uint32_t io_rd8(uint32_t a);
uint32_t io_rd16(uint32_t a);
void io_wr8(uint32_t a, uint32_t v);
void io_wr16(uint32_t a, uint32_t v);

/* Runtime services - src/core/m68k_rt.c */
void rt_call(uint32_t addr);
void rt_bad_jump(uint32_t target, uint32_t at);
void rt_rte(void);
uint32_t rt_get_sr(const Flags *f);
void rt_set_sr(Flags *f, uint32_t v);
void rt_exception(const char *what, uint32_t at);
void rt_service(void);
void rt_spcheck_fail(uint32_t expected, uint32_t at);
const char *rt_func_name(uint32_t addr);

/* ---- memory ------------------------------------------------------------- */
static inline uint32_t rd8(uint32_t a)
{
    a &= 0xFFFFFFu;
    if (a >= 0xE00000u) return g_ram[a & 0xFFFFu];
    if (a < 0x400000u) return a < g_rom_size ? g_rom[a] : 0xFFu;
    return io_rd8(a);
}

static inline uint32_t rd16(uint32_t a)
{
    a &= 0xFFFFFFu;
    if (a >= 0xE00000u) {
        /* second byte index wraps so an (invalid) odd access at $FFFFFF stays in bounds */
        return ((uint32_t)g_ram[a & 0xFFFFu] << 8) | g_ram[(a + 1) & 0xFFFFu];
    }
    if (a < 0x400000u) {
        if (a + 1 < g_rom_size) return ((uint32_t)g_rom[a] << 8) | g_rom[a + 1];
        return 0xFFFFu;
    }
    return io_rd16(a);
}

static inline uint32_t rd32(uint32_t a)
{
    a &= 0xFFFFFFu;
    if (a >= 0xE00000u && (a & 0xFFFFu) <= 0xFFFCu) {
        const uint8_t *p = g_ram + (a & 0xFFFFu);
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    return (rd16(a) << 16) | rd16(a + 2);
}

#ifdef TJE_SAT_TRACE
/* (audit builds) who writes the sprite table copy at $FFB65A */
void sat_trace_write(uint32_t a);
#define SAT_TRACE(a) do { if (((a) & 0xFFFFu) >= 0xB65Au && ((a) & 0xFFFFu) < 0xB8DAu) sat_trace_write(a); } while (0)
#else
#define SAT_TRACE(a) do { } while (0)
#endif

static inline void wr8(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFFu;
    if (a >= 0xE00000u) { SAT_TRACE(a); g_ram[a & 0xFFFFu] = (uint8_t)v; return; }
    if (a < 0x400000u) return; /* ROM */
    io_wr8(a, v & 0xFFu);
}

static inline void wr16(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFFu;
    if (a >= 0xE00000u) {
        SAT_TRACE(a);
        g_ram[a & 0xFFFFu] = (uint8_t)(v >> 8);
        g_ram[(a + 1) & 0xFFFFu] = (uint8_t)v;
        return;
    }
    if (a < 0x400000u) return;
    io_wr16(a, v & 0xFFFFu);
}

static inline void wr32(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFFu;
    if (a >= 0xE00000u && (a & 0xFFFFu) <= 0xFFFCu) {
        uint8_t *p = g_ram + (a & 0xFFFFu);
        SAT_TRACE(a);
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
        return;
    }
    wr16(a, v >> 16);
    wr16(a + 2, v & 0xFFFFu);
}

static inline void push32(uint32_t v) { R.a[7] -= 4; wr32(R.a[7], v); }
static inline void push16(uint32_t v) { R.a[7] -= 2; wr16(R.a[7], v); }
static inline uint32_t pop32(void) { uint32_t v = rd32(R.a[7]); R.a[7] += 4; return v; }
static inline uint32_t pop16(void) { uint32_t v = rd16(R.a[7]); R.a[7] += 2; return v; }

/* ---- timing ------------------------------------------------------------- */
#define CY(n) (R.cycles += (n))
#define RT_POLL() do { if ((int32_t)(R.cycles - R.next_event) >= 0) rt_service(); } while (0)

#ifndef TJE_NO_SPCHECK
#define RT_SPCHECK(sp0, at) do { if (R.a[7] != (sp0)) rt_spcheck_fail((sp0), (at)); } while (0)
#else
#define RT_SPCHECK(sp0, at) do { (void)(sp0); } while (0)
#endif

/* Coverage tracing (build with -DTJE_COVERAGE): records which translated functions ran. */
#ifdef TJE_COVERAGE
void rt_trace(uint32_t addr);
#define RT_TRACE(addr) rt_trace(addr)
#else
#define RT_TRACE(addr) ((void)0)
#endif
void rt_coverage_write(const char *path);

/* ---- flags & ALU -------------------------------------------------------- */
static inline uint32_t sz_mask(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline uint32_t sz_sign(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }
static inline int sz_bits(int sz) { return sz * 8; }

static inline uint32_t get_ccr(const Flags *f)
{
    return ((uint32_t)f->x << 4) | ((uint32_t)f->n << 3) | ((uint32_t)f->z << 2) | ((uint32_t)f->v << 1) | f->c;
}

static inline void set_ccr(Flags *f, uint32_t v)
{
    f->x = (v >> 4) & 1; f->n = (v >> 3) & 1; f->z = (v >> 2) & 1; f->v = (v >> 1) & 1; f->c = v & 1;
}

static inline void logic(Flags *f, uint32_t r, int sz)
{
    r &= sz_mask(sz);
    f->n = (r & sz_sign(sz)) != 0;
    f->z = r == 0;
    f->v = 0;
    f->c = 0;
}

static inline void nz(Flags *f, uint32_t r, int sz)
{
    f->n = (r & sz_sign(sz)) != 0;
    f->z = r == 0;
}

static inline uint32_t op_add(Flags *f, uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint64_t t;
    uint32_t r;
    s &= m; d &= m;
    t = (uint64_t)d + s;
    r = (uint32_t)t & m;
    f->c = f->x = t > m;
    f->v = (((s ^ r) & (d ^ r)) & sz_sign(sz)) != 0;
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_sub(Flags *f, uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint32_t r;
    s &= m; d &= m;
    r = (d - s) & m;
    f->c = f->x = s > d;
    f->v = (((s ^ d) & (r ^ d)) & sz_sign(sz)) != 0;
    nz(f, r, sz);
    return r;
}

static inline void op_cmp(Flags *f, uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint32_t r;
    s &= m; d &= m;
    r = (d - s) & m;
    f->c = s > d;
    f->v = (((s ^ d) & (r ^ d)) & sz_sign(sz)) != 0;
    nz(f, r, sz);
}

static inline uint32_t op_neg(Flags *f, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint32_t r;
    d &= m;
    r = (0u - d) & m;
    f->c = f->x = d != 0;
    f->v = ((d & r) & sz_sign(sz)) != 0;
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_negx(Flags *f, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint32_t r;
    d &= m;
    r = (0u - d - f->x) & m;
    f->c = f->x = (d != 0) || f->x;
    f->v = ((d & r) & sz_sign(sz)) != 0;
    f->n = (r & sz_sign(sz)) != 0;
    if (r) f->z = 0;
    return r;
}

static inline uint32_t op_addx(Flags *f, uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint64_t t;
    uint32_t r;
    s &= m; d &= m;
    t = (uint64_t)d + s + f->x;
    r = (uint32_t)t & m;
    f->c = f->x = t > m;
    f->v = (((s ^ r) & (d ^ r)) & sz_sign(sz)) != 0;
    f->n = (r & sz_sign(sz)) != 0;
    if (r) f->z = 0;
    return r;
}

static inline uint32_t op_subx(Flags *f, uint32_t s, uint32_t d, int sz)
{
    uint32_t m = sz_mask(sz);
    uint32_t r;
    s &= m; d &= m;
    r = (d - s - f->x) & m;
    f->c = f->x = ((uint64_t)s + f->x) > d;
    f->v = (((s ^ d) & (r ^ d)) & sz_sign(sz)) != 0;
    f->n = (r & sz_sign(sz)) != 0;
    if (r) f->z = 0;
    return r;
}

static inline uint32_t op_abcd(Flags *f, uint32_t s, uint32_t d)
{
    uint32_t r = (s & 0x0F) + (d & 0x0F) + f->x;
    if (r > 9) r += 6;
    r += (s & 0xF0) + (d & 0xF0);
    f->c = f->x = r > 0x99;
    if (f->c) r -= 0xA0;
    r &= 0xFF;
    f->n = (r & 0x80) != 0;
    if (r) f->z = 0;
    return r;
}

static inline uint32_t op_sbcd(Flags *f, uint32_t s, uint32_t d)
{
    uint32_t r = (d & 0x0F) - (s & 0x0F) - f->x;
    if (r > 9) r -= 6;
    r += (d & 0xF0) - (s & 0xF0);
    f->c = f->x = r > 0x99;
    if (f->c) r += 0xA0;
    r &= 0xFF;
    f->n = (r & 0x80) != 0;
    if (r) f->z = 0;
    return r;
}

/* Shifts and rotates. c is the shift count (0-63). */
static inline uint32_t op_asl(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t r;
    d &= m;
    if (c == 0) { f->c = 0; f->v = 0; nz(f, d, sz); return d; }
    if (c < (uint32_t)b) {
        uint32_t top = (c + 1 >= (uint32_t)b) ? m : (m & ~(m >> (c + 1)));
        r = (uint32_t)(((uint64_t)d << c) & m);
        f->c = f->x = (d >> (b - c)) & 1;
        f->v = !((d & top) == 0 || (d & top) == top);
    } else {
        r = 0;
        f->c = f->x = (c == (uint32_t)b) ? (d & 1) : 0;
        f->v = d != 0;
    }
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_asr(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t sign = d & sz_sign(sz);
    uint32_t r;
    d &= m;
    f->v = 0;
    if (c == 0) { f->c = 0; nz(f, d, sz); return d; }
    if (c < (uint32_t)b) {
        r = d >> c;
        if (sign) r |= m & ~(m >> c);
        f->c = f->x = (d >> (c - 1)) & 1;
    } else {
        r = sign ? m : 0;
        f->c = f->x = sign != 0;
    }
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_lsl(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t r;
    d &= m;
    f->v = 0;
    if (c == 0) { f->c = 0; nz(f, d, sz); return d; }
    if (c <= (uint32_t)b) {
        r = (uint32_t)(((uint64_t)d << c) & m);
        f->c = f->x = (d >> (b - c)) & 1;
    } else {
        r = 0;
        f->c = f->x = 0;
    }
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_lsr(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t r;
    d &= m;
    f->v = 0;
    if (c == 0) { f->c = 0; nz(f, d, sz); return d; }
    if (c <= (uint32_t)b) {
        r = (c == 32) ? 0 : d >> c;
        f->c = f->x = (d >> (c - 1)) & 1;
    } else {
        r = 0;
        f->c = f->x = 0;
    }
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_rol(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t r, k;
    d &= m;
    f->v = 0;
    if (c == 0) { f->c = 0; nz(f, d, sz); return d; }
    k = c % (uint32_t)b;
    r = k ? (uint32_t)((((uint64_t)d << k) | (d >> (b - k))) & m) : d;
    f->c = r & 1;
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_ror(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t r, k;
    d &= m;
    f->v = 0;
    if (c == 0) { f->c = 0; nz(f, d, sz); return d; }
    k = c % (uint32_t)b;
    r = k ? (uint32_t)(((d >> k) | ((uint64_t)d << (b - k))) & m) : d;
    f->c = (r & sz_sign(sz)) != 0;
    nz(f, r, sz);
    return r;
}

static inline uint32_t op_roxl(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t i;
    d &= m;
    f->v = 0;
    c %= (uint32_t)(b + 1);
    for (i = 0; i < c; i++) {
        uint32_t out = (d & sz_sign(sz)) != 0;
        d = ((d << 1) | f->x) & m;
        f->x = (uint8_t)out;
    }
    f->c = f->x;
    nz(f, d, sz);
    return d;
}

static inline uint32_t op_roxr(Flags *f, uint32_t d, uint32_t c, int sz)
{
    uint32_t m = sz_mask(sz);
    int b = sz_bits(sz);
    uint32_t i;
    d &= m;
    f->v = 0;
    c %= (uint32_t)(b + 1);
    for (i = 0; i < c; i++) {
        uint32_t out = d & 1;
        d = (d >> 1) | (f->x ? sz_sign(sz) : 0);
        f->x = (uint8_t)out;
    }
    f->c = f->x;
    nz(f, d, sz);
    return d;
}

static inline void op_divu(Flags *f, uint32_t *dreg, uint32_t s, uint32_t at)
{
    uint32_t q, r;
    s &= 0xFFFFu;
    if (s == 0) { rt_exception("divide by zero", at); return; }
    q = *dreg / s;
    r = *dreg % s;
    f->c = 0;
    if (q > 0xFFFFu) { f->v = 1; return; }
    *dreg = (r << 16) | q;
    f->v = 0;
    f->n = (q & 0x8000u) != 0;
    f->z = q == 0;
}

static inline void op_divs(Flags *f, uint32_t *dreg, uint32_t s, uint32_t at)
{
    int32_t dv = (int32_t)*dreg, sv = (int16_t)s, q, r;
    if (sv == 0) { rt_exception("divide by zero", at); return; }
    f->c = 0;
    if (dv == (int32_t)0x80000000 && sv == -1) { f->v = 1; return; }
    q = dv / sv;
    r = dv % sv;
    if (q < -32768 || q > 32767) { f->v = 1; return; }
    *dreg = ((uint32_t)(r & 0xFFFF) << 16) | ((uint32_t)q & 0xFFFFu);
    f->v = 0;
    f->n = q < 0;
    f->z = q == 0;
}

static inline void movep_store(uint32_t v, uint32_t a, int sz)
{
    if (sz == 4) { wr8(a, v >> 24); wr8(a + 2, v >> 16); wr8(a + 4, v >> 8); wr8(a + 6, v); }
    else { wr8(a, v >> 8); wr8(a + 2, v); }
}

static inline void movep_load(uint32_t *d, uint32_t a, int sz)
{
    if (sz == 4) *d = (rd8(a) << 24) | (rd8(a + 2) << 16) | (rd8(a + 4) << 8) | rd8(a + 6);
    else *d = (*d & 0xFFFF0000u) | (rd8(a) << 8) | rd8(a + 2);
}

#ifdef __cplusplus
}
#endif

#endif
