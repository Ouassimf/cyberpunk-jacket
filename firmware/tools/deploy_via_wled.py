#!/usr/bin/env python3
"""Builds a jacket firmware and installs it on a board that has WLED in its
other app slot (the Gledopto), without a cable and without WiFi credentials:

    1. build the image
    2. Bluetooth: BOOT_OTHER_SLOT, the jacket restarts into WLED
    3. HTTP: the image goes to WLED's update page, WLED restarts into it
    4. Bluetooth: ask the new firmware which build it is

    python firmware/tools/deploy_via_wled.py
    python firmware/tools/deploy_via_wled.py --env gledopto_bench
    python firmware/tools/deploy_via_wled.py --image some\\firmware.bin

The jacket talks to one device at a time: press Disconnect in the phone app
first, or the PC cannot find it. If WLED already answers, step 2 is skipped.

Where the board is comes from `secrets.ini` (`gledopto_ble`, `gledopto_wled`)
or from --address and --wled. Nothing else is read from that file.

Step 4 compares the build line the jacket reports ("gled-mic Oct 10 2026
21:35:51") with the ones inside the image. So that every build has its own,
the build step recompiles main.cpp, where that line comes from. If the jacket
does not come back, the script looks whether WLED did: a firmware that
crashes at start is given up by the jacket itself and WLED returns.

Needs `bleak`, like ble_check.py (same virtual environment).

Exit status: 0 installed and verified, 1 not installed, 3 uploaded but the
running build could not be read (see the last lines printed).
"""

from __future__ import annotations

import argparse
import asyncio
import configparser
import http.client
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

from bleak import BleakClient

import boot_other_slot

FIRMWARE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_ENV = "gledopto_bench_mic"

SLOT_BYTES = 0x180000            # app slot of WLED's 4 MB partition table
IMAGE_MAGIC = 0xE9
APP_DESCRIPTION_MAGIC = 0xABCD5432  # at offset 0x20 of an app image, not of a bootloader or a merged file
BUILD_LINE = re.compile(rb"[\w.-]{1,24} [A-Z][a-z]{2} [ \d]\d \d{4} \d\d:\d\d:\d\d")

GET_INFO = bytes([0x20])
N_INFO = 0x05


def step(text: str) -> None:
    print(f"\n== {text}")


def local_setting(key: str) -> str | None:
    parser = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(" ;", " #"))
    try:
        parser.read(FIRMWARE_DIR / "secrets.ini", encoding="utf-8")
        return parser.get("secrets", key, fallback="").strip() or None
    except configparser.Error:
        return None


