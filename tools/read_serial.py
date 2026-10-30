import sys, time, glob, serial
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12
end = time.time() + secs
s = None
while time.time() < end:
    try:
        if s is None:
            ports = glob.glob('/dev/cu.usbmodem*')
            if not ports:
                time.sleep(0.2); continue
            s = serial.Serial(ports[0], 115200, timeout=0.2)
            s.dtr = True; s.rts = True
            print(f'[opened {ports[0]}]', flush=True)
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode('utf-8', 'replace')); sys.stdout.flush()
    except (serial.SerialException, OSError) as e:
        print(f'[port dropped: {e}]', flush=True)
        s = None; time.sleep(0.3)
