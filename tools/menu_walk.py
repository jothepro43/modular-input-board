# Usage: menuwalk.py out.png "keys" ... each key string is sent, then one frame is captured.
import glob, struct, sys, time, zlib, serial
W, H, SC, COLS = 128, 64, 2, 4
s = serial.Serial(glob.glob('/dev/cu.usbmodem*')[0], 115200, timeout=0.2)
time.sleep(0.2)
def frame():
    s.reset_input_buffer(); s.write(b'S'); end = time.time() + 3
    while time.time() < end:
        l = s.readline().decode('utf-8', 'replace').strip()
        if l.startswith('SCREEN ') and len(l) == 7 + 2048: return bytes.fromhex(l[7:])
    return bytes(1024)
frames = []
for keys in sys.argv[2:]:
    for k in keys:
        s.write(k.encode()); time.sleep(0.12)
    time.sleep(0.2)
    frames.append(frame())
rows = (len(frames) + COLS - 1) // COLS
PW, PH = COLS * (W * SC + 6), rows * (H * SC + 6)
img = [bytearray(b'\x40' * PW) for _ in range(PH)]
for n, b in enumerate(frames):
    ox, oy = (n % COLS) * (W * SC + 6), (n // COLS) * (H * SC + 6)
    for y in range(H):
        for x in range(W):
            if b[x + (y // 8) * W] >> (y % 8) & 1:
                for a in range(SC):
                    for c in range(SC): img[oy + y * SC + a][ox + x * SC + c] = 255
            else:
                for a in range(SC):
                    for c in range(SC): img[oy + y * SC + a][ox + x * SC + c] = 0
def chunk(k, d): return struct.pack('>I', len(d)) + k + d + struct.pack('>I', zlib.crc32(k + d))
raw = b''.join(b'\0' + bytes(r) for r in img)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', PW, PH, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