def build(env_name: str) -> Path | None:
    pio = shutil.which("pio") or str(Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe")
    # main.cpp carries the build line (__DATE__ __TIME__); without this an unchanged main.cpp keeps its old one.
    stale = FIRMWARE_DIR / ".pio" / "build" / env_name / "src" / "main.cpp.o"
    if stale.exists():
        stale.unlink()
    environment = dict(os.environ, PYTHONIOENCODING="utf-8")
    environment.pop("MSYSTEM", None)  # PlatformIO refuses to run under Git Bash
    try:
        result = subprocess.run([pio, "run", "-e", env_name, "-d", str(FIRMWARE_DIR)], env=environment,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"Could not start PlatformIO ({pio}): {error}")
        return None
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print("\n".join(output.splitlines()[-40:]))
        print(f"The build of {env_name} failed.")
        return None
    for line in output.splitlines():
        if line.startswith("Image:"):
            print(line)
    return FIRMWARE_DIR / ".pio" / "build" / env_name / "firmware.bin"


def check_image(data: bytes) -> str | None:
    """Returns what is wrong with the image, or None."""
    if len(data) < 0x24 or data[0] != IMAGE_MAGIC:
        return "it is not an ESP32 app image (use firmware.bin; firmware.factory.bin also holds a bootloader)"
    if struct.unpack_from("<I", data, 0x20)[0] != APP_DESCRIPTION_MAGIC:
        return "it is an ESP32 image but not an app (a bootloader?)"
    if len(data) > SLOT_BYTES:
        return f"it is {len(data) - SLOT_BYTES} bytes larger than the app slot"
    return None


def wled_info(host: str, timeout: float = 3.0) -> dict | None:
    """WLED's /json/info, or None when no WLED answers at `host`."""
    connection = http.client.HTTPConnection(host, timeout=timeout)
    try:
        connection.request("GET", "/json/info")
        info = json.loads(connection.getresponse().read())
        return info if isinstance(info, dict) and info.get("brand") == "WLED" else None
    except (OSError, ValueError, http.client.HTTPException):
        return None
    finally:
        connection.close()


def wait_for_wled(host: str, seconds: float) -> dict | None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        info = wled_info(host)
        if info:
            return info
        time.sleep(1.0)
    return None


def upload(host: str, data: bytes, timeout: float) -> tuple[bool | None, str]:
    """Sends the image to WLED's update page. True accepted, False refused, None no usable answer."""
    boundary = "----jacket" + os.urandom(8).hex()
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"update\"; filename=\"firmware.bin\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
    connection = http.client.HTTPConnection(host, timeout=timeout)
    try:
        connection.request("POST", "/update", body=body,
                           headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
        response = connection.getresponse()
        # WLED answers with a whole page, scripts and styles included; its message is a line starting "Update".
        page = response.read().decode("utf-8", "replace")
        message = re.search(r"Update [^<]*", page)
        text = message.group(0).strip() if message else " ".join(re.sub(r"<[^>]+>", " ", page).split())[-120:]
        if response.status == 200 and "success" in text.lower():
            return True, text
        return False, f"HTTP {response.status}: {text}"
    except (OSError, http.client.HTTPException) as error:
        return None, f"{type(error).__name__}: {error}"
    finally:
        connection.close()


def decode_build(frame: bytes) -> tuple[str, str]:
    """(version, build line) of an INFO frame (PROTOCOL.md)."""
    strips = frame[5]
    o = 6 + 2 * strips + 4
    return "%d.%d.%d" % tuple(frame[2:5]), frame[o + 1:o + 1 + frame[o]].decode("utf-8", "replace")


async def running_build(address: str, seconds: float) -> tuple[str, str] | None:
    device, adv = await boot_other_slot.find(seconds, address)
    if device is None:
        return None
    print(f"Found {device.address} \"{adv.local_name or '?'}\" at {adv.rssi} dBm")
    for attempt in range(1, 5):
        answer: asyncio.Future = asyncio.get_running_loop().create_future()

        def on_notify(_, data: bytearray):
            if data and data[0] == N_INFO and not answer.done():
                answer.set_result(bytes(data))

        client = BleakClient(device, timeout=20.0, winrt={"use_cached_services": False})
        try:
            await client.connect()
            await client.start_notify(boot_other_slot.STATE, on_notify)
            await client.write_gatt_char(boot_other_slot.COMMAND, GET_INFO, response=True)
            return decode_build(await asyncio.wait_for(answer, 5.0))
        except Exception as error:  # weak signal, or the jacket still starting: try again
            print(f"  attempt {attempt} failed: {type(error).__name__}: {error}")
            await asyncio.sleep(2.0)
        finally:
            try:
                await client.disconnect()
            except Exception:
                pass
    return None


async def run(args) -> int:
    address = args.address or local_setting("gledopto_ble")
    host = args.wled or local_setting("gledopto_wled")
    if not address or not host:
        print("Where is the board? Set gledopto_ble and gledopto_wled in secrets.ini, or pass --address and --wled.")
        return 1

    if args.image:
        step(f"1/4 Image: {args.image}")
        path = Path(args.image)
    else:
        step(f"1/4 Building {args.env}")
        path = build(args.env)
        if path is None:
            return 1
    try:
        image = path.read_bytes()  # read once: what was checked is what is sent
    except OSError as error:
        print(f"Cannot read {path}: {error}")
        return 1
    problem = check_image(image)
    if problem:
        print(f"{path.name} cannot be installed: {problem}.")
        return 1
    build_lines = {line.decode() for line in BUILD_LINE.findall(image)}
    print(f"{path.name}: {len(image)} bytes, {100.0 * len(image) / SLOT_BYTES:.1f} % of the app slot"
          + (f", build {' / '.join(sorted(build_lines))}" if build_lines else ""))

    step("2/4 Restarting the jacket into WLED")
    info = wled_info(host)
    if info:
        print(f"WLED {info.get('ver', '?')} already answers at {host}: nothing to send.")
    else:
        result = await boot_other_slot.run(argparse.Namespace(address=address, yes=True, scan_timeout=args.scan_timeout, wait=15.0))
        if result != 0:
            print("Nothing was installed. The jacket runs what it ran before.")
            return 1
        info = wait_for_wled(host, 60.0)
        if not info:
            print(f"The jacket went away but no WLED answers at {host}. Has its address changed? Look in the router,\n"
                  "then run this again with --wled: the jacket is in WLED now, and that is where the image has to go.")
            return 1
        print(f"WLED {info.get('ver', '?')} answers at {host}.")

    step("3/4 Uploading")
    started = time.monotonic()
    accepted, text = upload(host, image, args.upload_timeout)
    if accepted is False:
        print(f"WLED refused the image: {text}\nThe jacket is still running WLED. Run this again once the cause is fixed.")
        return 1
    if accepted:
        print(f"WLED: \"{text}\" after {time.monotonic() - started:.0f} s.")
    else:
        print(f"No answer from WLED ({text}). That alone proves nothing; what starts next does.")

    step("4/4 Asking the jacket what it runs")
    running = await running_build(address, 60.0)
    if running is None:
        info = wled_info(host)
        if info and accepted is None:
            print("WLED still answers: the upload did not happen. Run this again.")
            return 1
        if info:
            print("NOT INSTALLED: WLED is back. The new firmware started and was given up (it crashed at start).")
            return 1
        print("UNVERIFIED: the jacket was not found over Bluetooth and WLED does not answer either.\n"
              "If a phone reconnected to it meanwhile, the app's Settings show the firmware it runs.")
        return 3
    version, line = running
    print(f"The jacket runs firmware {version}, build \"{line}\".")
    if line in build_lines:
        print("INSTALLED: that is the image just uploaded.")
        return 0
    if not build_lines:
        print("UNVERIFIED: no build line was found in the image to compare with.")
        return 3
    print("NOT INSTALLED: that is a different build from the image.")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a jacket firmware and install it through the WLED in the board's other app slot.")
    parser.add_argument("--env", default=DEFAULT_ENV, help=f"PlatformIO environment to build (default {DEFAULT_ENV})")
    parser.add_argument("--image", help="install this firmware.bin instead of building")
    parser.add_argument("--address", help="the board's Bluetooth address (default: gledopto_ble in secrets.ini)")
    parser.add_argument("--wled", help="address or host name WLED answers at (default: gledopto_wled in secrets.ini)")
    parser.add_argument("--scan-timeout", type=float, default=25.0, help="seconds to look for the jacket (default 25)")
    parser.add_argument("--upload-timeout", type=float, default=120.0, help="seconds to wait for WLED's answer to the upload (default 120)")
    args = parser.parse_args()
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 1


if __name__ == "__main__":
    sys.exit(main())
