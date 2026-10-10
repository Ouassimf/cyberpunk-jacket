#!/usr/bin/env python3
"""Prints a jacket's `status` line over Bluetooth LE: what its serial heartbeat
reports, for a board without a serial port (PROTOCOL.md "Diagnostics").

    python firmware/tools/read_status.py --address AA:BB:CC:DD:EE:FF
    python firmware/tools/read_status.py --address AA:BB:CC:DD:EE:FF --count 12

The line is refreshed by the jacket every 5 s and is only filled in while dev
mode is on. If dev mode is off the script switches it on for as long as it
runs and off again at the end; on a build with WiFi credentials that starts
WiFi meanwhile. With --effect N it also selects effect N on every segment
first (7 Beat Pulse and 8 VU Meter make a built-in microphone run) and puts
back what was there.

On a board with a microphone the line ends like this:

    | mic=on load=7.4% worst=21300us ovf=0 stack=3900 slot=L pk=812/3 fr=166 snd=mic

mic: capturing or not. load: share of one CPU core. worst: the longest single
processing call. ovf: 10 ms buffers lost since boot. slot: the half of the I2S
frame in use. pk: the loudest sample in the slot in use / in the other one,
since the previous line, as 16-bit values (32767 is clipping). fr: live frames
made. snd: who supplies sound to the effects now (mic, phone, none).

Needs `bleak`, like ble_check.py (same virtual environment).
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "89960001-beff-438f-8710-5e7d3dff8891"
COMMAND = "89960002-beff-438f-8710-5e7d3dff8891"
STATE = "89960003-beff-438f-8710-5e7d3dff8891"
STATUS = "8996d002-beff-438f-8710-5e7d3dff8891"

FLAG_DEV = 0x01
SEGMENT_SIZE = 14  # geometry 4 bytes + EffectConfig 10 bytes


async def find(timeout: float, address: str | None):
    found = asyncio.Event()
    hit: dict = {}

    def on_advertisement(device, adv):
        if SERVICE not in [u.lower() for u in adv.service_uuids]:
            return
        if address and device.address.lower() != address.lower():
            return
        hit.update(device=device, adv=adv)
        found.set()

    async with BleakScanner(on_advertisement, service_uuids=[SERVICE]):
        try:
            await asyncio.wait_for(found.wait(), timeout)
        except asyncio.TimeoutError:
            return None, None
    return hit["device"], hit["adv"]


async def run(args) -> int:
    if not args.address:
        print("note: no --address given: the first jacket heard is used")
    device, adv = await find(args.scan_timeout, args.address)
    if device is None:
        print("No jacket found. Is it powered, in range, and not connected to a phone?")
        return 1
    print(f"Found {device.address} at {adv.rssi} dBm")

    last_error: Exception | None = None
    for attempt in range(1, 5):
        client = BleakClient(device, timeout=20.0, winrt={"use_cached_services": False})
        try:
            await client.connect()
            break
        except Exception as error:  # weak signal: try again
            last_error = error
            print(f"  connect attempt {attempt} failed: {type(error).__name__}: {error}")
            await asyncio.sleep(1.0)
    else:
        print(f"Could not connect: {last_error}")
        return 1

    try:
        if client.services.get_characteristic(STATUS) is None:
            print("This firmware has no `status` characteristic.")
            return 1
        state = bytes(await client.read_gatt_char(STATE))
        dev_was_on = len(state) > 8 and bool(state[8] & FLAG_DEV)
        segments = [state[11 + i * SEGMENT_SIZE: 11 + (i + 1) * SEGMENT_SIZE] for i in range(state[10])] if len(state) > 10 else []
        if not dev_was_on:
            await client.write_gatt_char(COMMAND, bytes([0x31, 0x01]), response=True)
            print("dev mode was off: switched on for the duration")
        if args.effect is not None:
            # SET_PARAM effect on every segment: keeps colours, palette, speed and intensity.
            await client.write_gatt_char(COMMAND, bytes([0x04, 0xFF, 0x01, args.effect]), response=True)
            print(f"effect {args.effect} selected on every segment")

        previous = None
        shown = 0
        deadline = time.monotonic() + args.count * args.interval + 10
        while shown < args.count and time.monotonic() < deadline:
            text = bytes(await client.read_gatt_char(STATUS)).decode("utf-8", "replace")
            if text != previous and text != "dev mode is off":
                print(f"{time.strftime('%H:%M:%S')}  {text}")
                previous = text
                shown += 1
            await asyncio.sleep(1.0)

        if args.effect is not None:
            for index, segment in enumerate(segments):
                await client.write_gatt_char(COMMAND, bytes([0x04, index, 0x01, segment[4]]), response=True)
            print("effects put back")
        if not dev_was_on:
            await client.write_gatt_char(COMMAND, bytes([0x31, 0x00]), response=True)
            print("dev mode switched off again")
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Print a jacket's status line over Bluetooth LE.")
    parser.add_argument("--address", help="Bluetooth address of the jacket (always give it when more than one board is powered)")
    parser.add_argument("--count", type=int, default=6, help="how many lines to print before leaving (default 6, one per 5 s)")
    parser.add_argument("--interval", type=float, default=5.0, help="seconds between two lines of the jacket (default 5)")
    parser.add_argument("--effect", type=int, help="select this effect on every segment while reading, and put back what was there")
    parser.add_argument("--scan-timeout", type=float, default=25.0, help="seconds to look for the jacket (default 25)")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 1


if __name__ == "__main__":
    sys.exit(main())
