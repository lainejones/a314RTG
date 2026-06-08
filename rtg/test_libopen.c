/* test_libopen.c - check if a314rtg.card loads successfully */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

int main(void)
{
    struct Library *lib;

    lib = OpenLibrary("LIBS:Picasso96/a314rtg.card", 0);
    if (lib) {
        printf("OK: a314rtg.card opened\n");
        printf("  lib_Node.ln_Name = %s\n", lib->lib_Node.ln_Name);
        printf("  lib_Version      = %d\n", (int)lib->lib_Version);
        printf("  lib_NegSize      = %d\n", (int)lib->lib_NegSize);
        printf("  lib_PosSize      = %d\n", (int)lib->lib_PosSize);
        CloseLibrary(lib);
        return 0;
    } else {
        printf("FAIL: OpenLibrary returned NULL\n");
        printf("  a314rtg.card could not be loaded\n");
        return 5;
    }
}
