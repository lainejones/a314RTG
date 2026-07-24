/*
 * mmutest.c - CLI validation harness for the mmu.library dirty-page path
 * used by a314rtg.card 1.1 (GetPageUsedModified M-bit tracking).
 *
 * Runs the exact same call sequence the card's diff process uses, against an
 * AllocMem'd test buffer, printing every step. PASS at the end means the MMU
 * fast path will engage on this machine; any FAIL means the card silently
 * falls back to full scanning (still correct, just not faster).
 *
 * Build: make mmutest   Run on the Amiga:  PiDisk:rtg/mmutest
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>

#define MAPP_USED        (1UL << 3)
#define MAPP_MODIFIED    (1UL << 4)
#define MAPP_SINGLEPAGE  (1UL << 12)

static struct Library *MMUBase = NULL;

static APTR mmu_CurrentContext(void)
{
    register APTR res  __asm("d0");
    register APTR task __asm("a1") = NULL;
    register APTR lib  __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -240(%%a6)"
        : "=r"(res), "+r"(task), "+r"(lib) : : "d1", "a0", "cc", "memory");
    return res;
}

static ULONG mmu_GetPageSize(APTR ctx)
{
    register ULONG res __asm("d0");
    register APTR  c   __asm("a0") = ctx;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -48(%%a6)"
        : "=r"(res), "+r"(c), "+r"(lib) : : "d1", "a1", "cc", "memory");
    return res;
}

static ULONG mmu_GetPageUsedModified(APTR ctx, ULONG lower)
{
    register ULONG res __asm("d0");
    register APTR  c   __asm("a0") = ctx;
    register ULONG lo  __asm("a1") = lower;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -378(%%a6)"
        : "=r"(res), "+r"(c), "+r"(lo), "+r"(lib) : : "d1", "cc", "memory");
    return res;
}

static BOOL mmu_SetPropertiesA(APTR ctx, ULONG flags, ULONG mask,
                               ULONG lower, ULONG size, APTR tags)
{
    register ULONG res __asm("d0") = size;
    register APTR  c   __asm("a0") = ctx;
    register ULONG fl  __asm("d1") = flags;
    register ULONG ma  __asm("d2") = mask;
    register ULONG lo  __asm("a1") = lower;
    register APTR  tg  __asm("a2") = tags;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -84(%%a6)"
        : "+r"(res), "+r"(c), "+r"(fl), "+r"(lo), "+r"(lib)
        : "r"(ma), "r"(tg) : "cc", "memory");
    return (BOOL)res;
}

static BOOL mmu_RebuildTree(APTR ctx)
{
    register ULONG res __asm("d0");
    register APTR  c   __asm("a0") = ctx;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -96(%%a6)"
        : "=r"(res), "+r"(c), "+r"(lib) : : "d1", "a1", "cc", "memory");
    return (BOOL)res;
}

static ULONG mmu_GetPagePropertiesA(APTR ctx, ULONG lower)   /* LVO -0x06c */
{
    register ULONG res __asm("d0");
    register APTR  c   __asm("a0") = ctx;
    register ULONG lo  __asm("a1") = lower;
    register APTR  tg  __asm("a2") = NULL;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -108(%%a6)"
        : "=r"(res), "+r"(c), "+r"(lo), "+r"(lib)
        : "r"(tg) : "d1", "cc", "memory");
    return res;
}

static ULONG TESTSIZE = 64 * 1024;   /* override with argv[2] (hex) */

