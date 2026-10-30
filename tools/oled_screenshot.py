#!/usr/bin/env python3
"""Save what the firmware is drawing on the OLED as a PNG.

Sends "S" to modular_remote over serial, which replies with the screen buffer as hex.
This shows what the firmware drew, not what the physical screen is emitting.
Needs pyserial. Usage: oled_screenshot.py out.png
"""
import glob
import struct
import sys
import time
import zlib

import serial

WIDTH, HEIGHT, SCALE = 128, 64, 4


def grab():
    port = serial.Serial(glob.glob('/dev/cu.usbmodem*')[0], 115200, timeout=0.2)
    time.sleep(0.2)
    port.reset_input_buffer()
    port.write(b'S')
    end = time.time() + 3
    while time.time() < end:
        line = port.readline().decode('utf-8', 'replace').strip()
        if line.startswith('NO SCREEN'):
            sys.exit('Firmware reports no screen attached')
        if line.startswith('SCREEN ') and len(line) == 7 + WIDTH * HEIGHT // 4:
            return bytes.fromhex(line[7:])
    sys.exit('No screen data received')


def png(buffer, path):
    rows = []
    for y in range(HEIGHT):
        row = bytearray()
        for x in range(WIDTH):
            lit = buffer[x + (y // 8) * WIDTH] >> (y % 8) & 1
            row += (b'\xff\xff\xff' if lit else b'\x00\x00\x00') * SCALE
        rows += [b'\x00' + bytes(row)] * SCALE

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    header = struct.pack('>IIBBBBB', WIDTH * SCALE, HEIGHT * SCALE, 8, 2, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
                + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


if __name__ == '__main__':
    png(grab(), sys.argv[1])
