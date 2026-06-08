/* loadtest.c - load our .card the way rtg.library does and exercise it.
 *
 * LoadSeg("LIBS:Picasso96/a314rtg.card") -> scan hunks for the romtag (0x4afc
 * matchword whose rt_MatchTag points to itself) -> InitResident() to build the
 * library -> OpenLibrary("a314rtg.card") to get the base -> call FindCard
 * (LVO -30) and InitCard (LVO -36) with printf checkpoints around each.
 *
 * Build: m68k-amigaos-gcc -noixemul -O2 -I../Picasso96Develop/PrivateInclude \
 *          -I../Picasso96Develop/Include loadtest.c -o loadtest
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/Picasso96.h>
#include "boardinfo.h"
#include <stdio.h>

extern struct ExecBase *SysBase;

LONG call_findcard(struct Library *cardbase, struct BoardInfo *bi);
asm(
"   .text                  \n"
"   .globl _call_findcard  \n"
"_call_findcard:           \n"
"   move.l  a6,-(sp)       \n"
"   move.l  8(sp),a6       \n"   /* cardbase */
"   move.l  12(sp),a0      \n"   /* bi */
"   jsr     -30(a6)        \n"   /* FindCard */
"   move.l  (sp)+,a6       \n"
"   rts                    \n"
);

LONG call_initcard(struct Library *cardbase, struct BoardInfo *bi);
asm(
"   .text                  \n"
"   .globl _call_initcard  \n"
"_call_initcard:           \n"
"   move.l  a6,-(sp)       \n"
"   move.l  8(sp),a6       \n"   /* cardbase */
"   move.l  12(sp),a0      \n"   /* bi */
"   suba.l  a1,a1          \n"   /* toolTypes = NULL */
"   jsr     -36(a6)        \n"   /* InitCard */
"   move.l  (sp)+,a6       \n"
"   rts                    \n"
);

static struct Resident *find_romtag(BPTR seg)
{
    BPTR s = seg;
    while (s) {
        ULONG *sl = (ULONG *)BADDR(s);
        ULONG  bytes = sl[-1];                       /* block size incl 8-byte hdr */
        UWORD *p   = (UWORD *)&sl[1];                 /* code starts after next-ptr */
        UWORD *lim = (UWORD *)((UBYTE *)sl - 4 + bytes);
        for (; p < lim; p++) {
            if (*p == 0x4afc) {
                struct Resident *rt = (struct Resident *)p;
                if (rt->rt_MatchTag == rt)
                    return rt;
            }
        }
        s = (BPTR)sl[0];                              /* next segment */
    }
    return NULL;
}

int main(void)
{
    BPTR              seg;
    struct Resident  *rt;
    APTR              ir;
    struct Library   *card;
    struct BoardInfo *bi;
    LONG r;

    printf("sizeof(struct BoardInfo) = %lu\n", (ULONG)sizeof(struct BoardInfo));

    printf("LoadSeg(LIBS:Picasso96/a314rtg.card)...\n");
    seg = LoadSeg((STRPTR)"LIBS:Picasso96/a314rtg.card");
    printf("  seg = %08lx\n", (ULONG)seg);
    if (!seg) { printf("FAILED LoadSeg - file missing?\n"); return 0; }

    rt = find_romtag(seg);
    printf("  romtag = %08lx\n", (ULONG)rt);
    if (!rt) { printf("FAILED - no romtag found in segments\n"); UnLoadSeg(seg); return 0; }
    printf("  rt_Flags=%02x rt_Type=%u rt_Name=%s\n",
           rt->rt_Flags, rt->rt_Type, rt->rt_Name ? (char *)rt->rt_Name : "?");

    printf("InitResident...\n");
    ir = InitResident(rt, seg);
    printf("  InitResident returned %08lx\n", (ULONG)ir);

    card = (struct Library *)OpenLibrary((STRPTR)"a314rtg.card", 0);
    printf("OpenLibrary(\"a314rtg.card\") = %08lx\n", (ULONG)card);
    if (!card) { printf("FAILED - library not in list after InitResident\n"); return 0; }
    printf("  lib_Version=%u OpenCnt=%u NegSize=%u PosSize=%u\n",
           card->lib_Version, card->lib_OpenCnt,
           card->lib_NegSize, card->lib_PosSize);

    bi = (struct BoardInfo *)AllocMem(sizeof(struct BoardInfo),
                                      MEMF_PUBLIC | MEMF_CLEAR);
    printf("BoardInfo = %08lx\n", (ULONG)bi);
    if (!bi) { CloseLibrary(card); return 0; }

    printf("calling FindCard...\n");
    r = call_findcard(card, bi);
    printf("  FindCard -> %ld  (MemoryBase=%08lx MemorySize=%lu)\n",
           r, (ULONG)bi->MemoryBase, (ULONG)bi->MemorySize);

    printf("calling InitCard...\n");
    r = call_initcard(card, bi);
    printf("  InitCard -> %ld\n", r);
    printf("  BoardType=%u RGBFormats=%08lx SetGC=%08lx GetVSyncState=%08lx\n",
           bi->BoardType, (ULONG)bi->RGBFormats,
           (ULONG)bi->SetGC, (ULONG)bi->GetVSyncState);

    printf("\nALL DONE - FindCard and InitCard ran without crashing.\n");
    /* leave the library loaded; don't FreeMem bi (card may keep pointers) */
    return 0;
}
