/* The few definitions ym2612.c takes from Genesis Plus GX's shared.h (TJE port). */
#ifndef YM2612_PORT_H
#define YM2612_PORT_H
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef uint8_t UINT8; typedef int8_t INT8; typedef uint16_t UINT16; typedef int16_t INT16;
typedef uint32_t UINT32; typedef int32_t INT32; typedef uint8_t uint8;
#define INLINE static inline
/* (Genesis Plus GX state.h: context save/load) */
#define load_param(param, size) memcpy(param, &state[bufferptr], size); bufferptr += size;
#define save_param(param, size) memcpy(&state[bufferptr], param, size); bufferptr += size;
#include "ym2612.h"
#endif
