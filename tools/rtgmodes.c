/* RTGModes - dump every display mode the Amiga's display database knows about,
 * with size/depth and whether it is a BOARD (RTG/foreign) mode or a native one.
 * Writes to PiDisk:rtg/RTGModes.txt so it can be read off the A314 share.
 *
 * Build: m68k-amigaos-gcc -noixemul -O2 -o RTGModes rtgmodes.c
 */
#include <exec/types.h>
#include <graphics/displayinfo.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    BPTR f;
    ULONG id = INVALID_ID;
    char line[256];
    int n = 0;

    f = Open((CONST_STRPTR)"PiDisk:rtg/RTGModes.txt", MODE_NEWFILE);
    if (!f) {
        Printf((CONST_STRPTR)"could not open PiDisk:rtg/RTGModes.txt\n");
        return 20;
    }

    while ((id = NextDisplayInfo(id)) != INVALID_ID) {
        struct DisplayInfo   di;
        struct DimensionInfo dim;
        struct NameInfo      ni;
        UWORD w, h;
        UBYTE depth;
        const char *kind;

        if (GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, id) <= 0)
            continue;
        if (GetDisplayInfoData(NULL, (UBYTE *)&dim, sizeof(dim), DTAG_DIMS, id) <= 0)
            continue;

        ni.Name[0] = '\0';
        GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof(ni), DTAG_NAME, id);

        w     = (UWORD)(dim.Nominal.MaxX - dim.Nominal.MinX + 1);
        h     = (UWORD)(dim.Nominal.MaxY - dim.Nominal.MinY + 1);
        depth = (UBYTE)dim.MaxDepth;
        kind  = (di.PropertyFlags & DIPF_IS_FOREIGN) ? "BOARD " : "native";

        sprintf(line, "ID=%08lx  %4ux%-4u  d%-2u  %s  flags=%08lx  %s\n",
                (unsigned long)id, w, h, depth, kind,
                (unsigned long)di.PropertyFlags, ni.Name);
        Write(f, line, strlen(line));
        n++;
    }

    sprintf(line, "--- %d modes ---\n", n);
    Write(f, line, strlen(line));
    Close(f);
    Printf((CONST_STRPTR)"wrote %ld modes to PiDisk:rtg/RTGModes.txt\n", (LONG)n);
    return 0;
}
