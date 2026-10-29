/*
 * Tiny byte serializer for machine snapshots and save files.
 * Everything is stored little-endian with explicit sizes, so a file written on
 * one platform reads back on another (Windows, PSP, 3DS...).
 *
 * Writing: Ser s = {buf, 0, cap, 0}; ser_u32(&s, x)...  (buf may be NULL to measure)
 * Reading: Ser s = {buf, 0, len, 0}; x = des_u32(&s)...  s.err is set on overrun.
 */
#ifndef TJE_SER_H
#define TJE_SER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t pos, cap;
    int err;
} Ser;

static inline void ser_bytes(Ser *s, const void *p, size_t n)
{
    if (s->buf) {
        if (s->pos + n > s->cap) { s->err = 1; return; }
        memcpy(s->buf + s->pos, p, n);
    }
    s->pos += n;
}
static inline void ser_u8(Ser *s, uint32_t v) { uint8_t b = (uint8_t)v; ser_bytes(s, &b, 1); }
static inline void ser_u16(Ser *s, uint32_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; ser_bytes(s, b, 2); }
static inline void ser_u32(Ser *s, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    ser_bytes(s, b, 4);
}

static inline void des_bytes(Ser *s, void *p, size_t n)
{
    if (s->pos + n > s->cap) { s->err = 1; memset(p, 0, n); return; }
    memcpy(p, s->buf + s->pos, n);
    s->pos += n;
}
static inline uint32_t des_u8(Ser *s) { uint8_t b = 0; des_bytes(s, &b, 1); return b; }
static inline uint32_t des_u16(Ser *s) { uint8_t b[2]; des_bytes(s, b, 2); return b[0] | ((uint32_t)b[1] << 8); }
static inline uint32_t des_u32(Ser *s)
{
    uint8_t b[4];
    des_bytes(s, b, 4);
    return b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

#endif
