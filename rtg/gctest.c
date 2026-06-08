/* gctest.c - drive the card's SetGC directly to test the diff-spawn+connect
 * path, independent of P96. Loads the card, FindCard, InitCard, builds a
 * 640x480x16 ModeInfo, calls bi->SetGC (which should ensure_diff_started ->
 * CreateNewProc -> connect to the Pi "rtg" service), then waits ~6s so the
 * diff process can connect. Watch the Pi log for "Amiga connected".
 *
 * Build: m68k-amigaos-gcc -noixemul -O2 -I../Picasso96Develop/PrivateInclude \
 *          -I../Picasso96Develop/Include gctest.c -o gctest
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/Picasso96.h>
#include "boardinfo.h"
#include "settings.h"
#include <stdio.h>

extern struct ExecBase *SysBase;

LONG call_findcard(struct Library *cb, struct BoardInfo *bi);
asm("   .text\n .globl _call_findcard\n_call_findcard:\n"
    "   move.l a6,-(sp)\n move.l 8(sp),a6\n move.l 12(sp),a0\n"
    "   jsr -30(a6)\n move.l (sp)+,a6\n rts\n");
LONG call_initcard(struct Library *cb, struct BoardInfo *bi);
asm("   .text\n .globl _call_initcard\n_call_initcard:\n"
    "   move.l a6,-(sp)\n move.l 8(sp),a6\n move.l 12(sp),a0\n"
    "   suba.l a1,a1\n jsr -36(a6)\n move.l (sp)+,a6\n rts\n");
/* call_setgc(func, bi, mi): a0=bi a1=mi d0=0, jsr (func) */
void call_setgc(APTR func, struct BoardInfo *bi, struct ModeInfo *mi);
asm("   .text\n .globl _call_setgc\n_call_setgc:\n"
    "   move.l a2,-(sp)\n move.l 8(sp),a2\n move.l 12(sp),a0\n"
    "   move.l 16(sp),a1\n moveq #0,d0\n jsr (a2)\n move.l (sp)+,a2\n rts\n");

static struct Resident *find_romtag(BPTR seg)
{
    while (seg) {
        ULONG *sl = (ULONG *)BADDR(seg);
        UWORD *p = (UWORD *)&sl[1], *lim = (UWORD *)((UBYTE *)sl - 4 + sl[-1]);
        for (; p < lim; p++)
            if (*p == 0x4afc && ((struct Resident *)p)->rt_MatchTag == (struct Resident *)p)
                return (struct Resident *)p;
        seg = (BPTR)sl[0];
    }
    return NULL;
}

int main(void)
{
    BPTR seg;
    struct Resident *rt;
    struct Library *card;
    struct BoardInfo *bi;
    struct ModeInfo *mi;

    seg = LoadSeg((STRPTR)"LIBS:Picasso96/a314rtg.card");
    printf("LoadSeg=%08lx\n", (ULONG)seg);
    if (!seg) return 0;
    rt = find_romtag(seg);
    printf("romtag=%08lx\n", (ULONG)rt);
    if (!rt) { UnLoadSeg(seg); return 0; }
    InitResident(rt, seg);
    card = (struct Library *)OpenLibrary((STRPTR)"a314rtg.card", 0);
    printf("OpenLibrary=%08lx\n", (ULONG)card);
    if (!card) return 0;

    bi = (struct BoardInfo *)AllocMem(sizeof(struct BoardInfo), MEMF_PUBLIC | MEMF_CLEAR);
    mi = (struct ModeInfo  *)AllocMem(sizeof(struct ModeInfo),  MEMF_PUBLIC | MEMF_CLEAR);
    printf("bi=%08lx mi=%08lx\n", (ULONG)bi, (ULONG)mi);
    if (!bi || !mi) return 0;

    printf("FindCard -> %ld\n", call_findcard(card, bi));
    printf("InitCard -> %ld\n", call_initcard(card, bi));
    printf("bi->SetGC = %08lx\n", (ULONG)bi->SetGC);

    mi->Width = 640; mi->Height = 480; mi->Depth = 16;
    printf("calling SetGC(640x480x16) - should spawn diff + connect...\n");
    call_setgc((APTR)bi->SetGC, bi, mi);
    printf("SetGC returned (no crash). Waiting 6s for diff to connect...\n");
    Delay(300);
    printf("done - check the Pi log for 'rtg: Amiga connected'.\n");
    /* leave library + diff process running on purpose */
    return 0;
}
