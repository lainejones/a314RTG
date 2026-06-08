#!/usr/bin/env python3
# rtg.py - A314RTG Pi-side service (dirty-tile diffing renderer)
#
# Receives RTG drawing commands streamed over A314 from the Amiga card driver
# (a314rtg.card) and blits them to the Linux framebuffer /dev/fb0.
#
# Protocol (must match a314rtg.c). Each command is self-contained; commands may
# arrive split across, or several-per, MSG_DATA, so we reassemble a byte stream:
#   CMD_SETMODE   (1): w(2 BE) h(2 BE) bpp(1)                 = 6 bytes
#   CMD_SETSWITCH (2): state(1)                               = 2 bytes
#   CMD_PIXELS    (4): x(2 BE) y(2 BE) n(1) pixels(n*2 raw)   = 6 + n*2 bytes
#   CMD_FILL      (5): x(2 BE) y(2 BE) n(2 BE) pixel(2 raw)   = 9 bytes
# Pixel bytes are raw R5G6B5 little-endian, copied verbatim to fb0 (also LE
# RGB565 on the Pi) - no colour decoding, so byte order is correct end-to-end.
#
# Requirements:
#   - Pi booted to console (no X11) so /dev/fb0 is the HDMI framebuffer
#   - fb0 set to 16bpp at the same WxH as the P96 mode (via /boot/config.txt:
#     framebuffer_width / framebuffer_height, or a KMS mode)
#   - Registered in a314d.conf:  rtg <python3> /opt/a314/rtg.py
#
# Self-contained: speaks the a314d localhost protocol directly.

import logging
import mmap
import os
import select
import socket
import struct
import sys

logging.basicConfig(format='%(levelname)s rtg: %(message)s')
log = logging.getLogger('rtg')
log.setLevel(logging.INFO)

SERVICE_NAME = b'rtg'
A314D_HOST = 'localhost'
A314D_PORT = 7110

# a314d message types
MSG_REGISTER_REQ     = 1
MSG_REGISTER_RES     = 2
MSG_CONNECT          = 9
MSG_CONNECT_RESPONSE = 10
MSG_DATA             = 11
MSG_EOS              = 12
MSG_RESET            = 13

# RTG commands
CMD_SETMODE    = 1
CMD_SETSWITCH  = 2
CMD_PIXELS     = 4    # 16-bit: x2 y2 n1 + n*2 raw RGB565-LE
CMD_FILL       = 5    # 16-bit: x2 y2 n2 + 2 raw bytes
CMD_SETPALETTE = 6    # 8-bit:  start1 count1 + count*2 RGB565-LE
CMD_PIXELS8    = 7    # 8-bit:  x2 y2 n1 + n CLUT-index bytes
CMD_FILL8      = 8    # 8-bit:  x2 y2 n2 + 1 CLUT-index byte


def read_fb_size():
    vs = open('/sys/class/graphics/fb0/virtual_size').read().strip()
    w, h = (int(x) for x in vs.split(','))
    return w, h


