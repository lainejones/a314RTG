# A314RTG — a Picasso96 RTG card backed by a Raspberry Pi over A314

A314RTG turns the Raspberry Pi attached to your Amiga's [A314](https://github.com/niklasekstrom/a314)
board into a virtual [Picasso96](http://www.picasso96.net/) RTG graphics card. The
Amiga renders a screen *locally* into a carved-out framebuffer; a background
process diffs that framebuffer and streams **only the changed pixels** to the Pi
over the A314 clockport link, and the Pi paints them to its HDMI output via
`/dev/fb0`.

> **Status: experimental / work-in-progress.** It has reached a working state —
> the board registers with Picasso96, and opening a 640×480 or 800×600 screen
> renders Workbench on the Pi's HDMI in both 16-bit (R5G6B5) and 8-bit (CLUT)
> modes. It is shared here for others to read, build, and improve. See
> [Milestones](#milestones).

## Why a diff, not a shared framebuffer?

The A314 clockport has **no DMA-shared RAM** — only a slow (~70–120 KB/s)
byte-at-a-time link. A shared framebuffer is therefore impossible. Instead:

- Picasso96 renders normally into the Amiga-side framebuffer (the card keeps
  `GRANTDIRECTACCESS`; FillRect/BlitRect/text are real local-memory ops, **no
  A314 traffic**).
- A background `Process` (`a314rtg.diff`) keeps a shadow copy, diffs the
  framebuffer against it in horizontal runs, and ships changed runs as small
  packets (`PIXELS`/`FILL`, plus `SETPALETTE` for 8-bit) over A314.
- The Pi service (`pi/rtg.py`) reassembles the stream and blits to `/dev/fb0`.

A static screen costs ~no traffic; a full repaint trickles over a few seconds.

## Hardware

- Amiga 1200 (developed on 68030 @ 50 MHz, 128 MB FastRAM) with an A314 board
- A Raspberry Pi connected to the A314, booted to console (no X11)

## Repository layout

```
rtg/
  a314rtg.c      Amiga-side Picasso96 card driver (the whole driver + romtag)
  Makefile       cross-build via bebbo amiga-gcc
  a314rtg.info   monitor icon; its BOARDTYPE=a314rtg tooltype is what P96 reads
  *test.c        small standalone test programs used during development
  card_start.S, monitor_start.S   superseded (kept for reference; NOT built)
pi/
  rtg.py         Pi-side service: registers "rtg" with a314d, renders to /dev/fb0
tools/           host-side helpers (mode tables, p96 prefs parsing, etc.)
```

## Building

This repo does **not** bundle the third-party SDKs. Fetch them and place them
next to the repo so the Makefile's include paths resolve:

1. **A314 software** — clone [niklasekstrom/a314](https://github.com/niklasekstrom/a314)
   (the build was done against **v1.2.3**) into `./a314-1.2.3/`. The driver only
   needs the headers under `a314-1.2.3/Software/a314device` (`a314.h`); the Pi
   side needs `a314d` running.
2. **Picasso96 developer SDK** — obtain the Picasso96 developer archive (the
   `PrivateInclude/` + `Include/` headers, e.g. `boardinfo.h`,
   `libraries/Picasso96.h`) from the Picasso96 distribution / Aminet and place
   it at `./Picasso96Develop/`.

Then, with the [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc) toolchain
on PATH (developed under Windows + WSL at `/opt/amiga/bin`):

```sh
cd rtg
make build      # offline build — produces a314rtg.card
# (plain `make` also scp-deploys to a Pi share; edit PI_HOST first, or use `build`)
```

## Installing — Amiga

Copy from the build:

- `rtg/a314rtg.card` → `LIBS:Picasso96/`
- a stock Picasso96 **monitor loader** program → `DEVS:Monitors/a314rtg`
  (board-agnostic; it's the standard P96 monitor, not a custom driver)
- `rtg/a314rtg.info` → `DEVS:Monitors/a314rtg.info` — its tooltypes **must**
  include `BOARDTYPE=a314rtg` (that's what makes Picasso96 register the board)

Run `DEVS:Monitors/a314rtg` (or reboot), then run `Picasso96Mode` once to define
640×480 / 800×600 16-bit (or 8-bit) modes for the A314RTG board.

## Installing — Pi

```bash
cp pi/rtg.py /opt/a314/                 # alongside a314d
# register in /etc/a314d.conf for auto-start, e.g.:
#   rtg   python3 /opt/a314/rtg.py -ondemand
```

`/boot/config.txt` must give fb0 a 16bpp mode matching (or larger than) the P96
mode, e.g. `framebuffer_width=640` / `framebuffer_height=480`. The service
centres the Amiga screen in fb0.

## Wire protocol (Amiga → Pi)

Every packet is one `A314_WRITE`, self-contained, ≤252 bytes, big-endian header,
first byte = command id. 16-bit pixels are raw R5G6B5PC copied verbatim (fb0 is
also little-endian RGB565 — no colour conversion).

| ID | Name       | Mode  | Payload after cmd byte                  |
|----|------------|-------|-----------------------------------------|
| 1  | SETMODE    | both  | w(2) h(2) bpp(1)                        |
| 2  | SETSWITCH  | both  | state(1)                               |
| 4  | PIXELS     | 16bpp | x(2) y(2) n(1) + n×2 raw bytes         |
| 5  | FILL       | 16bpp | x(2) y(2) n(2) + pixel(2)              |
| 6  | SETPALETTE | 8bpp  | start(1) count(1) + count×2 RGB565-LE |
| 7  | PIXELS8    | 8bpp  | x(2) y(2) n(1) + n CLUT-index bytes   |
| 8  | FILL8      | 8bpp  | x(2) y(2) n(2) + index(1)             |

## Milestones

- **M1 — done:** card registers with P96, survives reboot, appears in P96 prefs.
- **M2 — working:** opening a screen connects `a314rtg.diff` to the Pi and
  Workbench renders on HDMI (16-bit and 8-bit). Key gotchas solved: romtag must
  be `RTF_AUTOINIT` (0x80); `SetPanning`'s `width` is already the row stride in
  bytes (don't multiply by bpp).
- **M3 — planned:** forward FillRect/BlitRect as real ops (speed), resolutions
  beyond 800×600, auto-start `rtg.py` via a314d.

## License

MIT — see [LICENSE](LICENSE). Note that the Picasso96 SDK and A314 software are
third-party and carry their own licenses; they are not included here.
