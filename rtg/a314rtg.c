/*
 * a314rtg.c - A314RTG Picasso96 card driver - Milestone 2 (dirty-tile diffing)
 *
 * HARDWARE REALITY (clockport / MODEL_CP):
 *   The A314cp board has NO DMA-shared system RAM - only a 64 KB SRAM window
 *   reached byte-by-byte through clockport registers (~100 KB/s). So the Pi
 *   CANNOT read the Amiga framebuffer directly; TranslateAddressA314 is a no-op
 *   on this board. Everything must stream through A314_WRITE (<=252 bytes/pkt).
 *
 * DESIGN:
 *   - P96 renders normally into the local Amiga framebuffer (GRANTDIRECTACCESS).
 *     FillRect/BlitRect are implemented locally so the framebuffer is always a
 *     correct copy of the screen regardless of which path P96 takes.
 *   - A separate Process ("a314rtg.diff") owns the A314 connection. It polls,
 *     diffs the framebuffer against a shadow copy in horizontal runs, and sends
 *     only the changed pixels (raw bytes - no pixel-format interpretation, so
 *     byte order is automatically correct end-to-end) to the Pi "rtg" service.
 *   - rtg.py blits received runs to /dev/fb0.
 *
 * Card callbacks never touch a314.device (they can run in Forbid()/high-pri
 * contexts); they only update shared state and wake the diff process.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <exec/resident.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/Picasso96.h>
#include "boardinfo.h"
#include "a314.h"

/* ====================================================================== *
 *  Library scaffolding (modelled on the MNT ZZ9000.card driver, which is
 *  the same architecture: a P96 card backed by an external renderer). The
 *  romtag is plain inline asm; Open/Close/Expunge/Init are normal C funcs.
 *  This replaces the previous hand-written card_start.S, whose library-init
 *  path made exec jump into the funcTable terminator (#8000000B Guru).
 * ====================================================================== */

#define VERSION   1
#define REVISION  1

struct A314RTGBase {
    struct Library   libNode;
    BPTR             segList;
    struct ExecBase *sysBase;
};

struct ExecBase   *SysBase;
struct DosLibrary *DOSBase = NULL;        /* used by proto/dos.h inlines */
static struct A314RTGBase *_base = NULL;  /* our library base             */

/* P96 entry points (defined at the bottom of this file) */
BOOL FindCard(struct BoardInfo *bi __asm("a0"));
BOOL InitCard(struct BoardInfo *bi __asm("a0"), STRPTR *toolTypes __asm("a1"));

/* library vectors (referenced by the romtag via inline asm) */
struct A314RTGBase *OpenLib(struct A314RTGBase *base __asm("a6"));
BPTR CloseLib(struct A314RTGBase *base __asm("a6"));
BPTR ExpungeLib(struct A314RTGBase *base __asm("a6"));
static ULONG ExtFuncLib(void);
struct A314RTGBase *InitLib(struct ExecBase *sysbase __asm("a6"),
                                     BPTR seglist __asm("a0"),
                                     struct A314RTGBase *base __asm("d0"));

char device_name[]      = "a314rtg.card";
char device_id_string[] = "a314rtg.card 1.1 (24.07.2026)\r\n";

static const APTR device_vectors[] = {
    (APTR)OpenLib,
    (APTR)CloseLib,
    (APTR)ExpungeLib,
    (APTR)ExtFuncLib,
    (APTR)FindCard,     /* LVO -30 */
    (APTR)InitCard,     /* LVO -36 */
    (APTR)-1,
};

const ULONG auto_init_tables[4] = {
    sizeof(struct A314RTGBase),
    (ULONG)device_vectors,
    0,                        /* no data table; InitLib fills the node */
    (ULONG)InitLib,
};

asm(
"   .globl _a314rtg_romtag           \n"
"_a314rtg_start:                     \n"
"   moveq #-1,d0                     \n"   /* fail if run as a program */
"   rts                              \n"
"   .even                           \n"
"_a314rtg_romtag:                    \n"
"   dc.w 0x4afc                      \n"   /* RTC_MATCHWORD            */
"   dc.l _a314rtg_romtag             \n"   /* rt_MatchTag              */
"   dc.l _a314rtg_endskip            \n"   /* rt_EndSkip               */
"   dc.b 0x80                        \n"   /* rt_Flags = RTF_AUTOINIT (0x80!)
                                            * 0x01 was RTF_COLDSTART -> InitResident
                                            * called rt_Init as code (the auto-init
                                            * DATA table) -> #80000004. */
"   dc.b 1                           \n"   /* rt_Version               */
"   dc.b 0x09                        \n"   /* rt_Type  = NT_LIBRARY    */
"   dc.b 0                           \n"   /* rt_Pri                   */
"   dc.l _device_name                \n"   /* rt_Name                  */
"   dc.l _device_id_string           \n"   /* rt_IdString              */
"   dc.l _auto_init_tables           \n"   /* rt_Init -> auto-init      */
"_a314rtg_endskip:                   \n"
);

struct A314RTGBase *InitLib(struct ExecBase *sysbase __asm("a6"),
                                     BPTR seglist __asm("a0"),
                                     struct A314RTGBase *base __asm("d0"))
{
    _base = base;
    _base->segList               = seglist;
    _base->sysBase               = sysbase;
    _base->libNode.lib_Node.ln_Type = NT_LIBRARY;
    _base->libNode.lib_Node.ln_Name = device_name;
    _base->libNode.lib_Flags     = LIBF_SUMUSED | LIBF_CHANGED;
    _base->libNode.lib_Version   = VERSION;
    _base->libNode.lib_Revision  = REVISION;
    _base->libNode.lib_IdString  = (APTR)device_id_string;
    SysBase = *(struct ExecBase **)4L;
    return _base;
}

struct A314RTGBase *OpenLib(struct A314RTGBase *base __asm("a6"))
{
    base->libNode.lib_OpenCnt++;
    base->libNode.lib_Flags &= ~LIBF_DELEXP;
    return base;
}

BPTR ExpungeLib(struct A314RTGBase *base __asm("a6"))
{
    /* Skeleton card: never expunge (we hold a permanent framebuffer). */
    base->libNode.lib_Flags |= LIBF_DELEXP;
    return 0;
}

BPTR CloseLib(struct A314RTGBase *base __asm("a6"))
{
    if (base->libNode.lib_OpenCnt > 0)
        base->libNode.lib_OpenCnt--;
    return 0;
}

static ULONG ExtFuncLib(void)
{
    return 0;
}

/* ---- Framebuffer (Amiga-side, P96 renders here) ------------------------- */

#define FB_WIDTH_MAX   800
#define FB_HEIGHT_MAX  600
#define FB_BPP         2
#define FB_SIZE        (FB_WIDTH_MAX * FB_HEIGHT_MAX * FB_BPP)   /* max visible */

/* Card memory handed to rtg.library. 2 MB is enough for small 16-bit screens
 * (800x600x2 = 960 KB visible + off-screen) and keeps us compatible with the
 * low-RAM A500/600 edge-card A314 variants. CRITICAL: this region is CARVED
 * out of the top of FastRAM so it sits OUTSIDE the system memory pool, the way
 * a real Zorro card's VRAM does - rtg.library rejects ordinary AllocMem'd RAM
 * (still inside a MemHeader) when building the board context. */
#define CARD_MEM_SIZE  (2 * 1024 * 1024)

static UBYTE *framebuffer  = NULL;  /* card memory (CARD_MEM_SIZE)           */
static UBYTE *shadow       = NULL;  /* last state pushed (FB_SIZE)           */
static UBYTE *visible_base = NULL;  /* base of the visible screen (SetPanning)*/

/* ---- Shared mode / switch state (card callbacks <-> diff process) -------- */

