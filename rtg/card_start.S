| card_start.S - A314RTG Picasso96 card driver library startup
| GNU Assembler m68k Motorola syntax
|
| This file provides the ROM tag, function jump table, and boilerplate
| library functions (Open/Close/Expunge/Null/InitRoutine).
| FindCard and InitCard are implemented in a314rtg.c.

    .equ    VERSION,            0
    .equ    REVISION,           1

    | exec/nodes.h offsets
    .equ    LN_TYPE,            8
    .equ    LN_PRI,             9
    .equ    LN_NAME,            10

    | exec/libraries.h offsets
    .equ    LIB_FLAGS,          14
    .equ    LIB_NEGSIZE,        16
    .equ    LIB_POSSIZE,        18
    .equ    LIB_VERSION,        20
    .equ    LIB_REVISION,       22
    .equ    LIB_IDSTRING,       24
    .equ    LIB_OPENCNT,        32

    .equ    LIBF_SUMUSED,       1
    .equ    LIBF_CHANGED,       2
    .equ    NT_LIBRARY,         9
    .equ    RTF_AUTOINIT,       1

    | CardBase layout (struct Library=34, Flags=1, pad=1, SysBase=4,
    |                   ExpBase=4, SegList=4, Name=4)
    .equ    card_SysBase,       36
    .equ    card_ExpBase,       40
    .equ    card_SegList,       44
    .equ    card_CardName,      48
    .equ    cardbase_SIZEOF,    52

    | Function count * 6 bytes per JMP slot
    .equ    LIB_NEGSIZE_VAL,    36

    .text

| Safety: return -1 if the binary is accidentally executed directly
Start:
    moveq   #-1,d0
    rts
    .even

| ---- ROM tag ---------------------------------------------------------------
RomTag:
    .word   0x4AFC              | RTC_MATCHWORD
    .long   RomTag              | rt_MatchTag (points to itself)
    .long   EndSkip             | rt_EndSkip
    .byte   RTF_AUTOINIT        | rt_Flags
    .byte   VERSION             | rt_Version
    .byte   NT_LIBRARY          | rt_Type
    .byte   0                   | rt_Pri
    .long   LibName             | rt_Name
    .long   IDString            | rt_IdString
    .long   InitTable           | rt_Init -> InitTable (RTF_AUTOINIT)

EndSkip:
    .even

LibName:
    .ascii  "a314rtg.card"
    .byte   0
    .even

IDString:
    .ascii  "a314rtg.card 0.1 (15.05.2026)"
    .byte   13,10,0
    .even

| ---- RTF_AUTOINIT table ----------------------------------------------------
InitTable:
    .long   cardbase_SIZEOF     | [0] size of library base
    .long   FuncTable           | [1] pointer to function table
    .long   0                   | [2] no data table; InitRoutine sets fields
    .long   InitRoutine         | [3] init function

| Function table: terminated by -1. Order maps to LVOs -6, -12, -18, ...
FuncTable:
    .long   Open
    .long   Close
    .long   Expunge
    .long   Null
    .long   _FindCard           | LVO -30
    .long   _InitCard           | LVO -36
    .long   -1

| C symbols have leading underscore in amiga-gcc object files
    .extern _FindCard
    .extern _InitCard
    .extern _SysBase
    .extern _g_cardbase

| ---- InitRoutine: d0=libbase, a0=seglist, a6=ExecBase ----------------------
InitRoutine:
    movea.l d0,a1

    move.l  a6,card_SysBase(a1)
    move.l  a0,card_SegList(a1)

    | Initialize the C global SysBase so AllocVec/etc. work in C code
    lea     _SysBase,a0
    move.l  a6,(a0)

    | Save library base for InitCard (bi->CardBase)
    lea     _g_cardbase,a0
    move.l  d0,(a0)

    | Library identity fields (normally set by data table)
    move.b  #NT_LIBRARY,LN_TYPE(a1)
    lea     LibName(pc),a0
    move.l  a0,LN_NAME(a1)
    lea     IDString(pc),a0
    move.l  a0,LIB_IDSTRING(a1)
    move.b  #(LIBF_SUMUSED+LIBF_CHANGED),LIB_FLAGS(a1)
    move.w  #VERSION,LIB_VERSION(a1)
    move.w  #REVISION,LIB_REVISION(a1)
    move.w  #LIB_NEGSIZE_VAL,LIB_NEGSIZE(a1)
    move.w  #cardbase_SIZEOF,LIB_POSSIZE(a1)

    | d0 still holds library base - return it to signal success
    rts

| ---- Open: a6=libbase, d0=version ------------------------------------------
Open:
    addq.w  #1,LIB_OPENCNT(a6)
    move.l  a6,d0
    rts

| ---- Close: a6=libbase ------------------------------------------------------
Close:
    moveq   #0,d0
    subq.w  #1,LIB_OPENCNT(a6)
    rts

| ---- Expunge: a6=libbase ---------------------------------------------------
| Skeleton: never expunge. A full driver would free resources here.
Expunge:
    moveq   #0,d0
    rts

| ---- Null ------------------------------------------------------------------
Null:
    moveq   #0,d0
    rts