int main(int argc, char **argv)
{
    APTR   ctx;
    ULONG  pgsz, base, size, p, flags;
    UBYTE *buf;
    volatile UBYTE *probe;
    static struct TagItem done = { TAG_DONE, 0 };
    int    npages, ndirty;

    setvbuf(stdout, NULL, _IONBF, 0);   /* progress visible even if we hang */
    printf("mmutest: a314rtg MMU dirty-tracking validation\n\n");

    MMUBase = OpenLibrary((STRPTR)"mmu.library", 43);
    if (!MMUBase) {
        printf("FAIL: mmu.library V43+ not available "
               "(OpenLibrary(\"mmu.library\",43) = NULL)\n");
        printf("      -> card will use full-scan fallback\n");
        return 10;
    }
    printf("  mmu.library open, version %d.%d\n",
           MMUBase->lib_Version, MMUBase->lib_Revision);

    ctx = mmu_CurrentContext();
    printf("  CurrentContext(NULL)      = 0x%08lx %s\n",
           (ULONG)ctx, ctx ? "" : "FAIL");
    if (!ctx) { CloseLibrary(MMUBase); return 10; }

    pgsz = mmu_GetPageSize(ctx);
    printf("  GetPageSize               = %lu bytes\n", pgsz);
    if (!pgsz || (pgsz & (pgsz - 1)) || pgsz > 65536) {
        printf("FAIL: unusable page size\n");
        CloseLibrary(MMUBase); return 10;
    }

    buf = (UBYTE *)AllocMem(TESTSIZE + pgsz, MEMF_FAST | MEMF_CLEAR);
    if (!buf) buf = (UBYTE *)AllocMem(TESTSIZE + pgsz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf) { printf("FAIL: no memory\n"); CloseLibrary(MMUBase); return 10; }

    if (argc > 2) TESTSIZE = strtoul(argv[2], NULL, 16);
    if (argc > 1 && strtoul(argv[1], NULL, 16) != 0) {
        /* target mode: validate an EXISTING region (e.g. the card fb from the
         * CMD_DEBUG line) instead of the fresh allocation */
        base = strtoul(argv[1], NULL, 16) & ~(pgsz - 1);
        printf("  TARGET mode: validating region at 0x%08lx\n", base);
        printf("  props(target) = 0x%08lx   props(pool buf) = 0x%08lx\n",
               mmu_GetPagePropertiesA(ctx, base),
               mmu_GetPagePropertiesA(ctx,
                   ((ULONG)buf + pgsz - 1) & ~(pgsz - 1)));
    } else {
        base = ((ULONG)buf + pgsz - 1) & ~(pgsz - 1);  /* page-aligned interior */
    }
    size = (TESTSIZE / pgsz) * pgsz;
    npages = (int)(size / pgsz);
    printf("  test buffer 0x%08lx, %d pages of %lu\n", base, npages, pgsz);

    printf("  calling SetProperties(SINGLEPAGE, size=0x%lx)...\n", size);
    if (!mmu_SetPropertiesA(ctx, MAPP_SINGLEPAGE, MAPP_SINGLEPAGE,
                            base, size, (APTR)&done)) {
        printf("FAIL: SetProperties(MAPP_SINGLEPAGE) returned FALSE\n");
        goto out;
    }
    printf("  SetProperties(SINGLEPAGE) = OK\n  calling RebuildTree...\n");

    if (!mmu_RebuildTree(ctx)) {
        printf("FAIL: RebuildTree returned FALSE\n");
        goto out;
    }
    printf("  RebuildTree               = OK\n");

    /* clear stale bits */
    for (p = base; p < base + size; p += pgsz)
        (void)mmu_GetPageUsedModified(ctx, p);
    printf("  cleared U/M on all pages\n");

    /* no-touch check: pages we never access again must stay clean */
    /* probe write into page 0 only */
    probe  = (volatile UBYTE *)base;
    *probe = 0x5a;

    ndirty = 0; flags = 0;
    for (p = base; p < base + size; p += pgsz) {
        ULONG f = mmu_GetPageUsedModified(ctx, p);
        if (f & MAPP_MODIFIED) {
            ndirty++;
            if (p == base) flags = f;
        }
    }
    printf("  after 1-byte write to page 0: %d of %d pages MODIFIED\n",
           ndirty, npages);

    if (!(flags & MAPP_MODIFIED)) {
        printf("\nFAIL: write did not set MODIFIED on its page "
               "-> card will fall back to full scan\n");
        goto out;
    }
    if (ndirty > 1) {
        printf("\nWARN: granularity coarser than one page (%d dirty)\n", ndirty);
    }

    /* clear-then-write again: proves the clear also flushed the ATC so a
     * SECOND write re-walks the table and sets M again (the failure mode a
     * naive raw-table implementation would hit) */
    *probe = 0xa5;
    if (mmu_GetPageUsedModified(ctx, base) & MAPP_MODIFIED) {
        printf("  repeat write after clear  = MODIFIED again (ATC ok)\n");
        printf("\nPASS: MMU dirty tracking fully functional - "
               "a314rtg.card 1.1 fast path will engage\n");
    } else {
        printf("\nFAIL: repeat write not detected (ATC not flushed on clear?) "
               "-> card falls back to full scan\n");
    }

out:
    FreeMem(buf, TESTSIZE + pgsz);
    CloseLibrary(MMUBase);
    return 0;
}
