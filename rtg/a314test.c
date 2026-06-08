/* a314test.c - connect to the Pi "rtg" service directly from a CLI program,
 * independent of the P96 card, to isolate link/service issues from card code.
 * Bounded wait (SendIO + CheckIO + timeout) so it can never hang.
 *
 * Build: m68k-amigaos-gcc -noixemul -O2 -I../a314-1.2.3/Software/a314device \
 *          a314test.c -o a314test
 */
#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "a314.h"
#include <stdio.h>

int main(void)
{
    struct MsgPort        *mp;
    struct A314_IORequest *ior;
    LONG err;
    int  i;

    mp = CreateMsgPort();
    printf("CreateMsgPort       = %08lx\n", (ULONG)mp);
    if (!mp) return 0;

    ior = (struct A314_IORequest *)CreateIORequest(mp, sizeof(struct A314_IORequest));
    printf("CreateIORequest     = %08lx\n", (ULONG)ior);
    if (!ior) { DeleteMsgPort(mp); return 0; }

    err = OpenDevice((STRPTR)"a314.device", 0, (struct IORequest *)ior, 0);
    printf("OpenDevice a314.dev = %ld   (0 = ok)\n", err);
    if (err != 0) {
        printf("=> a314.device won't open - the A314 driver/link is not up.\n");
        DeleteIORequest((struct IORequest *)ior); DeleteMsgPort(mp); return 0;
    }

    ior->a314_Request.io_Command = A314_CONNECT;
    ior->a314_Request.io_Error   = 0;
    ior->a314_Socket             = (ULONG)mp;       /* unique id */
    ior->a314_Buffer             = (STRPTR)"rtg";
    ior->a314_Length             = 3;

    printf("A314_CONNECT \"rtg\" (waiting up to 10s)...\n");
    SendIO((struct IORequest *)ior);
    for (i = 0; i < 50; i++) {
        if (CheckIO((struct IORequest *)ior)) break;
        Delay(10);                                  /* 0.2s */
    }
    if (!CheckIO((struct IORequest *)ior)) {
        printf("=> TIMEOUT: no response in 10s - a314d not answering / link stalled.\n");
        AbortIO((struct IORequest *)ior);
        WaitIO((struct IORequest *)ior);
        CloseDevice((struct IORequest *)ior);
        DeleteIORequest((struct IORequest *)ior); DeleteMsgPort(mp); return 0;
    }
    WaitIO((struct IORequest *)ior);
    err = ior->a314_Request.io_Error;
    printf("  io_Error = %ld\n", err);
    switch (err) {
        case A314_CONNECT_OK:            printf("=> CONNECTED OK - link + rtg service work!\n"); break;
        case A314_CONNECT_SOCKET_IN_USE: printf("=> SOCKET_IN_USE\n"); break;
        case A314_CONNECT_RESET:         printf("=> RESET\n"); break;
        case A314_CONNECT_UNKNOWN_SERVICE:
            printf("=> UNKNOWN_SERVICE - a314d has no 'rtg' (config not loaded / not ready)\n"); break;
        default:                         printf("=> error %ld\n", err); break;
    }

    if (err == A314_CONNECT_OK) {        /* clean up the stream */
        ior->a314_Request.io_Command = A314_RESET;
        DoIO((struct IORequest *)ior);
    }
    CloseDevice((struct IORequest *)ior);
    DeleteIORequest((struct IORequest *)ior);
    DeleteMsgPort(mp);
    return 0;
}