class RTGService:
    def __init__(self, sock):
        self.sock = sock
        self.rx = bytearray()        # a314d framing buffer
        self.cmd = bytearray()       # RTG command-stream buffer (active stream)
        self.stream = None
        self.fb_file = None
        self.mm = None
        self.fw = self.fh = self.stride = 0
        self.ox = self.oy = 0          # where the Amiga screen sits in fb0 (centered)
        self.pal = bytearray(256 * 2)  # CLUT for 8-bit modes (RGB565-LE per entry)
        self.dbgn = 0                  # diagnostic: log first few draw packets / mode
        self.maxx = 0                  # diagnostic: max x+n seen this mode
        self._open_fb()

    def _open_fb(self):
        try:
            w, h = read_fb_size()
            self.fb_file = open('/dev/fb0', 'r+b', buffering=0)
            self.mm = mmap.mmap(self.fb_file.fileno(), w * h * 2)
            self.fw, self.fh, self.stride = w, h, w * 2
            log.info('opened /dev/fb0 %dx%d 16bpp', w, h)
        except Exception as e:
            log.error('cannot open /dev/fb0: %s', e)

    # ---- a314d plumbing ---------------------------------------------------

    def _send(self, sid, mtype, payload=b''):
        self.sock.sendall(struct.pack('=IIB', len(payload), sid, mtype) + payload)

    def _feed(self, data):
        self.rx += data
        while len(self.rx) >= 9:
            plen, sid, mtype = struct.unpack('=IIB', self.rx[:9])
            if len(self.rx) < 9 + plen:
                break
            payload = bytes(self.rx[9:9 + plen])
            del self.rx[:9 + plen]
            self._on_msg(sid, mtype, payload)

    def _on_msg(self, sid, mtype, payload):
        if mtype == MSG_CONNECT:
            if payload == SERVICE_NAME and self.stream is None:
                self.stream = sid
                self.cmd = bytearray()
                self._send(sid, MSG_CONNECT_RESPONSE, b'\x00')
                log.info('Amiga connected (stream %d)', sid)
            else:
                self._send(sid, MSG_CONNECT_RESPONSE, b'\x03')   # reject
        elif sid == self.stream:
            if mtype == MSG_DATA:
                self.cmd += payload
                self._parse()
            elif mtype in (MSG_RESET, MSG_EOS):
                log.info('Amiga disconnected (stream %d)', sid)
                self.stream = None
                self.cmd = bytearray()

    # ---- RTG command stream ----------------------------------------------

    def _parse(self):
        b = self.cmd
        while b:
            c = b[0]
            if c == CMD_PIXELS:
                if len(b) < 6:
                    break
                n = b[5]
                total = 6 + n * 2
                if len(b) < total:
                    break
                x, y = struct.unpack_from('>HH', b, 1)
                data = bytes(b[6:total])
                del b[:total]
                self._pixels(x, y, n, data)
            elif c == CMD_FILL:
                if len(b) < 9:
                    break
                x, y, n = struct.unpack_from('>HHH', b, 1)
                px = bytes(b[7:9])
                del b[:9]
                self._fill(x, y, n, px)
            elif c == CMD_SETMODE:
                if len(b) < 6:
                    break
                w, h, bpp = struct.unpack_from('>HHB', b, 1)
                del b[:6]
                self._setmode(w, h, bpp)
            elif c == CMD_SETSWITCH:
                if len(b) < 2:
                    break
                st = b[1]
                del b[:2]
                self._setswitch(st)
            elif c == CMD_SETPALETTE:
                if len(b) < 3:
                    break
                start, count = b[1], b[2]
                total = 3 + count * 2
                if len(b) < total:
                    break
                data = bytes(b[3:total])
                del b[:total]
                self._setpalette(start, count, data)
            elif c == CMD_PIXELS8:
                if len(b) < 6:
                    break
                n = b[5]
                total = 6 + n
                if len(b) < total:
                    break
                x, y = struct.unpack_from('>HH', b, 1)
                idx = bytes(b[6:total])
                del b[:total]
                self._pixels8(x, y, n, idx)
            elif c == CMD_FILL8:
                if len(b) < 8:
                    break
                x, y, n = struct.unpack_from('>HHH', b, 1)
                ci = b[7]
                del b[:8]
                self._fill8(x, y, n, ci)
            else:
                log.warning('unknown command 0x%02x - flushing stream', c)
                b.clear()
                break

    def _setmode(self, w, h, bpp):
        # Center the Amiga screen inside fb0 (the HDMI panel is usually larger,
        # e.g. 1920x1080). 1:1, no scaling - crisp pixels, black surround.
        self.ox = max(0, (self.fw - w) // 2)
        self.oy = max(0, (self.fh - h) // 2)
        log.info('SETMODE %dx%d bpp=%d (fb0 %dx%d) -> centered at +%d,+%d  '
                 '[visible*bpp=%d]',
                 w, h, bpp, self.fw, self.fh, self.ox, self.oy,
                 w * (1 if bpp <= 8 else 2))
        self.dbgn = 0; self.maxx = 0                   # reset per-mode diagnostics
        if self.mm:                                    # clear surround to black
            self.mm[0:self.fw * self.fh * 2] = b'\x00' * (self.fw * self.fh * 2)

    def _setswitch(self, state):
        log.debug('SETSWITCH %d', state)
        if not state and self.mm:
            self.mm[0:self.fw * self.fh * 2] = b'\x00' * (self.fw * self.fh * 2)

    def _dbg(self, kind, x, y, n, sample):
        if x + n > self.maxx:
            self.maxx = x + n
        if self.dbgn < 10:
            self.dbgn += 1
            log.info('  %s x=%d y=%d n=%d data=%s', kind, x, y, n, sample)
        elif self.dbgn == 10:
            self.dbgn += 1
            log.info('  (... maxx so far=%d ...)', self.maxx)

    def _fill(self, x, y, n, px):
        self._dbg('FILL16', x, y, n, px.hex())
        x += self.ox; y += self.oy
        if not self.mm or y >= self.fh or x >= self.fw:
            return
        n = min(n, self.fw - x)
        if n <= 0:
            return
        off = y * self.stride + x * 2
        self.mm[off:off + n * 2] = px * n

    def _pixels(self, x, y, n, data):
        self._dbg('PIX16', x, y, n, data[:8].hex())
        x += self.ox; y += self.oy
        if not self.mm or y >= self.fh or x >= self.fw:
            return
        n = min(n, self.fw - x)
        if n <= 0:
            return
        off = y * self.stride + x * 2
        self.mm[off:off + n * 2] = data[:n * 2]

    # ---- 8-bit (CLUT) -----------------------------------------------------

    def _setpalette(self, start, count, data):
        log.info('  SETPALETTE start=%d count=%d', start, count)
        end = min(start + count, 256)
        self.pal[start * 2:end * 2] = data[:(end - start) * 2]

    def _fill8(self, x, y, n, ci):
        self._dbg('FILL8', x, y, n, '%02x' % ci)
        x += self.ox; y += self.oy
        if not self.mm or y >= self.fh or x >= self.fw:
            return
        n = min(n, self.fw - x)
        if n <= 0:
            return
        px = bytes(self.pal[ci * 2:ci * 2 + 2])
        off = y * self.stride + x * 2
        self.mm[off:off + n * 2] = px * n

    def _pixels8(self, x, y, n, idx):
        self._dbg('PIX8', x, y, n, idx[:8].hex())
        x += self.ox; y += self.oy
        if not self.mm or y >= self.fh or x >= self.fw:
            return
        n = min(n, self.fw - x)
        if n <= 0:
            return
        pal = self.pal
        out = bytearray(n * 2)
        for k in range(n):
            ci = idx[k] * 2
            out[k * 2]     = pal[ci]
            out[k * 2 + 1] = pal[ci + 1]
        off = y * self.stride + x * 2
        self.mm[off:off + n * 2] = out

    # ---- main loop --------------------------------------------------------

    def run(self):
        log.info('RTG service running')
        while True:
            select.select([self.sock], [], [])
            buf = self.sock.recv(65536)
            if not buf:
                log.info('a314d closed connection')
                return
            self._feed(buf)


def main():
    if '-d' in sys.argv:
        log.setLevel(logging.DEBUG)

    if '-ondemand' in sys.argv:
        # a314d spawned us; service already registered, socket fd inherited.
        fd = int(sys.argv[sys.argv.index('-ondemand') + 1])
        sock = socket.socket(fileno=fd)
        sock.setblocking(True)
    else:
        # Standalone: connect and register ourselves.
        sock = socket.socket()
        sock.connect((A314D_HOST, A314D_PORT))
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(struct.pack('=IIB', len(SERVICE_NAME), 0, MSG_REGISTER_REQ)
                     + SERVICE_NAME)

    RTGService(sock).run()


if __name__ == '__main__':
    main()