static volatile UWORD mode_w   = 0;     /* visible width  in pixels         */
static volatile UWORD mode_h   = 0;     /* visible height in pixels         */
static volatile UWORD mode_bpr = 0;     /* bytes per row (bitmap stride)     */
static volatile UWORD pan_width = 0;    /* SetPanning bitmap width (pixels)  */
static volatile UBYTE mode_bpp  = 16;
static volatile UBYTE mode_rgbf = 0;     /* raw RGBFTYPE from SetPanning      */
static volatile UWORD ri_bpr    = 0;     /* P96's own RenderInfo BytesPerRow  */
static volatile ULONG ri_mem    = 0;     /* ...and Memory, seen in FillRect   */
static volatile BOOL  dbg_dirty = FALSE;
static volatile BOOL  mode_swap = FALSE; /* 16-bit big-endian mode: swap pixel
                                          * bytes into the wire packets so the
                                          * Pi's LE RGB565 fb shows true colour
                                          * (shadow keeps frame bytes) */
static volatile BOOL  mode_changed  = FALSE;
static volatile BOOL  switch_state  = TRUE;
static volatile BOOL  switch_changed = FALSE;
static volatile BOOL  diff_running   = FALSE;

/* CLUT for 8-bit modes: 256 entries pre-converted to RGB565 little-endian
 * (2 bytes each) so the diff/Pi never decode - SetColorArray fills this, the
 * diff process ships it to the Pi as CMD_SETPALETTE when palette_changed. */
static volatile UBYTE pal[256 * 2];
static volatile BOOL  palette_changed = FALSE;

/* ---- RTG wire protocol --------------------------------------------------
 *
 * Every command is one A314_WRITE, self-contained, <= 252 bytes.
 * Multi-byte header fields are big-endian; pixel payloads are raw framebuffer
 * bytes (R5G6B5PC, little-endian in memory) copied verbatim - never decoded.
 *
 *   CMD_SETMODE   (1): w(2) h(2) bpp(1)                       = 6 bytes
 *   CMD_SETSWITCH (2): state(1)                               = 2 bytes
 *   CMD_PIXELS    (4): x(2) y(2) n(1) pixels(n*2)             = 6 + n*2 bytes
 *   CMD_FILL      (5): x(2) y(2) n(2) pixel(2)                = 9 bytes
 */
#define CMD_SETMODE    1
#define CMD_SETSWITCH  2
#define CMD_PIXELS     4   /* 16-bit: x2 y2 n1 + n*2 raw R5G6B5PC bytes */
#define CMD_FILL       5   /* 16-bit: x2 y2 n2 + 2 raw bytes            */
#define CMD_SETPALETTE 6   /* 8-bit:  start1 count1 + count*2 RGB565-LE */
#define CMD_PIXELS8    7   /* 8-bit:  x2 y2 n1 + n CLUT-index bytes      */
#define CMD_FILL8      8   /* 8-bit:  x2 y2 n2 + 1 CLUT-index byte       */
#define CMD_DEBUG      9   /* panwidth(2) bpr(2) rgbf(1) bpp(1) mmu(1) - the
                            * Pi just journals it; sent on every mode change */

#define MAX_IDX_PER_PKT  240         /* 6 + 240 = 246 <= 252 (8-bit pixels)  */
#define PAL_PER_PKT      120         /* 3 + 120*2 = 243 <= 252 (palette)     */

#define MAX_PIX_PER_PKT   120        /* 6 + 120*2 = 246 <= 252               */
#define FILL_MIN_RUN      3          /* min uniform run worth a FILL packet
                                      * (FILL=9B beats PIXELS=6+2n once n>=2; 3
                                      * keeps tiny noise as PIXELS but compresses
                                      * Workbench's flat areas hard)          */
#define MAX_BYTES_PER_PASS 12000L    /* push more per wake so big repaints don't
                                      * stall in 40ms idle gaps; the slow link
                                      * self-throttles by blocking on a314_write */

/* Pack a UWORD big-endian into buf[off..off+1] */
#define PW(buf, off, v) do { \
    (buf)[(off)]   = (UBYTE)((v) >> 8); \
    (buf)[(off)+1] = (UBYTE)(v); \
} while (0)

/*
 * 16x16 -> 32 unsigned multiply via the 68000's hardware MULU.W. Used for
 * row*bytesperrow addressing. Avoids the __mulsi3 libgcc helper (a 32x32
 * multiply), which is unavailable under -nostdlib. Both operands must fit in
 * 16 bits (row <= 600, bytesperrow <= 1600 - always true here).
 */
static inline ULONG mul16(UWORD a, UWORD b)
{
    ULONG res = a;
    __asm__ ("mulu.w %1,%0" : "+d"(res) : "d"((ULONG)b) : "cc");
    return res;
}

/* Bytes per pixel for a P96 depth (we mirror 8-bit CLUT and 16-bit R5G6B5). */
static UWORD bpp_bytes(UWORD depth)
{
    if (depth <= 8)  return 1;
    if (depth <= 16) return 2;
    if (depth <= 24) return 3;
    return 4;
}

/* ---- mmu.library dirty-page tracking (optional fast path) ----------------
 *
 * Idea (suggested by eriQue): instead of re-comparing the whole framebuffer
 * against the shadow every pass, ask the 68030's MMU which pages were written.
 * Every page descriptor carries a hardware-maintained M (modified) bit; the
 * mmu.library V43+ call GetPageUsedModified() reads AND clears it atomically
 * (including the ATC flush that raw table peeking would get wrong).
 *
 * We NEVER touch PMMU registers directly - this machine runs MMULib/MuForce,
 * which owns the MMU tree. Everything goes through mmu.library, and the whole
 * path self-validates at init with a probe write: if the M-bit doesn't report
 * it (no MMU tree, transparent translation, whatever), mmu_active stays FALSE
 * and diffing falls back to full scanning, exactly as before. A periodic
 * reconciliation sweep re-diffs everything anyway, so even a lying M-bit can
 * only delay an update, never lose it.
 */

#define MAPP_USED        (1UL << 3)    /* from MMULib mmu/context.h */
#define MAPP_MODIFIED    (1UL << 4)
#define MAPP_SINGLEPAGE  (1UL << 12)   /* page-level calls REQUIRE this */

static struct Library *MMUBase   = NULL;
static APTR            mmu_ctx   = NULL;
static ULONG           mmu_pgsz  = 0;
static BOOL            mmu_active = FALSE;
static UBYTE           mmu_stage  = 0;  /* how far init got (CMD_DEBUG):
                                         * 0=no lib 1=no ctx 2=bad pgsz
                                         * 3=SetProps failed 4=Rebuild failed
                                         * 5=probe failed 6=ACTIVE */

/* Row-needs-diff flags driving the send loop (indexed by screen row). */
static UBYTE dirty_rows[FB_HEIGHT_MAX];

#define RECON_EVERY 128        /* full re-diff every ~5s as a safety net */
static UWORD recon_countdown = 0;
static ULONG last_vbase = 0;   /* pan/stride change detection */
static UWORD last_bpr   = 0;

static void mark_all_rows(void)
{
    UWORD i;
    for (i = 0; i < FB_HEIGHT_MAX; i++) dirty_rows[i] = 1;
}

/* Call stubs - no amiga.lib glue exists for mmu.library. Standard AmigaOS
 * convention: d0/d1/a0/a1 are scratch, the rest is preserved. LVOs from
 * MMULib's mmu_pragmas.h. */

static APTR mmu_CurrentContext(void)            /* LVO -0x0f0, a1=task */
{
    register APTR res  __asm("d0");
    register APTR task __asm("a1") = NULL;
    register APTR lib  __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -240(%%a6)"
        : "=r"(res), "+r"(task), "+r"(lib) : : "d1", "a0", "cc", "memory");
    return res;
}

