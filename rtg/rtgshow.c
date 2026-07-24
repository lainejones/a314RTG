/*
 * rtgshow.c - open a P96/RTG screen directly by DisplayID and animate on it.
 *
 * Bypasses the ScreenMode prefs GUI entirely: opens the screen (which fires
 * the card's SetGC -> diff process -> Pi "rtg" service chain), draws a text
 * banner plus a bouncing filled box for the hold period so the incremental
 * diff has continuous small updates to stream, then closes and exits.
 *
 *   rtgshow [hexModeID] [seconds]     defaults: 50041102 45
 *
 * Watch journalctl -u a314d on the Pi for SETMODE + traffic; the screen
 * appears on the Pi HDMI. Exit code 5 = screen would not open.
 */

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    ULONG mode  = 0x50041102UL;
    LONG  secs  = 45;
    LONG  depth = 16;           /* 0 = omit SA_Depth (mode's own depth) */
    struct Screen *scr;
    struct RastPort *rp;
    WORD x = 20, y = 40, dx = 7, dy = 5, bw = 60, bh = 40;
    LONG w, h, t, frames;

    if (argc > 1) mode  = strtoul(argv[1], NULL, 16);
    if (argc > 2) secs  = atol(argv[2]);
    if (argc > 3) depth = atol(argv[3]);

    printf("rtgshow: opening DisplayID 0x%08lx for %ld s depth=%ld\n",
           mode, secs, depth);

    scr = OpenScreenTags(NULL,
        SA_DisplayID,  mode,
        depth ? SA_Depth : TAG_IGNORE, depth,
        SA_Title,      (ULONG)"A314RTG test screen",
        SA_ShowTitle,  TRUE,
        SA_Quiet,      FALSE,
        TAG_DONE);
    if (!scr) {
        printf("rtgshow: OpenScreenTags FAILED for 0x%08lx\n", mode);
        return 5;
    }
    w = scr->Width; h = scr->Height;
    printf("rtgshow: screen open %ldx%ld, actual ModeID=0x%08lx depth=%d\n",
           w, h, (ULONG)GetVPModeID(&scr->ViewPort),
           (int)scr->RastPort.BitMap->Depth);
    rp = &scr->RastPort;

    /* static backdrop: banner bars so a single full push is visible too */
    SetAPen(rp, 2); RectFill(rp, 0, 20, w - 1, 30);
    SetAPen(rp, 3); RectFill(rp, 0, h - 30, w - 1, h - 20);

    if (secs < 0) {                        /* negative = STATIC test image */
        SetAPen(rp, 1);
        RectFill(rp, w/2 - 100, h/2 - 60, w/2 + 100, h/2 + 60);
        SetAPen(rp, 3);
        RectFill(rp, w/2 - 60,  h/2 - 30, w/2 + 60,  h/2 + 30);
        printf("rtgshow: static image, holding %ld s\n", -secs);
        Delay(-secs * 50);
        CloseScreen(scr);
        printf("rtgshow: done\n");
        return 0;
    }

    frames = secs * 25;                    /* ~25 fps ticks */
    for (t = 0; t < frames; t++) {
        SetAPen(rp, 0); RectFill(rp, x, y, x + bw, y + bh);   /* erase */
        x += dx; y += dy;
        if (x < 0 || x + bw >= w) { dx = -dx; x += dx; }
        if (y < 32 || y + bh >= h - 32) { dy = -dy; y += dy; }
        SetAPen(rp, (t / 25) % 8);
        RectFill(rp, x, y, x + bw, y + bh);
        Delay(2);                          /* 25 ticks/s at Delay(2) */
    }

    CloseScreen(scr);
    printf("rtgshow: done\n");
    return 0;
}
