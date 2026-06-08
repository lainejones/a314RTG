/* carvetest2.c - crash bisector for A314RTG init, as a normal CLI program.
 *
 * TEST A: exercises dbgf() (RawDoFmt + the hand-written asm PutChProc) exactly
 *         as the card does, with printf checkpoints around it.
 * TEST B: runs the REAL carve_card_mem() including the chunk-splitting loop
 *         (printf-instrumented), to see whether the split itself crashes.
 *
 * Whichever test fails to print "SURVIVED" is the source of the #80000004.
 *
 * NOTE: TEST B performs the real split on the live memory list and does NOT
 * undo it (the carved block stays allocated, like the card). Harmless, but
 * REBOOT afterwards to restore full FastRAM.
 *
 * Build: m68k-amigaos-gcc -noixemul -O2 carvetest2.c -o carvetest2
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <proto/exec.h>
#include <stdio.h>

#define CARD_MEM_SIZE  (2UL * 1024 * 1024)

extern struct ExecBase *SysBase;

/* ---- same asm PutChProc the card uses for dbgf ------------------------- */
asm(
"   .text                        \n"
"   .globl _a314rtg_rawputch     \n"
"_a314rtg_rawputch:              \n"
"   move.l  a6,-(sp)             \n"
"   move.l  a3,-(sp)             \n"
"   move.l  4.w,a6               \n"
"   jsr     -516(a6)             \n"
"   move.l  (sp)+,a3             \n"
"   move.l  (sp)+,a6             \n"
"   rts                          \n"
);
extern void a314rtg_rawputch(void);

static void dbgf(const char *fmt, ...)
{
    RawDoFmt((CONST_STRPTR)fmt, (APTR)((&fmt) + 1),
             (void (*)())a314rtg_rawputch, (APTR)0);
}

/* ---- the real carve, with printf instead of dbgf for visibility -------- */
static UBYTE *carve_card_mem(ULONG size)
{
    UBYTE *block;
    struct MemHeader *mh, *upper;
    struct MemChunk *mc, *next;
    struct MemChunk *lowhead = NULL, *lowtail = NULL;
    struct MemChunk *uphead  = NULL, *uptail  = NULL;
    ULONG a, aend, orig_upper, lowfree = 0, upfree = 0;
    BOOL  used_upper = FALSE;

    size = (size + 0x1FUL) & ~0x1FUL;

    block = (UBYTE *)AllocMem(size, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    if (!block) { printf("  carve: AllocMem FAILED\n"); return NULL; }
    upper = (struct MemHeader *)AllocMem(sizeof(struct MemHeader),
                                         MEMF_PUBLIC | MEMF_CLEAR);
    a = (ULONG)block; aend = a + size;
    printf("  carve: block=%08lx upper=%08lx size=%lu\n",
           (ULONG)block, (ULONG)upper, size);

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        if (a >= (ULONG)mh->mh_Lower && aend <= (ULONG)mh->mh_Upper)
            break;
    if (mh->mh_Node.ln_Succ == NULL) {
        Permit();
        printf("  carve: NO MEMHEADER contains block - in pool\n");
        if (upper) FreeMem(upper, sizeof(struct MemHeader));
        return block;
    }

    orig_upper = (ULONG)mh->mh_Upper;
    printf("  carve: mh=%08lx lo=%08lx up=%08lx attr=%04lx\n",
           (ULONG)mh, (ULONG)mh->mh_Lower, orig_upper, (ULONG)mh->mh_Attributes);
    if (aend < orig_upper && !upper) {
        Permit();
        printf("  carve: need upper header but AllocMem gave NULL\n");
        return block;
    }

    for (mc = mh->mh_First; mc; mc = next) {
        next = mc->mc_Next; mc->mc_Next = NULL;
        if ((ULONG)mc >= aend) {
            if (uptail) uptail->mc_Next = mc; else uphead = mc;
            uptail = mc; upfree += mc->mc_Bytes;
        } else {
            if (lowtail) lowtail->mc_Next = mc; else lowhead = mc;
            lowtail = mc; lowfree += mc->mc_Bytes;
        }
    }

    mh->mh_First = lowhead;
    mh->mh_Free  = lowfree;
    mh->mh_Upper = (APTR)a;

    if (aend < orig_upper) {
        upper->mh_Node.ln_Type = NT_MEMORY;
        upper->mh_Node.ln_Pri  = mh->mh_Node.ln_Pri;
        upper->mh_Node.ln_Name = mh->mh_Node.ln_Name;
        upper->mh_Attributes   = mh->mh_Attributes;
        upper->mh_First        = uphead;
        upper->mh_Lower        = (APTR)aend;
        upper->mh_Upper        = (APTR)orig_upper;
        upper->mh_Free         = upfree;
        Enqueue(&SysBase->MemList, (struct Node *)upper);
        used_upper = TRUE;
    }
    Permit();
    printf("  carve: SPLIT OK a=%08lx aend=%08lx (off-pool now)\n", a, aend);

    if (upper && !used_upper)
        FreeMem(upper, sizeof(struct MemHeader));
    return block;
}

static void dump_memlist(void)
{
    struct MemHeader *mh; int i = 0;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ, i++)
        printf("  [%d] %-14s attr=%04lx lower=%08lx upper=%08lx free=%lu KB\n",
               i, mh->mh_Node.ln_Name ? (char *)mh->mh_Node.ln_Name : "(noname)",
               (ULONG)mh->mh_Attributes,
               (ULONG)mh->mh_Lower, (ULONG)mh->mh_Upper, mh->mh_Free / 1024);
    Permit();
}

int main(void)
{
    UBYTE *block;

    printf("=== TEST A: dbgf (RawDoFmt + asm PutChProc) ===\n");
    printf("  calling dbgf (output goes to serial, not here)...\n");
    dbgf((CONST_STRPTR)"dbgf-test %ld %08lx\n", 42L, 0x12345678L);
    printf("  dbgf SURVIVED\n\n");

    printf("=== TEST B: real carve_card_mem incl. split loop ===\n");
    block = carve_card_mem(CARD_MEM_SIZE);
    printf("  carve returned %08lx, SURVIVED\n\n", (ULONG)block);

    printf("Memory list now (FastRAM should be split in two):\n");
    dump_memlist();

    printf("\nALL DONE - neither dbgf nor the carve crashed.\n");
    printf("(FastRAM is now split; reboot to restore.)\n");
    return 0;
}
