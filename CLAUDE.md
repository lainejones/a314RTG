# A314RTG — Picasso96 RTG card driver for Amiga via Raspberry Pi

## What this project is

A Picasso96 `.card` driver that makes an Amiga 1200 treat the connected Raspberry Pi (via A314 board) as a virtual RTG graphics card. P96 renders locally into the Amiga framebuffer; a background diff process streams the **changed pixels** over the A314 clockport link to the Pi, which renders them to its HDMI output via `/dev/fb0`.

## Hardware

- Amiga 1200 with 68030 @ 50 MHz, 68882 FPU, 128 MB FastRAM
- A314 board connected via clockport (default A1200 clockport)
- Raspberry Pi connected to A314

## Repository layout

```
rtg/
  a314rtg.c       Amiga-side P96 card driver (C, m68k-amigaos-gcc)
  card_start.S    Library boilerplate: ROM tag, jump table, InitRoutine (68k asm)
  Makefile        Cross-build via WSL + bebbo amiga-gcc at /opt/amiga/bin/
pi/
  rtg.py          Pi-side service: registers "rtg" with a314d, renders to /dev/fb0
Picasso96Develop/ P96 SDK headers (PrivateInclude/, Include/)
a314-1.2.3/       A314 source — a314device headers used for build, a314d.py for Pi
```

## Building (Windows + WSL)

```powershell
wsl bash << 'EOF'
export PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin
cd /mnt/c/projects/A314RTG/rtg
make clean && make
EOF
```

`make` builds the card + monitor **and auto-deploys** to the Pi share
`/home/laine/a314shared/rtg/` (Amiga `PiDisk:rtg/`) via scp, plus syncs the
Pi service to `/opt/a314/rtg.py`. Network steps are non-fatal, so an offline
build still succeeds; use `make build` to skip deployment entirely.

After build, on the Amiga mount `PiDisk:` and copy from `rtg/`:
- `a314rtg.card` → `LIBS:Picasso96/`
- `a314rtg`      → `DEVS:Monitors/`  (the STOCK P96 loader program, board-agnostic copy = `rtg/p96monitor`; NOT a custom driver — `monitor_start.S` is dead)
- `a314rtg.info` → `DEVS:Monitors/`  (icon; `BOARDTYPE=a314rtg` is what the loader reads to register the board + load the card)

Then run `DEVS:Monitors/a314rtg` from a Shell (or reboot) — the loader registers
the board with rtg.library and loads the card. It then appears in `Picasso96Mode`,
where you define the 640×480 / 800×600 16-bit modes.

Then run `Picasso96Mode` once to define 640×480 / 800×600 16-bit modes for the
A314RTG board (the card driver does not supply modes — that's P96's design).

## Amiga installation

- `LIBS:Picasso96/a314rtg.card` — the card driver
- `DEVS:Monitors/<name>.monitor` + `.info` — monitor driver; `.info` tooltypes must include `BOARDTYPE=a314rtg`
- P96 prefs: select the A314RTG card, choose a screen mode (640×480 or 800×600 16-bit)

## Pi installation

```bash
# Copy rtg.py alongside a314d.py (usually /home/pi/a314/)
cp pi/rtg.py /home/pi/a314/

# Run manually (Pi must be at console, no X11):
cd /home/pi/a314
python3 rtg.py

# Or add to /etc/a314d.conf for auto-start with a314d
```

Pi `/boot/config.txt` must match the P96 mode:
```
framebuffer_width=640
framebuffer_height=480
```

## A314 IPC — key facts

- Amiga opens `a314.device`, issues `A314_CONNECT` to service name `"rtg"`
- `A314_WRITE` max payload: **≤252 bytes** per call (the link resets the stream if `len+3 > 255`)
- Clockport E-clock: ~709 KHz → ~70–120 KB/s effective throughput
- Pi service connects to `a314d` daemon on localhost:7110, receives MSG_CONNECT / MSG_DATA / MSG_RESET

## RTG command protocol

**Architecture — this is a framebuffer DIFF, not direct draw commands.** The A314
clockport has NO DMA-shared RAM (only a slow ~70–120 KB/s byte-at-a-time link), so a
shared framebuffer is impossible. Instead P96 renders **locally** into the Amiga
framebuffer (the card carves a dedicated region and keeps `GRANTDIRECTACCESS`;
FillRect/BlitRect/BlitTemplate/etc. are real local-memory ops, **no A314**). A separate
background Process (`a314rtg.diff`, spawned in `SetGC`) then diffs the framebuffer vs a
shadow copy in horizontal runs and streams **only the changed pixels** to the Pi. Static
screen ≈ no traffic; a full 640×480×16 repaint trickles over ~5–6 s.

