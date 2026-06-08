| monitor_start.S - Minimal A314RTG P96 monitor library
|
| P96 loads files from DEVS:Monitors/ as libraries and calls FindCard (LVO -30)
| to check if the hardware is present. For a virtual card, FindCard must always
| return TRUE. InitCard (LVO -36) also returns TRUE without touching anything;
| BIF_INTERNALMODESONLY on the card driver means P96 uses the card's own
| ResolutionsList for mode timing rather than this monitor library.

    .equ    VERSION,         0
    .equ    REVISION,        1

    | exec/nodes.h
    .equ    LN_TYPE,         8
    .equ    LN_NAME,         10
    .equ    NT_LIBRARY,      9

    | exec/libraries.h
    .equ    LIB_FLAGS,       14
    .equ    LIB_NEGSIZE,     16
    .equ    LIB_POSSIZE,     18
    .equ    LIB_VERSION,     20
    .equ    LIB_REVISION,    22
    .equ    LIB_IDSTRING,    24
    .equ    LIB_OPENCNT,     32

    .equ    LIBF_SUMUSED,    1
    .equ    LIBF_CHANGED,    2
    .equ    RTF_AUTOINIT,    1

    | 6 functions: Open Close Expunge Null FindCard(-30) InitCard(-36)
    .equ    LIB_NEGSIZE_VAL, 36
    | Library base is plain struct Library (34 bytes) — no extra fields needed
    .equ    MONBASE_SIZEOF,  34

    .text

| Safety: return -1 if accidentally executed directly
Start:
    moveq   #-1,d0
    rts
    .even

| ---- ROM tag ---------------------------------------------------------------
RomTag:
    .word   0x4AFC              | RTC_MATCHWORD
    .long   RomTag              | rt_MatchTag
    .long   EndSkip             | rt_EndSkip
    .byte   RTF_AUTOINIT        | rt_Flags
    .byte   VERSION             | rt_Version
    .byte   NT_LIBRARY          | rt_Type
    .byte   0                   | rt_Pri
    .long   LibName             | rt_Name
    .long   IDString            | rt_IdString
    .long   InitTable           | rt_Init

EndSkip:
    .even

LibName:
    .ascii  "a314rtg.monitor"
    .byte   0
    .even

IDString:
    .ascii  "a314rtg.monitor 0.1 (A314RTG virtual RTG)"
    .byte   13,10,0
    .even

| ---- RTF_AUTOINIT table ----------------------------------------------------
InitTable:
    .long   MONBASE_SIZEOF      | library base size
    .long   FuncTable           | function table
    .long   0                   | no data table
    .long   InitRoutine         | init function

FuncTable:
    .long   Open                | LVO  -6
    .long   Close               | LVO -12
    .long   Expunge             | LVO -18
    .long   Null                | LVO -24
    .long   FindCard            | LVO -30
    .long   InitCard            | LVO -36
    .long   -1                  | end of table

| ---- InitRoutine: d0=libbase, a0=seglist, a6=ExecBase ----------------------
InitRoutine:
    movea.l d0,a1
    move.b  #NT_LIBRARY,LN_TYPE(a1)
    lea     LibName(pc),a0
    move.l  a0,LN_NAME(a1)
    lea     IDString(pc),a0
    move.l  a0,LIB_IDSTRING(a1)
    move.b  #(LIBF_SUMUSED+LIBF_CHANGED),LIB_FLAGS(a1)
    move.w  #VERSION,LIB_VERSION(a1)
    move.w  #REVISION,LIB_REVISION(a1)
    move.w  #LIB_NEGSIZE_VAL,LIB_NEGSIZE(a1)
    move.w  #MONBASE_SIZEOF,LIB_POSSIZE(a1)
    | d0 still holds library base — return it to signal success
    rts

| ---- Standard library functions --------------------------------------------
Open:
    addq.w  #1,LIB_OPENCNT(a6)
    move.l  a6,d0
    rts

Close:
    moveq   #0,d0
    subq.w  #1,LIB_OPENCNT(a6)
    rts

Expunge:
    moveq   #0,d0
    rts

Null:
    moveq   #0,d0
    rts

| ---- FindCard (LVO -30) -----------------------------------------------------
| Virtual card is always present — return TRUE unconditionally.
| P96 uses this to decide whether to include this monitor in the
| "assign settings to board" list.
FindCard:
    moveq   #1,d0
    rts

| ---- InitCard (LVO -36) -----------------------------------------------------
| BIF_INTERNALMODESONLY means P96 uses the card driver's ResolutionsList
| for timing. Nothing to fill in here.
InitCard:
    moveq   #1,d0
    rts
