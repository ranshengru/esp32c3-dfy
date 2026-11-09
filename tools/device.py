"""Load the companion into ESP32-C3 RAM and keep its console open."""

import argparse
from pathlib import Path
import re
import select
import sys
import time

from esptool.cmds import detect_chip, load_ram

def wait_for(port, pattern, timeout=8):
    end = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < end:
        data.extend(port.read(128))
        if b"PET ERROR" in data:
            raise RuntimeError(f"Device reported an error: {data[-200:]!r}")
        match = re.search(pattern, data)
        if match:
            print(match.group(0).decode("ascii"), flush=True)
            return match
    raise RuntimeError(f"Device response timed out: {data[-200:]!r}")


def send(port, message):
    command = ("SAY " + message + "\n").encode("utf-8")
    if not message.strip() or len(command) > 193 or any(ord(c) < 32 for c in message):
        raise ValueError("Use a single nonempty line of at most 188 UTF-8 bytes")
    for offset in range(0, len(command), 32):
        port.write(command[offset:offset + 32])
        time.sleep(0.09)


def dimensions(port):
    match = wait_for(port, rb"BUBBLE (\d+) (\d+) ([12])\r?\n")
    return tuple(int(group) for group in match.groups())


def demonstrate(port):
    send(port, "在呢")
    short = dimensions(port)
    if short[2] != 1:
        raise RuntimeError(f"Expected a single-line bubble, received {short}")
    time.sleep(1.5)

    send(port, "今天也要开心呀，累了就歇一会儿，记得照顾好自己哦！")
    wide = dimensions(port)
    if wide[0] <= short[0] or wide[1] <= short[1] or wide[2] != 2:
        raise RuntimeError(f"Long text did not expand the bubble: {wide}")
    dimensions(port)
    wait_for(port, rb"BUBBLE 0 0 0", timeout=5)
    print("Verified short, wrapped, paginated, and cleared bubbles", flush=True)


def demonstrate_motion(port):
    port.write(b"PET\n")
    timing = wait_for(port, rb"MOTION START\r?\nBUBBLE [1-9]\d* \d+ [12]\r?\nMOTION DONE (10) (\d+)\r?\n")
    frames, elapsed_ms = map(int, timing.groups())
    print(f"Measured animation: {frames} frames in {elapsed_ms} ms ({1000 * frames / elapsed_ms:.1f} FPS)", flush=True)


def monitor(port):
    interactive = sys.stdin.isatty()
    if interactive:
        print("Type a message to display it, /pet to animate, or /quit to exit.", flush=True)
    while True:
        line = port.readline()
        if line:
            print(line.decode("utf-8", "replace").strip(), flush=True)
        if not interactive or not select.select([sys.stdin], [], [], 0)[0]:
            continue
        command = sys.stdin.readline().strip()
        if command == "/quit":
            return
        if command == "/pet":
            port.write(b"PET\n")
        elif command:
            try:
                send(port, command)
            except ValueError as error:
                print(error, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--demo", action="store_true")
    args = parser.parse_args()

    esp = detect_chip(args.port, connect_mode="usb-reset", connect_attempts=2)
    try:
        if esp.CHIP_NAME != "ESP32-C3" or esp.secure_download_mode:
            raise RuntimeError("Expected an ESP32-C3 in regular download mode")
        load_ram(esp, str(args.image))
        esp._port.timeout = 0.15
        # The first bubble follows readiness, potentially in the same USB packet.
        wait_for(esp._port, rb"PET READY v0\.1\r?\nBUBBLE [1-9]\d* \d+ [12]")
        if args.demo:
            esp._port.write(b"TEST\n")
            try:
                time.sleep(2)
                demonstrate(esp._port)
                demonstrate_motion(esp._port)
            finally:
                esp._port.write(b"LIVE\n")
        print("Companion is running in RAM; keeping the USB session open.", flush=True)
        monitor(esp._port)
    except KeyboardInterrupt:
        pass
    finally:
        esp._port.close()


if __name__ == "__main__":
    main()
