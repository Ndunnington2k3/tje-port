#include "m68k_rt.h"
#include "genesis.h"

#include <stdio.h>
#include <stdlib.h>

M68KState R;
uint8_t g_ram[0x10000];
const uint8_t *g_rom;
uint32_t g_rom_size;

static int find_func(uint32_t addr)
{
    int lo = 0, hi = (int)rt_func_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t a = rt_func_table[mid].addr;
        if (a == addr) return mid;
        if (a < addr) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

const char *rt_func_name(uint32_t addr)
{
    int i = find_func(addr & 0xFFFFFFu);
    return i >= 0 ? rt_func_table[i].name : "?";
}

void rt_call(uint32_t addr)
{
    int i = find_func(addr & 0xFFFFFFu);
    if (i < 0) {
        genesis_fatal("call to untranslated address %06X (return address %06X)", addr, rd32(R.a[7]));
        return;
    }
    rt_func_table[i].fn();
}

void rt_bad_jump(uint32_t target, uint32_t at)
{
    genesis_fatal("jump table at %06X went to unknown target %06X", at, target);
}

uint32_t rt_get_sr(const Flags *f)
{
    return (R.sr & 0xFF00u) | get_ccr(f);
}

void rt_set_sr(Flags *f, uint32_t v)
{
    R.sr = v & 0xA700u;
    set_ccr(f, v);
    genesis_check_interrupts();
}

void rt_rte(void)
{
    uint32_t sr = pop16();
    (void)pop32(); /* return PC - control returns to the interrupted C function */
    R.sr = sr & 0xA700u;
    genesis_check_interrupts();
}

void rt_exception(const char *what, uint32_t at)
{
    genesis_log("68k exception: %s at %06X (in %s)", what, at, "translated code");
}

void rt_spcheck_fail(uint32_t expected, uint32_t at)
{
    static int reported;
    if (reported < 20) {
        reported++;
        genesis_log("stack mismatch after call at %06X: SP %06X, expected %06X", at, R.a[7], expected);
    }
}

void rt_service(void)
{
    genesis_run_events();
}

#ifdef TJE_COVERAGE
static uint32_t *coverage_counts;

void rt_trace(uint32_t addr)
{
    int i;
    if (!coverage_counts) coverage_counts = (uint32_t *)calloc(rt_func_count, sizeof *coverage_counts);
    i = find_func(addr);
    if (i >= 0 && coverage_counts) coverage_counts[i]++;
}
#endif

/* Write "address name count" for every translated function (count 0 = never ran).
 * Only meaningful in TJE_COVERAGE builds; otherwise writes nothing. */
void rt_coverage_write(const char *path)
{
#ifdef TJE_COVERAGE
    FILE *f;
    unsigned i;
    if (!path || !coverage_counts) return;
    f = fopen(path, "w");
    if (!f) return;
    for (i = 0; i < rt_func_count; i++)
        fprintf(f, "%06X %s %u\n", rt_func_table[i].addr, rt_func_table[i].name, coverage_counts[i]);
    fclose(f);
#else
    (void)path;
#endif
}
