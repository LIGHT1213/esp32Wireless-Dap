import sys
import time

import serial


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: capture_serial.py <port> <seconds> <outfile>")
        return 2

    port = sys.argv[1]
    seconds = float(sys.argv[2])
    outfile = sys.argv[3]

    end_time = time.time() + seconds
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.rtscts = False
    ser.dsrdtr = False
    # Keep the ESP32 auto-reset lines deasserted before and after open so
    # passive log capture does not reboot the probe during Keil tests.
    ser.dtr = False
    ser.rts = False

    with open(outfile, "wb") as out:
        ser.open()
        ser.dtr = False
        ser.rts = False
        while time.time() < end_time:
            data = ser.read(4096)
            if data:
                out.write(data)
                out.flush()
        ser.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