All packets are big-endian (68k native); each is one `A314_WRITE`, self-contained, and
**≤252 bytes** (the link resets the stream if a packet's `len+3 > 255`). First byte = cmd ID.

| ID | Name       | Mode  | Payload after cmd byte                   | Total     |
|----|------------|-------|------------------------------------------|-----------|
| 1  | SETMODE    | both  | w(2) h(2) bpp(1) bpr(2) panwidth(2)      | 10        |
| 2  | SETSWITCH  | both  | state(1)                                 | 2         |
| 4  | PIXELS     | 16bpp | x(2) y(2) n(1) + n×2 raw R5G6B5PC bytes  | 6 + n×2   |
| 5  | FILL       | 16bpp | x(2) y(2) n(2) + pixel(2)                | 9         |
| 6  | SETPALETTE | 8bpp  | start(1) count(1) + count×2 RGB565-LE    | 4 + cnt×2 |
| 7  | PIXELS8    | 8bpp  | x(2) y(2) n(1) + n CLUT-index bytes      | 6 + n     |
| 8  | FILL8      | 8bpp  | x(2) y(2) n(2) + index(1)                | 8         |

- `n` = run length (pixels) on row `y` starting at column `x`.
- 16-bit pixels are raw R5G6B5PC, little-endian in Amiga memory, copied verbatim — fb0 is
  also LE RGB565, so byte order already matches (no colour conversion).
- `SETMODE.bpr` = the real row stride the diff reads with (= SetPanning's width **in bytes**;
  do NOT multiply by bpp — see stride-doubling note below). `panwidth` is diagnostic only.
- 8-bit modes send `SETPALETTE` (the 256-entry CLUT, chunked) whenever the palette changes,
  then `PIXELS8`/`FILL8` carry CLUT indices.
- **There is no FILLRECT / BLITRECT / SETPAN on the wire** — those P96 calls draw into local
  memory and reach the Pi only as diffed PIXELS/FILL runs.

## Milestones

### Milestone 1 — COMPLETE
- Card registers with P96, survives reboot, appears in P96 prefs
- Static BSS framebuffer (800×600×2 = 960 KB) in Amiga FastRAM
- All required P96 callbacks stubbed

### Milestone 2 — got it WORKING, now debugging regressions
- Reached a working state (2026-05-30): board loads un-greyed, opening a 640×480 / 800×600
  screen connects the `a314rtg.diff` Process to the Pi and **Workbench renders on the Pi's
  HDMI**, in both 16-bit and 8-bit (CLUT) modes.
- `FindCard` opens `a314.device`; `SetGC` spawns `a314rtg.diff`, which connects to the Pi
  `"rtg"` service (bounded connect + retry to survive cold-boot timing).
- P96 renders LOCALLY; the diff Process streams changed pixels (PIXELS/FILL, plus
  SETPALETTE for 8-bit). `SetGC`/`SetSwitch`/`SetPanning` emit SETMODE/SETSWITCH.
- Pi `rtg.py` reassembles the command stream and blits to `/dev/fb0` (centres the Amiga
  screen in a 1920×1080 fb).
- **Status: had it working, then hit issues — actively debugging.** Hard-won gotchas:
  romtag `rt_Flags` MUST be `0x80` (RTF_AUTOINIT), not `0x01`, or the card crashes
  (#80000004) at load; and `mode_bpr` = SetPanning's `width` **directly** (it's already
  bytes) — multiplying by bpp doubled the stride → shear + OOB shadow write → reboot.

### Milestone 3 — PLANNED
- Direct-blit acceleration commands (FillRect/BlitRect/BlitTemplate forwarded as real
  ops) to cut traffic — correctness is already covered by the diff, this is for speed.
- Expand max resolution beyond 800×600 (raise MaxHor/VerValue arrays + card mem).
- Auto-start `rtg.py` via the a314d service config (already wired via `a314d.conf`).

## Key technical notes

### SysBase initialisation
P96 does NOT set `bi->ExecBase` before calling `FindCard`/`InitCard`. The global `struct ExecBase *SysBase` is initialised in `card_start.S` `InitRoutine` from `a6`:
```asm
lea     _SysBase,a0
move.l  a6,(a0)
```

### Monitor driver tooltype
P96 prefs only shows the card if a monitor driver in `DEVS:Monitors/` has `BOARDTYPE=a314rtg` in its `.info` tooltypes. Without this, P96 ignores the card entirely (but it still auto-initialises on boot via RTF_AUTOINIT).

### Framebuffer memory
P96 renders directly into the Amiga-side framebuffer (FastRAM; the card carves a
dedicated region and keeps `GRANTDIRECTACCESS`). The `a314rtg.diff` Process keeps a
shadow copy and, each pass, diffs framebuffer vs shadow and ships only the changed runs
to the Pi (then updates the shadow to match). Sync is by **diffing**, NOT by intercepting
each P96 draw call.

### A314 socket ID
We use `(ULONG)&a314_socket` as the socket identifier — the BSS address is unique per session, requires no dos.library DateStamp call.

### proto_a314.h incompatibility
`proto_a314.h` uses SAS/C `__reg()` inline syntax, not GCC `__asm()`. Do not include it. We only use `a314.h` (structs + command codes) and standard `DoIO` calls.

### Diff process (`a314rtg.diff`)
Spawned from `SetGC` via `CreateNewProc` (this is safe — an earlier "CreateNewProc from
a callback crashes" theory was a red herring; the real crash was the romtag flag). It owns
the A314 connection, polls ~every 40 ms, diffs in horizontal runs with a per-pass byte
budget (resume row across passes), and emits FILL for uniform runs ≥ `FILL_MIN_RUN`,
else PIXELS. Raw R5G6B5PC bytes are copied verbatim (no colour decode).
