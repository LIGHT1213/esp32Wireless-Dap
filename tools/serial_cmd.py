#!/usr/bin/env python3

import argparse
import re
import sys
import time

import serial


def read_until(ser, patterns, timeout_s):
    deadline = time.monotonic() + timeout_s
    buffer = ""
    compiled = [re.compile(p) for p in patterns]
    while time.monotonic() < deadline:
        chunk = ser.read(ser.in_waiting or 1).decode("utf-8", errors="ignore")
        if chunk:
            buffer += chunk
            for pat in compiled:
                if pat.search(buffer):
                    return buffer
        else:
            time.sleep(0.05)
    return buffer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("command", nargs="?")
    parser.add_argument("--cmd", action="append", dest="commands")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-wait", type=float, default=2.0)
    parser.add_argument("--timeout", type=float, default=6.0)
    args = parser.parse_args()

    commands = []
    if args.command is not None:
        commands.append(args.command)
    if args.commands:
        commands.extend(args.commands)
    if not commands:
        raise SystemExit("no command provided")

    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(args.boot_wait)

        # Drain current boot/log noise and wait for prompt if present.
        output = read_until(ser, [r"wdap> "], 1.5)

        for command in commands:
            ser.write((command + "\r").encode("utf-8"))
            ser.flush()
            output += read_until(ser, [r"wdap> "], args.timeout)

    sys.stdout.write(output)


if __name__ == "__main__":
    main()
