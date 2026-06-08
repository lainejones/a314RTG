/* carvetest.c - standalone CLI diagnostic for the A314RTG memory carve.
 *
 * Runs as a NORMAL Amiga CLI program (printf to the Shell) so we get real,
 * guaranteed output without serial debug or rtg.library. It mirrors what
 * carve_card_mem() does in the card driver, but NON-DESTRUCTIVELY: it walks
 * the system memory list, performs the same 2 MB AllocMem the card does,
 * reports exactly which MemHeader the block lands in and whether the carve
 * (split off-pool) would engage or fall back -- then frees the block and
 * leaves the live memory list untouched.
 *
 * Build (normal startup, has printf):
 *   m68k-amigaos-gcc -noixemul -O2 carvetest.c -o carvetest
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <stdio.h>

#define CARD_MEM_SIZE  (2UL * 1024 * 1024)

extern struct ExecBase *SysBase;

static ULONG largest_chunk(struct MemHeader *mh)
{
    struct MemChunk *mc;
    ULONG best = 0;
    for (mc = mh->mh_First; mc; mc = mc->mc_Next)
        if (mc->mc_Bytes > best) best = mc->mc_Bytes;
    return best;
}

static void dump_memlist(const char *when)
{
    struct MemHeader *mh;
    int i = 0;
    printf("\n--- MemList %s ---\n", when);
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ, i++) {
        ULONG attr = mh->mh_Attributes;
        printf(" [%d] %-14s attr=%04lx %s%s lower=%08lx upper=%08lx "
               "free=%lu KB largest=%lu KB\n",
               i,
               mh->mh_Node.ln_Name ? (char *)mh->mh_Node.ln_Name : "(noname)",
               attr,
               (attr & MEMF_CHIP) ? "CHIP " : "",
               (attr & MEMF_FAST) ? "FAST " : "",
               (ULONG)mh->mh_Lower, (ULONG)mh->mh_Upper,
               mh->mh_Free / 1024, largest_chunk(mh) / 1024);
    }
    Permit();
}

int main(void)
{
    UBYTE *block;
    struct MemHeader *mh, *found = NULL;
    ULONG size = (CARD_MEM_SIZE + 0x1FUL) & ~0x1FUL;
    ULONG a, aend, orig_upper;

    printf("A314RTG carve diagnostic - requesting %lu KB MEMF_FAST\n",
           size / 1024);

    dump_memlist("BEFORE alloc");

    block = (UBYTE *)AllocMem(size, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    printf("\nAllocMem(MEMF_FAST) -> %08lx\n", (ULONG)block);
    if (!block) {
        printf("FAILED: no FAST memory. Carve cannot run.\n");
        return 0;
    }
    a = (ULONG)block;
    aend = a + size;

    /* Find the MemHeader whose range contains the block. */
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        if (a >= (ULONG)mh->mh_Lower && aend <= (ULONG)mh->mh_Upper) {
            found = mh;
            break;
        }
    Permit();

    if (!found) {
        printf("\nRESULT: NO MEMHEADER contains the block.\n");
        printf("        => carve FALLS BACK to in-pool memory (the bug).\n");
        printf("        block=%08lx..%08lx is outside every mh_Lower..mh_Upper.\n",
               a, aend);
        FreeMem(block, size);
        return 0;
    }

    orig_upper = (ULONG)found->mh_Upper;
    printf("\nRESULT: block is inside MemHeader '%s' (%08lx)\n",
           found->mh_Node.ln_Name ? (char *)found->mh_Node.ln_Name : "(noname)",
           (ULONG)found);
    printf("        mh_Lower=%08lx mh_Upper=%08lx attr=%04lx\n",
           (ULONG)found->mh_Lower, orig_upper, found->mh_Attributes);
    printf("        block   =%08lx..%08lx\n", a, aend);
    if (aend < orig_upper)
        printf("        => carve WOULD SPLIT: lower stays, %lu KB above goes to a "
               "new upper MemHeader. Needs the extra MemHeader AllocMem.\n",
               (orig_upper - aend) / 1024);
    else
        printf("        => block reaches the very top of this header; carve just "
               "lowers mh_Upper (no upper MemHeader needed).\n");

    FreeMem(block, size);
    dump_memlist("AFTER free (should match BEFORE)");

    printf("\nDone. (No permanent changes made to the live memory list.)\n");
    return 0;
}