static ULONG mmu_GetPageSize(APTR ctx)          /* LVO -0x030, a0=ctx */
{
    register ULONG res __asm("d0");
    register APTR  c   __asm("a0") = ctx;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -48(%%a6)"
        : "=r"(res), "+r"(c), "+r"(lib) : : "d1", "a1", "cc", "memory");
    return res;
}

static ULONG mmu_GetPageUsedModified(APTR ctx, ULONG lower)
{                                               /* LVO -0x17a, a0=ctx a1=lower */
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
{               /* LVO -0x054, a0=ctx d1=flags d2=mask a1=lower d0=size a2=tags */
    register ULONG res __asm("d0") = size;     /* d0 is input AND result */
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

static BOOL mmu_RebuildTree(APTR ctx)           /* LVO -0x060, a0=ctx */
{
    register ULONG res __asm("d0");
    register APTR  c   __asm("a0") = ctx;
    register APTR  lib __asm("a6") = (APTR)MMUBase;
    __asm__ volatile ("jsr -96(%%a6)"
        : "=r"(res), "+r"(c), "+r"(lib) : : "d1", "a1", "cc", "memory");
    return (BOOL)res;
}

/* One-time init, run from the diff process (safe user context). Any failure
 * just leaves mmu_active FALSE - the full-scan fallback is always correct. */
static void mmu_init(void)
{
    ULONG base, size, p;
    volatile UBYTE *probe;
    static struct TagItem done = { TAG_DONE, 0 };

    if (MMUBase) return;                        /* only try once per boot */
    MMUBase = OpenLibrary((STRPTR)"mmu.library", 43);
    if (!MMUBase) return;                       /* V43+ needed for UsedModified */

    mmu_stage = 1;
    mmu_ctx = mmu_CurrentContext();
    if (!mmu_ctx) return;
    mmu_stage = 2;
    mmu_pgsz = mmu_GetPageSize(mmu_ctx);
    if (!mmu_pgsz || (mmu_pgsz & (mmu_pgsz - 1)) || mmu_pgsz > 65536) return;
    mmu_stage = 3;

    /* Force per-page descriptors over the whole card memory (visible_base can
     * sit anywhere inside it). Without MAPP_SINGLEPAGE a large block descriptor
     * could cover the region and page-level queries are not permitted. */
    base = (ULONG)framebuffer & ~(mmu_pgsz - 1);
    size = (((ULONG)framebuffer + CARD_MEM_SIZE + mmu_pgsz - 1)
            & ~(mmu_pgsz - 1)) - base;
    if (!mmu_SetPropertiesA(mmu_ctx, MAPP_SINGLEPAGE, MAPP_SINGLEPAGE,
                            base, size, (APTR)&done)) return;
    mmu_stage = 4;
    if (!mmu_RebuildTree(mmu_ctx)) return;
    mmu_stage = 5;

    /* Clear stale M-bits, then prove the path end-to-end with one real write:
     * if the hardware doesn't report it, never trust the MMU on this setup. */
    for (p = base; p < base + size; p += mmu_pgsz)
        (void)mmu_GetPageUsedModified(mmu_ctx, p);
    probe  = (volatile UBYTE *)framebuffer;
    *probe = *probe;
    if (mmu_GetPageUsedModified(mmu_ctx, base) & MAPP_MODIFIED) {
        mmu_active = TRUE;
        mmu_stage  = 6;
    }
}

/* ---- A314 connection (owned by the diff process only) ------------------- */

static struct MsgPort        *a314_mp     = NULL;
static struct A314_IORequest *a314_ior    = NULL;
static BYTE                   a314_sigbit = -1;
static ULONG                  a314_socket = 0;

static const char a314_name[] = "a314.device";
static const char rtg_svc[]   = "rtg";

static BOOL a314_open(void)
{
    a314_mp = (struct MsgPort *)AllocMem(
        sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);
    if (!a314_mp) return FALSE;

    a314_sigbit = AllocSignal(-1);
    if (a314_sigbit == -1) {
        FreeMem(a314_mp, sizeof(struct MsgPort));
        a314_mp = NULL;
        return FALSE;
    }

    a314_mp->mp_Node.ln_Type        = NT_MSGPORT;
    a314_mp->mp_Flags               = PA_SIGNAL;
    a314_mp->mp_SigBit              = (UBYTE)a314_sigbit;
    a314_mp->mp_SigTask             = FindTask(NULL);
    a314_mp->mp_MsgList.lh_Head     = (struct Node *)&a314_mp->mp_MsgList.lh_Tail;
    a314_mp->mp_MsgList.lh_Tail     = NULL;
    a314_mp->mp_MsgList.lh_TailPred = (struct Node *)&a314_mp->mp_MsgList.lh_Head;

    a314_ior = (struct A314_IORequest *)AllocMem(
        sizeof(struct A314_IORequest), MEMF_PUBLIC | MEMF_CLEAR);
    if (!a314_ior) {
        FreeSignal(a314_sigbit); a314_sigbit = -1;
        FreeMem(a314_mp, sizeof(struct MsgPort)); a314_mp = NULL;
        return FALSE;
    }

    a314_ior->a314_Request.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    a314_ior->a314_Request.io_Message.mn_ReplyPort    = a314_mp;
    a314_ior->a314_Request.io_Message.mn_Length       = sizeof(struct A314_IORequest);

    if (OpenDevice((STRPTR)a314_name, 0, (struct IORequest *)a314_ior, 0) != 0) {
        FreeMem(a314_ior, sizeof(struct A314_IORequest)); a314_ior = NULL;
        FreeSignal(a314_sigbit); a314_sigbit = -1;
        FreeMem(a314_mp, sizeof(struct MsgPort)); a314_mp = NULL;
        return FALSE;
    }
    return TRUE;
}

static void a314_close(void)
{
    if (a314_ior) {
        CloseDevice((struct IORequest *)a314_ior);
        FreeMem(a314_ior, sizeof(struct A314_IORequest));
        a314_ior = NULL;
    }
    if (a314_sigbit != -1) { FreeSignal(a314_sigbit); a314_sigbit = -1; }
    if (a314_mp) { FreeMem(a314_mp, sizeof(struct MsgPort)); a314_mp = NULL; }
}

/* One bounded connect attempt (~4s max). NEVER blocks forever - if a314d/the
 * link isn't ready yet (cold boot: the Pi powers up after the Amiga) we abort
 * and the caller retries, instead of the old DoIO that could hang the diff
 * process indefinitely and wedge it (leaving diff_running stuck TRUE). */
static BOOL a314_connect_rtg(void)
{
    int i;
    a314_socket = (ULONG)a314_mp;          /* unique per session            */
    a314_ior->a314_Request.io_Command = A314_CONNECT;
    a314_ior->a314_Request.io_Error   = 0;
    a314_ior->a314_Socket             = a314_socket;
    a314_ior->a314_Buffer             = (STRPTR)rtg_svc;
    a314_ior->a314_Length             = 3;
    SendIO((struct IORequest *)a314_ior);
    for (i = 0; i < 20; i++) {             /* up to ~4s */
        if (CheckIO((struct IORequest *)a314_ior)) break;
        Delay(10);
    }
    if (!CheckIO((struct IORequest *)a314_ior)) {
        AbortIO((struct IORequest *)a314_ior);
        WaitIO((struct IORequest *)a314_ior);
        return FALSE;                      /* timed out - caller may retry */
    }
    WaitIO((struct IORequest *)a314_ior);
    return a314_ior->a314_Request.io_Error == A314_CONNECT_OK;
}

/* Returns 0 on success, non-zero if the stream was reset (connection dead). */
static int a314_write(UBYTE *buf, WORD len)
{
    a314_ior->a314_Request.io_Command = A314_WRITE;
    a314_ior->a314_Request.io_Error   = 0;
    a314_ior->a314_Socket             = a314_socket;
    a314_ior->a314_Buffer             = (STRPTR)buf;
    a314_ior->a314_Length             = len;
    DoIO((struct IORequest *)a314_ior);
    return (a314_ior->a314_Request.io_Error == A314_WRITE_OK) ? 0 : 1;
}

/* ---- Diff process ------------------------------------------------------- */

static UWORD diff_y = 0;            /* round-robin resume row                */

static void zero_shadow(void)
{
    ULONG *p = (ULONG *)shadow;
    ULONG  n = FB_SIZE / 4;
    while (n--) *p++ = 0;
    mark_all_rows();          /* everything must be re-sent */
}

static int send_setmode(void)
{
    UBYTE pkt[6];
    pkt[0] = CMD_SETMODE;
    PW(pkt, 1, mode_w);
    PW(pkt, 3, mode_h);
    pkt[5] = mode_bpp;
    return a314_write(pkt, 6);
}

static int send_setswitch(void)
{
    UBYTE pkt[2];
    pkt[0] = CMD_SETSWITCH;
    pkt[1] = switch_state ? 1 : 0;
    return a314_write(pkt, 2);
}

/* Diagnostic pass-through wrapper around P96's software FillRect: record the
 * RenderInfo geometry P96 itself renders with, then chain to the original. */
typedef void (*fillrect_fn)(struct BoardInfo *bi __asm("a0"),
                            struct RenderInfo *ri __asm("a1"),
                            WORD x __asm("d0"), WORD y __asm("d1"),
                            WORD w __asm("d2"), WORD h __asm("d3"),
                            ULONG color __asm("d4"), UBYTE mask __asm("d5"),
                            RGBFTYPE fmt __asm("d7"));
static fillrect_fn orig_FillRect = NULL;

static void my_FillRect(struct BoardInfo *bi __asm("a0"),
                        struct RenderInfo *ri __asm("a1"),
                        WORD x __asm("d0"), WORD y __asm("d1"),
                        WORD w __asm("d2"), WORD h __asm("d3"),
                        ULONG color __asm("d4"), UBYTE mask __asm("d5"),
                        RGBFTYPE fmt __asm("d7"))
{
    if (ri) {
        if ((UWORD)ri->BytesPerRow != ri_bpr ||
            (ULONG)ri->Memory      != ri_mem) {
            ri_bpr    = (UWORD)ri->BytesPerRow;
            ri_mem    = (ULONG)ri->Memory;
            dbg_dirty = TRUE;
        }
        /* THE AUTHORITATIVE STRIDE (found 2026-07-24): P96's own renderers
         * used 5120 B/row here while every derivation from SetPanning's width
         * guessed 1280 or 2560. Whenever P96 renders into the visible bitmap,
         * adopt its stride outright - the diff must read what P96 wrote. The
         * bpr-change detection in diff_pass() then re-diffs everything. */
        if ((UBYTE *)ri->Memory == visible_base && ri->BytesPerRow > 0 &&
            (UWORD)ri->BytesPerRow != mode_bpr)
            mode_bpr = (UWORD)ri->BytesPerRow;
    }
    orig_FillRect(bi, ri, x, y, w, h, color, mask, fmt);
}

/* Ground-truth telemetry for the Pi journal - what the card actually decided
 * about geometry/format. Sent after every SETMODE. */
static int send_debug(void)
{
    ULONG vb = (ULONG)visible_base, fb = (ULONG)framebuffer;
    UBYTE pkt[22];
    pkt[0] = CMD_DEBUG;
    PW(pkt, 1, pan_width);
    PW(pkt, 3, mode_bpr);
    pkt[5] = mode_rgbf;
    pkt[6] = mode_bpp;
    pkt[7] = (UBYTE)((mmu_active ? 1 : 0) | (mode_swap ? 2 : 0) |
                     (mmu_stage << 4));
    PW(pkt,  8, (UWORD)(vb >> 16));  PW(pkt, 10, (UWORD)vb);
    PW(pkt, 12, (UWORD)(fb >> 16));  PW(pkt, 14, (UWORD)fb);
    PW(pkt, 16, ri_bpr);
    PW(pkt, 18, (UWORD)(ri_mem >> 16)); PW(pkt, 20, (UWORD)ri_mem);
    return a314_write(pkt, 22);
}

/* Push the whole 256-entry CLUT (for 8-bit modes), chunked under the 252-byte
 * packet cap. Returns non-zero if the connection died. */
static int send_palette(void)
{
    UBYTE pkt[3 + PAL_PER_PKT * 2];
    UWORD start = 0;
    while (start < 256) {
        UWORD cnt = 256 - start, b;
        if (cnt > PAL_PER_PKT) cnt = PAL_PER_PKT;
        pkt[0] = CMD_SETPALETTE;
        pkt[1] = (UBYTE)start;
        pkt[2] = (UBYTE)cnt;
        for (b = 0; b < cnt * 2; b++) pkt[3 + b] = pal[start * 2 + b];
        if (a314_write(pkt, (WORD)(3 + cnt * 2))) return 1;
        start += cnt;
    }
    return 0;
}

/* 16-bit (R5G6B5) bounded diff of ONE row. Decrements *budget by bytes sent.
 * Returns non-zero if the connection died. */
static int diff_row16(UWORD y, UWORD w, UWORD bpr, UBYTE *vbase, LONG *budget)
{
    UBYTE *frow = vbase  + mul16(y, bpr);       /* bitmap stride       */
    UBYTE *srow = shadow + mul16(y, w * 2);     /* shadow: packed rows */
    UWORD  col  = 0;
    UBYTE  pkt[256];

    while (col < w) {
        UWORD c0, c1, seg, first, i;
        BOOL  uniform;

        /* skip matching pixels */
        while (col < w &&
               *(UWORD *)(frow + col*2) == *(UWORD *)(srow + col*2))
            col++;
        if (col >= w) break;
        c0 = col;

        /* extend across the changed run */
        while (col < w &&
               *(UWORD *)(frow + col*2) != *(UWORD *)(srow + col*2))
            col++;
        c1  = col;
        seg = c1 - c0;

        /* uniform-colour test (for FILL compression) */
        first   = *(UWORD *)(frow + c0*2);
        uniform = TRUE;
        for (i = c0; i < c1; i++) {
            if (*(UWORD *)(frow + i*2) != first) { uniform = FALSE; break; }
        }

        if (uniform && seg >= FILL_MIN_RUN) {
            pkt[0] = CMD_FILL;
            PW(pkt, 1, c0);
            PW(pkt, 3, y);
            PW(pkt, 5, seg);
            if (mode_swap) {                  /* BE mode: swap on the wire */
                pkt[7] = frow[c0*2 + 1];
                pkt[8] = frow[c0*2];
            } else {
                pkt[7] = frow[c0*2];
                pkt[8] = frow[c0*2 + 1];
            }
            if (a314_write(pkt, 9)) return 1;
            *budget -= 9;
            for (i = c0; i < c1; i++) {       /* shadow = frame bytes */
                srow[i*2]     = frow[c0*2];
                srow[i*2 + 1] = frow[c0*2 + 1];
            }
        } else {
            i = c0;
            while (i < c1) {
                UWORD n = c1 - i;
                UWORD b;
                if (n > MAX_PIX_PER_PKT) n = MAX_PIX_PER_PKT;
                pkt[0] = CMD_PIXELS;
                PW(pkt, 1, i);
                PW(pkt, 3, y);
                pkt[5] = (UBYTE)n;
                if (mode_swap) {              /* BE mode: swap on the wire */
                    for (b = 0; b < n*2; b += 2) {
                        pkt[6 + b]     = frow[i*2 + b + 1];
                        pkt[6 + b + 1] = frow[i*2 + b];
                    }
                } else {
                    for (b = 0; b < n*2; b++) pkt[6 + b] = frow[i*2 + b];
                }
                if (a314_write(pkt, (WORD)(6 + n*2))) return 1;
                *budget -= 6 + n*2;
                for (b = 0; b < n*2; b++) srow[i*2 + b] = frow[i*2 + b];
                i += n;
            }
        }

        if (*budget <= 0) return 0;   /* resume this row next pass */
    }
    return 0;
}

/* 8-bit (CLUT index) bounded diff of ONE row. Non-zero => connection died. */
static int diff_row8(UWORD y, UWORD w, UWORD bpr, UBYTE *vbase, LONG *budget)
{
    UBYTE *frow = vbase  + mul16(y, bpr);       /* bitmap stride       */
    UBYTE *srow = shadow + mul16(y, w);         /* shadow: packed rows */
    UWORD  col  = 0;
    UBYTE  pkt[256];

    while (col < w) {
        UWORD c0, c1, seg, i;
        UBYTE first;
        BOOL  uniform;

        while (col < w && frow[col] == srow[col]) col++;
        if (col >= w) break;
        c0 = col;
        while (col < w && frow[col] != srow[col]) col++;
        c1  = col;
        seg = c1 - c0;

        first   = frow[c0];
        uniform = TRUE;
        for (i = c0; i < c1; i++)
            if (frow[i] != first) { uniform = FALSE; break; }

        if (uniform && seg >= FILL_MIN_RUN) {
            pkt[0] = CMD_FILL8;
            PW(pkt, 1, c0);
            PW(pkt, 3, y);
            PW(pkt, 5, seg);
            pkt[7] = first;
            if (a314_write(pkt, 8)) return 1;
            *budget -= 8;
            for (i = c0; i < c1; i++) srow[i] = first;
        } else {
            i = c0;
            while (i < c1) {
                UWORD n = c1 - i, b;
                if (n > MAX_IDX_PER_PKT) n = MAX_IDX_PER_PKT;
                pkt[0] = CMD_PIXELS8;
                PW(pkt, 1, i);
                PW(pkt, 3, y);
                pkt[5] = (UBYTE)n;
                for (b = 0; b < n; b++) pkt[6 + b] = frow[i + b];
                if (a314_write(pkt, (WORD)(6 + n))) return 1;
                *budget -= 6 + n;
                for (b = 0; b < n; b++) srow[i + b] = frow[i + b];
                i += n;
            }
        }
        if (*budget <= 0) return 0;
    }
    return 0;
}

/* One bounded diff pass. Non-zero => connection died.
 *
 * With mmu_active: ask the MMU which pages of the visible window were written
 * since last pass and only re-diff the rows they cover; everything else costs
 * one flag check per row. A periodic reconciliation sweep plus pan/stride
 * change detection guarantee convergence even if the M-bit ever lies.
 * Without mmu_active: every row is marked dirty every pass = the original
 * full-scan behaviour, unchanged.
 *
 * 16-bit-only known-good baseline: 8-bit kept, 32-bit deliberately not
 * mirrored (the 32-bit conversion path was an unverified detour - 2026-06-01). */
static int diff_pass(void)
{
    UWORD  w = mode_w, h = mode_h, bpr = mode_bpr;
    UBYTE *vbase = visible_base;
    LONG   budget = MAX_BYTES_PER_PASS;
    UWORD  count;

    if (!w || !h || !bpr || !vbase) return 0;
    if (w > FB_WIDTH_MAX || h > FB_HEIGHT_MAX) return 0;
    if (mode_bpp != 16 && mode_bpp != 8) return 0;  /* other depths not mirrored */
    /* The shadow is packed at visible width (w*bytes/px per row) so a WIDE
     * P96 bitmap allocation can't overrun it; only the frame read uses the
     * real stride. Just make sure that read stays inside card memory. */
    if (mul16(w, bpp_bytes(mode_bpp)) > bpr) return 0;    /* nonsense stride */
    if ((ULONG)vbase - (ULONG)framebuffer + mul16(h, bpr) > CARD_MEM_SIZE)
        return 0;

    /* The shadow is screen content keyed by row/col; the MMU sees addresses.
     * After a pan or stride change, page dirt no longer maps to screen dirt -
     * re-diff everything once. */
    if ((ULONG)vbase != last_vbase || bpr != last_bpr) {
        last_vbase = (ULONG)vbase;
        last_bpr   = bpr;
        mark_all_rows();
    }

    if (mmu_active) {
        ULONG end = (ULONG)vbase + mul16(h, bpr);
        ULONG pg  = (ULONG)vbase & ~(mmu_pgsz - 1);
        for (; pg < end; pg += mmu_pgsz) {
            if (mmu_GetPageUsedModified(mmu_ctx, pg) & MAPP_MODIFIED) {
                LONG r0 = ((LONG)(pg - (ULONG)vbase)) / (LONG)bpr;
                LONG r1 = ((LONG)(pg + mmu_pgsz - 1 - (ULONG)vbase)) / (LONG)bpr;
                if (r0 < 0)        r0 = 0;
                if (r1 >= (LONG)h) r1 = (LONG)h - 1;
                for (; r0 <= r1; r0++) dirty_rows[r0] = 1;
            }
        }
        if (++recon_countdown >= RECON_EVERY) {  /* safety net vs missed dirt */
            recon_countdown = 0;
            mark_all_rows();
        }
    } else {
        mark_all_rows();       /* no MMU: re-scan all rows, as before */
    }

    for (count = 0; count < h; count++) {
        UWORD y = diff_y;
        if (dirty_rows[y]) {
            int dead = (mode_bpp == 16)
                     ? diff_row16(y, w, bpr, vbase, &budget)
                     : diff_row8 (y, w, bpr, vbase, &budget);
            if (dead) return 1;
            if (budget <= 0) return 0;   /* resume at this row next pass */
            dirty_rows[y] = 0;
        }
        if (++diff_y >= h) diff_y = 0;
    }
    return 0;
}

static void diff_task(void)
{
    int tries;

    if (!a314_open())        { diff_running = FALSE; return; }

    /* Retry the connect for ~30s: on a cold boot the Pi (powered by the Amiga)
     * comes up after us, so a314d / the "rtg" service may not be ready at the
     * instant the screen first opens. Each attempt is bounded (~4s) so we never
     * hang. Give up cleanly after that so a later screen-open can try again. */
    for (tries = 0; tries < 8; tries++) {
        if (a314_connect_rtg()) break;
        Delay(25);            /* ~0.5s between attempts */
    }
    if (tries >= 8) { a314_close(); diff_running = FALSE; return; }

    mmu_init();              /* optional MMU dirty-tracking fast path */
    zero_shadow();           /* force a full first push */
    mode_changed  = FALSE;
    if (send_setmode())   goto done;
    if (send_debug())     goto done;
    switch_changed = FALSE;
    if (send_setswitch()) goto done;

    for (;;) {
        if (mode_changed)    { mode_changed = FALSE;
                               if (send_setmode()) break;
                               if (send_debug())   break;
                               zero_shadow(); }
        if (palette_changed) { palette_changed = FALSE;
                               if (send_palette()) break;
                               zero_shadow();   /* re-resolve pixels vs new CLUT */ }
        if (switch_changed)  { switch_changed = FALSE;
                               if (send_setswitch()) break; }
        if (dbg_dirty)       { dbg_dirty = FALSE;
                               if (send_debug()) break; }
        if (diff_pass()) break;
        Delay(2);            /* ~40 ms between polls */
    }
done:
    a314_close();
    diff_running = FALSE;
}

static void ensure_diff_started(void)
{
    struct TagItem tags[5];

    if (diff_running) return;
    if (!DOSBase) {
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 0);
        if (!DOSBase) return;
    }
    diff_running = TRUE;     /* claim before spawning to avoid double-start */

    tags[0].ti_Tag = NP_Entry;     tags[0].ti_Data = (ULONG)diff_task;
    tags[1].ti_Tag = NP_Name;      tags[1].ti_Data = (ULONG)"a314rtg.diff";
    tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = 20480;
    tags[3].ti_Tag = NP_Priority;  tags[3].ti_Data = (ULONG)0;
    tags[4].ti_Tag = TAG_DONE;     tags[4].ti_Data = 0;

    if (!CreateNewProc(tags)) diff_running = FALSE;
}

/* ---- Display / mode callbacks ------------------------------------------- */

static BOOL SetSwitch(
    struct BoardInfo *bi __asm("a0"),
    BOOL state           __asm("d0"))
{
    (void)bi;
    switch_state   = state ? TRUE : FALSE;
    switch_changed = TRUE;
    return TRUE;
}

static void SetColorArray(
    struct BoardInfo *bi __asm("a0"),
    UWORD start          __asm("d0"),
    UWORD count          __asm("d1"))
{
    /* P96 has filled bi->CLUT[start..start+count-1] (8-bit R/G/B). Convert to
     * RGB565 little-endian and stash for the diff process to push to the Pi. */
    UWORD i, end = start + count;
    if (end > 256) end = 256;
    for (i = start; i < end; i++) {
        UBYTE r = bi->CLUT[i].Red, g = bi->CLUT[i].Green, b = bi->CLUT[i].Blue;
        UWORD v = (UWORD)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        pal[i * 2]     = (UBYTE)(v & 0xFF);   /* low byte first (LE, like fb0) */
        pal[i * 2 + 1] = (UBYTE)(v >> 8);
    }
    palette_changed = TRUE;
}

static void SetDAC(
    struct BoardInfo *bi __asm("a0"),
    UWORD format         __asm("d0"),
    RGBFTYPE rgbf        __asm("d7"))
{
    (void)bi; (void)format; (void)rgbf;
}

static void SetGC(
    struct BoardInfo *bi __asm("a0"),
    struct ModeInfo  *mi __asm("a1"),
    BOOL border          __asm("d0"))
{
    (void)bi; (void)border;
    mode_w    = mi->Width;
    mode_h    = mi->Height;
    mode_bpp  = (UBYTE)mi->Depth;
    mode_bpr  = (UWORD)((mi->Width * bpp_bytes(mi->Depth) + 63) & ~63);
    if (!visible_base) visible_base = framebuffer;  /* until SetPanning runs */
    mode_changed = TRUE;
    /* Start the Pi-streaming diff process the first time a screen opens on the
     * board. (The #80000004 we once blamed on CreateNewProc here was actually
     * the romtag RTF_AUTOINIT bug crashing before any callback ran, so this is
     * safe: SetGC runs in the screen-opening Process's context.) */
    ensure_diff_started();
}

static void SetPanning(
    struct BoardInfo *bi __asm("a0"),
    UBYTE *mem           __asm("a1"),
    UWORD width          __asm("d0"),
    UWORD xoffset        __asm("d3"),
    WORD  yoffset        __asm("d1"),
    WORD  unused         __asm("d2"),
    RGBFTYPE rgbf        __asm("d7"))
{
    (void)bi; (void)xoffset; (void)yoffset; (void)unused;
    /* Big-endian 16-bit formats need their pixel bytes swapped on the wire
     * (fb0 on the Pi is little-endian RGB565). The PC formats stream as-is. */
    mode_swap = (rgbf == RGBFB_R5G6B5 || rgbf == RGBFB_R5G5B5) ? TRUE : FALSE;
    mode_rgbf = (UBYTE)rgbf;
    /* `width` is the allocated bitmap's row width in PIXELS (proven 2026-07-24
     * by CMD_DEBUG telemetry: width=1280 for a 640-visible 16-bit screen whose
     * real stride is 2560 bytes - P96 allocated the bitmap double-wide). The
     * stride is width * bytes-per-pixel of the mode's RGBFTYPE. The old
     * "width is already bytes" reading only held when the bitmap width matched
     * the visible width. Safe now: the shadow is packed at visible width, so a
     * wide stride can't overrun it (that was the June crash). */
    if (width) {
        UWORD pxbytes;
        switch (rgbf) {
        case RGBFB_NONE: case RGBFB_CLUT:                     pxbytes = 1; break;
        case RGBFB_R8G8B8: case RGBFB_B8G8R8:                 pxbytes = 3; break;
        case RGBFB_A8R8G8B8: case RGBFB_A8B8G8R8:
        case RGBFB_R8G8B8A8: case RGBFB_B8G8R8A8:             pxbytes = 4; break;
        default:                                              pxbytes = 2; break;
        }
        pan_width = width;
        mode_bpr  = (UWORD)mul16(width, pxbytes);
    }
    if (mem) { visible_base = mem; mode_changed = TRUE; }
}

static UWORD CalculateBytesPerRow(
    struct BoardInfo *bi __asm("a0"),
    UWORD width          __asm("d0"),
    UWORD depth          __asm("d1"),
    struct ModeInfo  *mi __asm("a1"),
    RGBFTYPE rgbf        __asm("d7"))
{
    UWORD bpr;
    (void)bi; (void)mi; (void)rgbf;
    bpr = (UWORD)(width * bpp_bytes(depth));
    return (bpr + 63) & ~63;   /* align to 64 bytes */
}

static BOOL SetDisplay(
    struct BoardInfo *bi __asm("a0"),
    BOOL state           __asm("d0"))
{
    (void)bi; (void)state;
    return TRUE;
}

static LONG ResolvePixelClock(
    struct BoardInfo *bi __asm("a0"),
    struct ModeInfo  *mi __asm("a1"),
    ULONG clock          __asm("d0"),
    RGBFTYPE rgbf        __asm("d7"))
{
    (void)bi; (void)clock; (void)rgbf;
    /* Dumb framebuffer, no real CRTC: report a fixed nominal clock (the 18:40
     * known-good value). The Pi scans out at its own rate regardless. */
    mi->PixelClock       = 25000000;
    mi->pll1.Clock       = 0;
    mi->pll2.ClockDivide = 1;
    return 0;
}

static ULONG GetPixelClock(
    struct BoardInfo *bi __asm("a0"),
    struct ModeInfo  *mi __asm("a1"),
    ULONG index          __asm("d0"),
    RGBFTYPE rgbf        __asm("d7"))
{
    (void)bi; (void)mi; (void)index; (void)rgbf;
    return 25000000;   /* nominal; the 18:40 known-good value */
}

static void SetClock(struct BoardInfo *bi __asm("a0")) { (void)bi; }

static void WaitVerticalSync(
    struct BoardInfo *bi __asm("a0"),
    BOOL state           __asm("d0"))
{
    (void)bi; (void)state;
}

/* Mandatory: P96 polls this for the current vertical-sync state. We have no
 * real CRTC; return the polled-for state so any P96 wait-loop exits at once
 * (matches the ZZ9000 driver's no-register fallback). */
static BOOL GetVSyncState(
    struct BoardInfo *bi __asm("a0"),
    BOOL expected        __asm("d0"))
{
    (void)bi;
    return expected;
}

static void SetSplitPosition(
    struct BoardInfo *bi __asm("a0"),
    WORD pos             __asm("d0"))
{
    (void)bi; (void)pos;
}

static void SetMemoryMode(
    struct BoardInfo *bi __asm("a0"),
    RGBFTYPE rgbf        __asm("d7"))
{
    (void)bi; (void)rgbf;
}

static void SetWriteMask(
    struct BoardInfo *bi __asm("a0"),
    UBYTE mask           __asm("d0"))
{
    (void)bi; (void)mask;
}

static void SetClearMask(
    struct BoardInfo *bi __asm("a0"),
    UBYTE mask           __asm("d0"))
{
    (void)bi; (void)mask;
}

static void SetReadPlane(
    struct BoardInfo *bi __asm("a0"),
    UBYTE plane          __asm("d0"))
{
    (void)bi; (void)plane;
}

static void WaitBlitter(struct BoardInfo *bi __asm("a0")) { (void)bi; }

static BOOL SetInterrupt(
    struct BoardInfo *bi __asm("a0"),
    BOOL state           __asm("d0"))
{
    (void)bi; (void)state;
    return FALSE;
}

static ULONG GetCompatibleFormats(
    struct BoardInfo *bi __asm("a0"),
    RGBFTYPE rgbf        __asm("d7"))
{
    (void)bi; (void)rgbf;
    /* Accept every format we advertise (ZZ9000 returns ~0 here). Returning
     * only R5G6B5PC made P96 VETO screen opens on the plain big-endian
     * "16bit" board modes: it silently fell back to a native 8-bit screen
     * and SetGC never ran - found 2026-07-24 with rtgshow's actual-ModeID/
     * depth report. The diff handles byte order itself (mode_swap). */
    return RGBFF_HICOLOR | RGBFF_TRUECOLOR | RGBFF_TRUEALPHA |
           RGBFF_CLUT | RGBFF_NONE;
}

static APTR CalculateMemory(
    struct BoardInfo *bi    __asm("a0"),
    APTR mem                __asm("a1"),
    struct RenderInfo *ri   __asm("d0"),
    RGBFTYPE rgbf           __asm("d7"))
{
    (void)bi; (void)ri; (void)rgbf;
    return mem;
}

/* ---- Main entry points --------------------------------------------------
 *
 * This is a "dumb framebuffer" card: it has no hardware acceleration, so every
 * rendering callback is pointed at P96's own software *Default implementation
 * (P96 pre-fills the *Default fields before InitCard). Those render with the
 * CPU straight into the card framebuffer; the diff process then mirrors the
 * result to the Pi. Leaving any of these NULL crashes P96 the moment it tries
 * to use that operation (e.g. BlitTemplate for text), so they must ALL be set.
 */

/*
 * FindCard: confirm the card is present and claimable; allocate framebuffer
 * and shadow. A314 connection is deferred to the diff process (started in
 * SetGC, from user-task context).
 */
/*
 * Reserve a dedicated card-memory region by shrinking a FastRAM MemHeader from
 * the top, so the region ends up OUTSIDE the system memory pool (no MemHeader
 * covers it) - exactly like a real card's VRAM. rtg.library appears to require
 * this to build a board context; plain AllocMem'd RAM is rejected. Returns NULL
 * if no FastRAM header has `size` free at its very top (caller falls back).
 */
/* ---- Serial debug (kprintf) — AmigaOS 3.2 captures this to RAM:debug.log -- */
asm(
"   .text                        \n"
"   .globl _a314rtg_rawputch     \n"
"_a314rtg_rawputch:              \n"   /* RawDoFmt PutChProc: d0=char, a3=data */
"   move.l  a6,-(sp)             \n"
"   move.l  a3,-(sp)             \n"
"   move.l  4.w,a6               \n"   /* SysBase */
"   jsr     -516(a6)             \n"   /* RawPutChar(d0) */
"   move.l  (sp)+,a3             \n"
"   move.l  (sp)+,a6             \n"
"   rts                          \n"
);
extern void a314rtg_rawputch(void);

/* C-callable single-char emit: arg on stack, char -> d0, RawPutChar(d0). */
asm(
"   .text                        \n"
"   .globl _a314rtg_putc         \n"
"_a314rtg_putc:                  \n"
"   move.l  a6,-(sp)             \n"
"   move.l  8(sp),d0             \n"   /* arg c (ret + saved a6 above it) */
"   move.l  4.w,a6               \n"   /* SysBase */
"   jsr     -516(a6)             \n"   /* RawPutChar(d0) */
"   move.l  (sp)+,a6             \n"
"   rts                          \n"
);
extern void a314rtg_putc(int c);

static void dbgf(CONST_STRPTR fmt, ...)
{
    RawDoFmt(fmt, (APTR)((&fmt) + 1),
             (void (*)())a314rtg_rawputch, (APTR)0);
}

static UBYTE *carve_card_mem(ULONG size)
{
    UBYTE *block;
    struct MemHeader *mh, *upper;
    struct MemChunk *mc, *next;
    struct MemChunk *lowhead = NULL, *lowtail = NULL;
    struct MemChunk *uphead  = NULL, *uptail  = NULL;
    ULONG a, aend, orig_upper, lowfree = 0, upfree = 0;
    BOOL  used_upper = FALSE;

    size = (size + 0x1FUL) & ~0x1FUL;                    /* 32-byte align */

    /* Grab an exclusively-owned block from the big FAST pool, then surgically
     * remove its address range from the system memory list so no MemHeader
     * covers it - leaving it "dedicated", off-pool, like a real card's VRAM.
     * Both allocations happen BEFORE Forbid() (AllocMem inside Forbid silently
     * fails, which is why the previous carve never engaged). */
    block = (UBYTE *)AllocMem(size, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    if (!block) return NULL;
    upper = (struct MemHeader *)AllocMem(sizeof(struct MemHeader),
                                         MEMF_PUBLIC | MEMF_CLEAR);
    a = (ULONG)block; aend = a + size;
    dbgf((CONST_STRPTR)"a314rtg carve: block=%08lx upper=%08lx size=%ld\n",
         (ULONG)block, (ULONG)upper, size);

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ != NULL;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        if (a >= (ULONG)mh->mh_Lower && aend <= (ULONG)mh->mh_Upper)
            break;
    if (mh->mh_Node.ln_Succ == NULL) {                   /* not found */
        Permit();
        dbgf((CONST_STRPTR)"a314rtg carve: NO MEMHEADER contains block - in pool\n");
        if (upper) FreeMem(upper, sizeof(struct MemHeader));
        return block;
    }

    orig_upper = (ULONG)mh->mh_Upper;
    dbgf((CONST_STRPTR)"a314rtg carve: mh=%08lx lo=%08lx up=%08lx attr=%04lx\n",
         (ULONG)mh, (ULONG)mh->mh_Lower, orig_upper, (ULONG)mh->mh_Attributes);
    if (aend < orig_upper && !upper) {                   /* need split but no header */
        Permit();
        dbgf((CONST_STRPTR)"a314rtg carve: need upper header but AllocMem gave NULL\n");
        return block;                                    /* fall back: in-pool */
    }

    /* Split free chunks: below the block stay in mh, above go to `upper`.
     * (No chunk straddles the block - it's allocated, so AllocMem already
     * split the surrounding free chunks at a and aend.) */
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
    mh->mh_Upper = (APTR)a;                               /* mh now ends at block */

    if (aend < orig_upper) {                              /* memory above block */
        upper->mh_Node.ln_Type = NT_MEMORY;
        upper->mh_Node.ln_Pri  = mh->mh_Node.ln_Pri;
        upper->mh_Node.ln_Name = mh->mh_Node.ln_Name;
        upper->mh_Attributes   = mh->mh_Attributes;
        upper->mh_First        = uphead;
        upper->mh_Lower        = (APTR)aend;              /* upper starts past block */
        upper->mh_Upper        = (APTR)orig_upper;
        upper->mh_Free         = upfree;
        Enqueue(&SysBase->MemList, (struct Node *)upper);
        used_upper = TRUE;
    }
    Permit();
    dbgf((CONST_STRPTR)"a314rtg carve: SPLIT OK a=%08lx aend=%08lx (off-pool now)\n",
         a, aend);

    if (upper && !used_upper)                             /* block reached the top */
        FreeMem(upper, sizeof(struct MemHeader));
    return block;
}

BOOL FindCard(struct BoardInfo *bi __asm("a0"))
{
    if (!SysBase) SysBase = *(struct ExecBase **)4L;
    /* dead-simple raw markers (bypass RawDoFmt) to prove the channel works */
    a314rtg_putc('<'); a314rtg_putc('F'); a314rtg_putc('C'); a314rtg_putc('>');
    a314rtg_putc('\r'); a314rtg_putc('\n');
    dbgf((CONST_STRPTR)"a314rtg: FindCard enter bi=%08lx\n", (ULONG)bi);

    if (!framebuffer) {
        framebuffer = carve_card_mem(CARD_MEM_SIZE);   /* dedicated, off-pool */
        if (!framebuffer) {
            dbgf((CONST_STRPTR)"a314rtg: FindCard AllocMem FAILED -> FALSE\n");
            return FALSE;
        }
        /* The carve is raw RAM at the SAME address every boot - it still
         * holds every previous session's frames ("ghosts"). Any stride
         * mismatch then reads old sessions' pixels as if they were current.
         * Start clean. */
        {
            ULONG *p = (ULONG *)framebuffer;
            ULONG  n = CARD_MEM_SIZE / 4;
            while (n--) *p++ = 0;
        }
    }
    if (!shadow) {
        shadow = (UBYTE *)AllocMem(FB_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        if (!shadow) return FALSE;
    }
    bi->RegisterBase = framebuffer;   /* non-NULL; see InitCard */
    bi->MemoryBase   = framebuffer;
    bi->MemorySize   = CARD_MEM_SIZE;
    dbgf((CONST_STRPTR)"a314rtg: FindCard fb=%08lx size=%ld -> TRUE\n",
         (ULONG)framebuffer, (ULONG)CARD_MEM_SIZE);
    return TRUE;
}

static const char BoardNameStr[] = "A314RTG";

BOOL InitCard(
    struct BoardInfo *bi  __asm("a0"),
    STRPTR *toolTypes     __asm("a1"))
{
    (void)toolTypes;

    if (!SysBase) SysBase = *(struct ExecBase **)4L;
    a314rtg_putc('<'); a314rtg_putc('I'); a314rtg_putc('C'); a314rtg_putc('>');
    a314rtg_putc('\r'); a314rtg_putc('\n');
    dbgf((CONST_STRPTR)"a314rtg: InitCard enter bi=%08lx\n", (ULONG)bi);

    {
        /* Per-mode capability limits. rtg.library needs these non-zero or it
         * creates NO screen modes -> "could not create board context".
         * (PiStorm's pigfx sets these for exactly this reason.) */
        int i;
        for (i = 0; i < MAXMODES; i++) {
            bi->MaxHorValue[i]      = FB_WIDTH_MAX;
            bi->MaxVerValue[i]      = FB_HEIGHT_MAX;
            bi->MaxHorResolution[i] = FB_WIDTH_MAX;
            bi->MaxVerResolution[i] = FB_HEIGHT_MAX;
            bi->PixelClockCount[i]  = 1;
        }
    }

    bi->CardBase               = (struct CardBase *)_base;
    bi->ExecBase               = SysBase;
    bi->BoardName              = (char *)BoardNameStr;
    bi->BoardType              = BT_uaegfx;    /* virtual/software RTG board type
                                                * (UAE's RTG / PiStorm pigfx) */
    bi->PaletteChipType        = PCT_S3ViRGE;  /* match PiStorm's recognised */
    bi->GraphicsControllerType = GCT_S3ViRGE;  /* chip/controller identity     */

    bi->RegisterBase           = framebuffer;  /* non-NULL (PiStorm/ZZ9000 set
                                                * this; rtg.library wants it).
                                                * We never poke it directly. */
    bi->MemoryBase             = framebuffer;
    bi->MemorySize             = CARD_MEM_SIZE;
    bi->MaxMemorySize          = CARD_MEM_SIZE;
    bi->MaxChunkSize           = CARD_MEM_SIZE;
    bi->MemorySpaceBase        = framebuffer;
    bi->MemorySpaceSize        = CARD_MEM_SIZE;
    bi->MemoryClock            = 100000000;   /* nominal; we have no real CRTC */

    bi->BitsPerCannon          = 8;
    /* Advertise the full set (like PiStorm) so rtg.library can build the base
     * mode list. Our diff only handles 16-bit R5G6B5PC, so it skips any non-
     * 16-bit mode the user might pick. */
    bi->RGBFormats             = RGBFF_HICOLOR | RGBFF_TRUECOLOR |
                                 RGBFF_TRUEALPHA | RGBFF_CLUT | RGBFF_NONE;
    bi->Flags                 |= BIF_GRANTDIRECTACCESS;

    /* No AllocCardMem/FreeCardMem override: let rtg.library manage the card
     * memory region itself (so off-screen bitmaps get distinct addresses).
     * The visible screen's address arrives via SetPanning. */
    bi->SetSwitch            = SetSwitch;
    bi->SetColorArray        = SetColorArray;
    bi->SetDAC               = SetDAC;
    bi->SetGC                = SetGC;
    bi->SetPanning           = SetPanning;
    bi->CalculateBytesPerRow = CalculateBytesPerRow;
    bi->CalculateMemory      = CalculateMemory;
    bi->GetCompatibleFormats = GetCompatibleFormats;
    bi->SetDisplay           = SetDisplay;
    bi->ResolvePixelClock    = ResolvePixelClock;
    bi->GetPixelClock        = GetPixelClock;
    bi->SetClock             = SetClock;
    bi->SetMemoryMode        = SetMemoryMode;
    bi->SetWriteMask         = SetWriteMask;
    bi->SetClearMask         = SetClearMask;
    bi->SetReadPlane         = SetReadPlane;
    bi->WaitVerticalSync     = WaitVerticalSync;
    bi->GetVSyncState        = GetVSyncState;
    bi->WaitBlitter          = WaitBlitter;
    bi->SetInterrupt         = SetInterrupt;
    bi->SetSplitPosition     = SetSplitPosition;

    /* Rendering callbacks (FillRect, BlitRect, BlitTemplate, DrawLine,
     * ScrollPlanar, BlitPlanar2Chunky, ...) are deliberately NOT set: P96
     * pre-fills them with its own software implementations before InitCard,
     * which render straight into our framebuffer. We accelerate nothing, so we
     * override nothing. (Touching them here risks copying a wrong-offset value
     * into a live pointer -> jump-through-garbage Guru.)
     *
     * EXCEPTION (diagnostic, 2026-07-24): FillRect is wrapped by a pass-
     * through that records ri->BytesPerRow/Memory (P96's own idea of the
     * frame geometry) for CMD_DEBUG, then chains to P96's prefilled
     * implementation. Same field we write, saved first - no offset guessing. */
    orig_FillRect = bi->FillRect;
    if (orig_FillRect) bi->FillRect = my_FillRect;

    /* (We do NOT start the diff process here - CreateNewProc from InitCard's
     * context is unsafe and crashes #80000004. It starts from SetGC instead,
     * once a screen actually opens on the board.) */

    dbgf((CONST_STRPTR)"a314rtg: InitCard done -> TRUE\n");
    return TRUE;
}
