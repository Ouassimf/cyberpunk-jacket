#!/usr/bin/env python3
"""Sends BOOT_OTHER_SLOT (PROTOCOL.md, opcode 0x32) to a jacket over Bluetooth LE:
"restart into the firmware in your other app slot".

    python firmware/tools/boot_other_slot.py            (asks before sending)
    python firmware/tools/boot_other_slot.py --yes

On a board that was bought with other firmware and has been updated once, the
other slot holds that other firmware: this is the way back to it without a
button or a serial port. The jacket verifies the image first. If it is good the
jacket closes the Bluetooth link and resets into the other firmware (its own
scene and presets are kept for the next time jacket firmware runs); if there
is none, the jacket answers ERROR code 4 and carries on as before.

Needs `bleak`, like ble_check.py (same virtual environment).

Exit status: 0 the jacket accepted and went away, 2 the jacket refused,
1 anything else (not found, could not connect, no answer either way).
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

BOOT_OTHER_SLOT = bytes([0x32, 0x5A])
N_ERROR = 0x7F
ERRORS = {1: "unknown opcode: this firmware does not know BOOT_OTHER_SLOT",
          2: "frame too short",
          3: "wrong confirmation byte",
          4: "the other app slot holds no firmware that verifies",
          5: "storage failure"}


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
    print(f"Looking for a jacket (up to {args.scan_timeout:.0f} s) ...")
    device, adv = await find(args.scan_timeout, args.address)
    if device is None:
        print("No jacket found. Is it powered, in range, and not connected to a phone?")
        return 1
    print(f"Found {device.address} \"{adv.local_name or '?'}\" at {adv.rssi} dBm")
    if not args.yes:
        answer = input("Restart it into the firmware in its OTHER app slot? Type yes: ")
        if answer.strip().lower() != "yes":
            print("Nothing sent.")
            return 1

    gone = asyncio.Event()
    refused: list[int] = []
    answered = asyncio.Event()

    def on_notify(_, data: bytearray):
        if len(data) >= 3 and data[0] == N_ERROR and data[1] == BOOT_OTHER_SLOT[0]:
            refused.append(data[2])
            answered.set()

    last_error: Exception | None = None
    for attempt in range(1, 5):
        client = BleakClient(device, disconnected_callback=lambda _: gone.set(), timeout=20.0,
                             winrt={"use_cached_services": False})
        try:
            await client.connect()
            await client.start_notify(STATE, on_notify)
            break
        except Exception as error:  # weak signal: try again
            last_error = error
            print(f"  connect attempt {attempt} failed: {type(error).__name__}: {error}")
            try:
                await client.disconnect()
            except Exception:
                pass
            await asyncio.sleep(1.0)
    else:
        print(f"Could not connect: {last_error}")
        return 1

    gone.clear()
    sent = time.monotonic()
    try:
        await client.write_gatt_char(COMMAND, BOOT_OTHER_SLOT, response=True)
        print("Sent 32 5a (BOOT_OTHER_SLOT). Waiting for the jacket to refuse or to go away ...")
    except Exception as error:
        # The jacket may restart before the write is acknowledged.
        print(f"  the write was not acknowledged ({type(error).__name__}); waiting to see whether the jacket went away ...")

    waiters = [asyncio.ensure_future(gone.wait()), asyncio.ensure_future(answered.wait())]
    await asyncio.wait(waiters, timeout=args.wait, return_when=asyncio.FIRST_COMPLETED)
    for waiter in waiters:
        waiter.cancel()

    if refused:
        code = refused[0]
        print(f"REFUSED: ERROR code {code} ({ERRORS.get(code, 'unknown code')}). The jacket carries on with its current firmware.")
        try:
            await client.disconnect()
        except Exception:
            pass
        return 2
    if gone.is_set():
        print(f"ACCEPTED: the jacket dropped the link {time.monotonic() - sent:.1f} s after the command; it is restarting into the other slot.")
        return 0
    print(f"NO ANSWER within {args.wait:.0f} s: the jacket neither refused nor went away. Nothing can be concluded.")
    try:
        await client.disconnect()
    except Exception:
        pass
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Restart a jacket into the firmware in its other app slot (BOOT_OTHER_SLOT).")
    parser.add_argument("--yes", action="store_true", help="do not ask before sending")
    parser.add_argument("--address", help="Bluetooth address, if more than one jacket is in range")
    parser.add_argument("--scan-timeout", type=float, default=25.0, help="seconds to look for the jacket (default 25)")
    parser.add_argument("--wait", type=float, default=15.0, help="seconds to wait for the jacket's reaction (default 15)")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 1


if __name__ == "__main__":
    sys.exit(main())
