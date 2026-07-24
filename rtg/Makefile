CC      = m68k-amigaos-gcc
# Built the ZZ9000 way: -nostartfiles (no crt0/main; the romtag is the entry)
# but NOT -nostdlib, link -lamiga -noixemul. The library scaffolding is now in
# a314rtg.c (no card_start.S). -m68020 matches the target accelerator.
CFLAGS  = -m68020 -O2 -noixemul -nostartfiles -fomit-frame-pointer \
          -Wall -Wno-unused-parameter \
          -I../Picasso96Develop/PrivateInclude \
          -I../Picasso96Develop/Include \
          -I../a314-1.2.3/Software/a314device

TARGET      = a314rtg.card

# ---- Pi deployment ------------------------------------------------------
# The build drops ready-to-copy files into the Amiga-facing Pi share so
# PiDisk:rtg/ on the Amiga always has the latest. The monitor is deployed
# WITHOUT an extension (a314rtg) so it can be copied straight to
# DEVS:Monitors/. The Pi-side service rtg.py is synced to /opt/a314/.
# "a314cp" is a WSL ~/.ssh/config alias (192.168.50.33, key auth). A bare
# user@ip here would fall back to password prompts and hang scripted deploys.
PI_HOST    = a314cp
PI_SHARE   = /home/laine/a314shared/rtg
PI_SVC     = /opt/a314/rtg.py
SSHOPTS    = -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o BatchMode=yes

# The "monitor" deployed to DEVS:Monitors/ is the STOCK P96 loader program
# (p96monitor, a board-agnostic copy of the standard Picasso96 monitor program),
# NOT a custom driver. It reads its icon's BOARDTYPE=a314rtg tooltype, registers
# the board with rtg.library, and loads LIBS:Picasso96/a314rtg.card. Our old
# monitor_start.S stub did nothing on load and is no longer used.
all: $(TARGET) deploy

# Build only, no Pi deployment (use when offline).
build: $(TARGET)

$(TARGET): a314rtg.c
	$(CC) $(CFLAGS) a314rtg.c -o $@ -lamiga

# Copy deliverables to the Pi share + sync the Pi service. Network steps are
# non-fatal (prefixed with -) so an offline build still succeeds.
deploy: $(TARGET) p96monitor a314rtg.info
	@echo "==> Deploying to $(PI_HOST):$(PI_SHARE)"
	-scp $(SSHOPTS) $(TARGET)     $(PI_HOST):$(PI_SHARE)/a314rtg.card
	-scp $(SSHOPTS) p96monitor    $(PI_HOST):$(PI_SHARE)/a314rtg
	-scp $(SSHOPTS) a314rtg.info  $(PI_HOST):$(PI_SHARE)/a314rtg.info
	-ssh $(SSHOPTS) $(PI_HOST) 'rm -f $(PI_SHARE)/a314rtg.monitor'
	@echo "==> Syncing Pi service rtg.py -> $(PI_SVC)"
	-scp $(SSHOPTS) ../pi/rtg.py  $(PI_HOST):/home/laine/rtg.py
	-ssh $(SSHOPTS) $(PI_HOST) 'sudo cp /home/laine/rtg.py $(PI_SVC) && sudo chmod 644 $(PI_SVC)'
	@echo "==> Deploy done. On the Amiga, mount PiDisk: and copy from rtg/:"
	@echo "      a314rtg.card -> LIBS:Picasso96/"
	@echo "      a314rtg      -> DEVS:Monitors/"
	@echo "      a314rtg.info -> DEVS:Monitors/"

# CLI harness validating the mmu.library dirty-page path on a live machine.
mmutest: mmutest.c
	$(CC) -m68020 -O2 -noixemul -o $@ mmutest.c
	-scp $(SSHOPTS) mmutest $(PI_HOST):$(PI_SHARE)/mmutest

clean:
	rm -f $(TARGET) mmutest *.o
