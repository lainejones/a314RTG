/*
 * test_rtg.c - standalone A314 RTG connection test
 * Run from Amiga Shell to diagnose FindCard failures.
 * Build: make test_rtg
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "a314.h"

struct ExecBase   *SysBase;
struct DosLibrary *DOSBase;

int main(void)
{
    struct MsgPort        mp;
    struct A314_IORequest ior;
    BYTE sigbit;

    SysBase = *(struct ExecBase **)4L;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0);
    if (!DOSBase)
        return 1;

    PutStr("A314 RTG connection test\n");

    sigbit = AllocSignal(-1);
    if (sigbit < 0) {
        PutStr("FAIL: AllocSignal\n");
        goto done;
    }
    PutStr("OK: AllocSignal\n");

    mp.mp_Node.ln_Type        = NT_MSGPORT;
    mp.mp_Flags               = PA_SIGNAL;
    mp.mp_SigBit              = (UBYTE)sigbit;
    mp.mp_SigTask             = FindTask(NULL);
    mp.mp_MsgList.lh_Head     = (struct Node *)&mp.mp_MsgList.lh_Tail;
    mp.mp_MsgList.lh_Tail     = NULL;
    mp.mp_MsgList.lh_TailPred = (struct Node *)&mp.mp_MsgList.lh_Head;

    ior.a314_Request.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ior.a314_Request.io_Message.mn_ReplyPort    = &mp;
    ior.a314_Request.io_Message.mn_Length       = sizeof(ior);

    PutStr("Opening a314.device... ");
    if (OpenDevice("a314.device", 0, (struct IORequest *)&ior, 0) != 0) {
        PutStr("FAIL\n");
        FreeSignal(sigbit);
        goto done;
    }
    PutStr("OK\n");

    PutStr("Connecting to 'rtg' service... ");
    ior.a314_Request.io_Command = A314_CONNECT;
    ior.a314_Request.io_Error   = 0;
    ior.a314_Socket             = 0x52544701UL;
    ior.a314_Buffer             = (STRPTR)"rtg";
    ior.a314_Length             = 3;
    DoIO((struct IORequest *)&ior);

    if (ior.a314_Request.io_Error == A314_CONNECT_OK) {
        PutStr("OK - connected!\n");
        ior.a314_Request.io_Command = A314_RESET;
        ior.a314_Request.io_Error   = 0;
        DoIO((struct IORequest *)&ior);
        PutStr("Disconnected.\n");
    } else {
        Printf("FAIL (error %ld)\n", (LONG)(BYTE)ior.a314_Request.io_Error);
        Printf("  0=ok 1=socket_in_use 2=reset 3=unknown_service\n");
    }

    CloseDevice((struct IORequest *)&ior);
    FreeSignal(sigbit);

done:
    CloseLibrary((struct Library *)DOSBase);
    return 0;
}
