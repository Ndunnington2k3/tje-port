/* Per-instruction differential test: translated C vs a reference 68000 (Musashi). */
#ifndef TJE_ISA_TEST_H
#define TJE_ISA_TEST_H

#include <stdint.h>
#include "core/m68k_rt.h"

typedef void (*IsaFn)(Flags *f, int *taken);

typedef struct {
    uint32_t addr;     /* where the instruction lives in the ROM */
    uint8_t len;
    const char *op;    /* decoder mnemonic (e.g. "add", "bcc") */
    IsaFn fn;          /* the translator's C for this instruction */
    const char *text;
} IsaTest;

extern const IsaTest isa_tests[];
extern const unsigned isa_test_count;

#endif
