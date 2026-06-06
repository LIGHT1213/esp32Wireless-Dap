import serial, sys, time
port=sys.argv[1]
out=sys.argv[2]
duration=float(sys.argv[3])
end=time.time()+duration
with serial.Serial(port, 115200, timeout=0.1) as ser, open(out, 'wb') as f:
    while time.time()<end:
        data=ser.read(4096)
        if data:
            f.write(data)
            f.flush()
