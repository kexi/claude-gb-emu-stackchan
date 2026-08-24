# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial==3.5"]
# ///
"""Capture a serial port for a bounded duration without requiring an interactive TTY."""

from __future__ import annotations

import argparse
import sys
import time

import serial
from serial import SerialException


def positive_seconds(value: str) -> int:
    seconds = int(value)
    if seconds <= 0:
        raise argparse.ArgumentTypeError("duration must be a positive integer")
    return seconds


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("port")
    parser.add_argument("duration", type=positive_seconds)
    return parser.parse_args()


def open_device(port: str, deadline: float) -> serial.Serial:
    while time.monotonic() < deadline:
        try:
            return serial.Serial(port, 115200, timeout=0.25, exclusive=True)
        except (OSError, SerialException):
            time.sleep(0.25)
    raise RuntimeError(f"serial port did not become available: {port}")


def reset_device(port: str) -> None:
    reset_deadline = time.monotonic() + 5
    device = open_device(port, reset_deadline)
    try:
        # ESP32 auto-reset: keep GPIO0 deasserted while pulsing EN low.
        device.dtr = False
        device.rts = True
        time.sleep(0.1)
        device.rts = False
    finally:
        device.close()
    time.sleep(0.5)


def main() -> int:
    args = parse_args()
    if args.reset:
        reset_device(args.port)

    deadline = time.monotonic() + args.duration
    device: serial.Serial | None = None
    try:
        while time.monotonic() < deadline:
            if device is None:
                device = open_device(args.port, deadline)
            try:
                chunk = device.read(device.in_waiting or 1)
            except (OSError, SerialException):
                device.close()
                device = None
                time.sleep(0.25)
                continue
            if not chunk:
                continue
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
    finally:
        if device is not None:
            device.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
