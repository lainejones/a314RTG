# A314RTG — a Picasso96 RTG card backed by a Raspberry Pi over A314

A314RTG turns the Raspberry Pi attached to your Amiga's [A314](https://github.com/niklasekstrom/a314)
board into a virtual [Picasso96](http://www.picasso96.net/) RTG graphics card. The
Amiga renders a screen *locally* into a carved-out framebuffer; a background
process diffs that framebuffer and streams **only the changed pixels** to the Pi
over the A314 clockport link, and the Pi paints them to its HDMI output via
`/dev/fb0`.

> ⚠️ **Proof of concept — and it is SLOW.** This was built to prove the idea
> works, not to be a practical day-to-day RTG card. It **will always be slow**:
> the A314 clockport is a ~70–120 KB/s byte-at-a-time link, so a full screen
> repaint takes several seconds — there is no way around that with this
> transport. It's fine for a mostly-static Workbench, not for animation/video.
>
> It is also **work-in-progress and does not work with every configuration** —
> it's been exercised on one setup (see [Hardware](#hardware)) at 640×480 /
> 800×600 in 16-bit and 8-bit, and other machines/modes/A314 variants are
> untested and may not work. Shared here for others to read, build, and improve.
> See [Milestones](#milestones).

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

## Requirements

Amiga:

- **68020 or better.** The card driver is built with `-m68020` and will not run
  on a plain 68000 (an A500 with the A314 needs an accelerator).
- **Picasso96** installed (`LIBS:Picasso96/` must exist; the installer checks).
  Picasso96 itself needs AmigaOS 3.0 or newer. Developed and tested on
  AmigaOS 3.2.3.
- The **A314 Amiga software** (`DEVS:a314.device`) and `PiDisk:` (a314fs) if
  you install from the Pi share.
- Optional: Thomas Richter's MMULib (`mmu.library` V43+) on a 68030/040/060 for
  the cheaper dirty-page tracking described below.

Pi:

- The **A314 Pi software** installed at `/opt/a314` with `a314d` running
  (normally as the `a314d` systemd service).
- Python 3 (the A314 venv at `/opt/a314/venv` is used when present; `rtg.py`
  needs only the standard library).
- Booted to the **console, no X11/desktop**, with an HDMI display attached at
  boot, so `/dev/fb0` is the HDMI framebuffer at **16 bpp** (the console default).

## Versions

The package and the card driver are numbered separately:

| Component | Version | How to check |
|-----------|---------|--------------|
| Release package (archive name, GitHub tag) | **1.0.1** | the archive name |
| `a314rtg.card` (Amiga card driver) | **1.1** (24.07.2026) | `Version a314rtg.card` in a Shell once the card is loaded (after the reboot) |
| `pi/install.sh`, `pi/rtg.py` | no own number; ship with the package | |

Release 1.0.1 changed only the Pi-side `install.sh` (it stages from a release
package and finds `a314d.conf` in more places); the card driver is the same 1.1
build as in release 1.0.

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

## Installing — Pi (do this first)

On the Pi (needs an existing [a314](https://github.com/niklasekstrom/a314)
install at `/opt/a314`):

```bash
cd pi
sudo ./install.sh            # or: sudo ./install.sh /path/to/a314shared
```

This installs `rtg.py` to `/opt/a314/`, registers the `rtg` service in
`a314d.conf` (so `a314d` starts `rtg.py` on demand - no manual start needed),
restarts `a314d`, and stages the Amiga-side files (`a314rtg.card`, the monitor
loader, its icon and `Install_A314RTG`) into `<shared-dir>/rtg/` so the Amiga
can copy them straight off `PiDisk:`. The shared dir defaults to
`/home/<your user>/a314shared`; pass `-` to skip staging.

`install.sh` looks for `a314d.conf` in `/etc/opt/a314/` (where the A314
installer puts it), `/opt/a314/`, `/etc/` and `/etc/a314/`. If yours is
elsewhere, name it: `sudo CONF=/path/to/a314d.conf ./install.sh`.

The Amiga screen is centred 1:1 inside whatever mode fb0 negotiated (black
surround). Have the HDMI display connected when the Pi boots so fb0 comes up
at the panel's real resolution rather than a small fallback.

## Installing — Amiga

With `PiDisk:` mounted (after the Pi install staged the files):

```
cd PiDisk:rtg
Execute Install_A314RTG
```

The script checks Picasso96 is present, installs
`LIBS:Picasso96/a314rtg.card` + `DEVS:Monitors/a314rtg` (+`.info` — its
`BOARDTYPE=a314rtg` tooltype is what makes P96 register the board), makes the
monitor loader executable, then prints the one-time mode setup:

1. Reboot (the monitor loader registers the card at boot).
2. `SYS:Prefs/P96Prefs` → the `a314rtg` item → **Add default modes** → Save →
   let it reboot. This creates board-attached `A314RTG:` screenmodes — modes
   named `no board:` mean this step is missing and screens will not reach
   the board.
3. `SYS:Prefs/ScreenMode` → pick an `A314RTG:` mode (e.g.
   `A314RTG:640x400 16bit PC`) → **Use** to try, **Save** to keep.

Installing by hand, from the unpacked archive instead of `PiDisk:`: copy
`a314rtg.card` to `LIBS:Picasso96/`, and `a314rtg` + `a314rtg.info` to
`DEVS:Monitors/`. Use the `.lha` archive if you can: a `.zip` loses AmigaDOS
protection bits, so after a `.zip` run `protect DEVS:Monitors/a314rtg +e`
(`Install_A314RTG` does this for you).

## MMU dirty-page tracking (68030, optional)

On machines running Thomas Richter's `mmu.library` V43+ (the MMULib package),
the diff process asks the MMU which framebuffer pages were written instead of
re-comparing the whole framebuffer every pass — near-zero CPU cost while the
screen is static. It's fully self-configuring: the card validates the whole
path against real render traffic at mode-switch time and silently stays on
the classic full-scan diff when there's no MMU, no mmu.library, or a mode
whose bitmap exceeds the safe tracking budget (~512 KB span). No separate
builds, no tooltypes — machines without an MMU just work.

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
- **M3 — partly done:** `rtg.py` is auto-started by a314d (`pi/install.sh`
  registers it); MMU dirty-page tracking cuts the diff cost. Still planned:
  forward FillRect/BlitRect as real ops (speed), resolutions beyond 800×600.

## License

MIT — see [LICENSE](LICENSE). Note that the Picasso96 SDK and A314 software are
third-party and carry their own licenses; they are not included here.
