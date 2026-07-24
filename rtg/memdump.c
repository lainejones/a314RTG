/*
 * memdump.c - dump a raw memory region to a file (for offline analysis of the
 * P96 card framebuffer layout). Use the addresses from the card's CMD_DEBUG
 * journal line.
 *
 *   memdump <hexaddr> <hexlen> <file>
 *   e.g. memdump 87e0000 40000 PiDisk:rtg/frame.bin
 */

#include <exec/types.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    ULONG addr, len;
    BPTR  f;

    if (argc < 4) { printf("usage: memdump <hexaddr> <hexlen> <file>\n"); return 5; }
    addr = strtoul(argv[1], NULL, 16);
    len  = strtoul(argv[2], NULL, 16);

    f = Open((STRPTR)argv[3], MODE_NEWFILE);
    if (!f) { printf("memdump: cannot open %s\n", argv[3]); return 5; }
    Write(f, (APTR)addr, len);
    Close(f);
    printf("memdump: wrote 0x%lx bytes from 0x%08lx to %s\n", len, addr, argv[3]);
    return 0;
}
