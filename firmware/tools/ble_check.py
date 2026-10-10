#!/usr/bin/env python3
"""Hardware check: drives a real jacket over Bluetooth LE and compares every
answer with protocol/PROTOCOL.md.

    python firmware/tools/ble_check.py --port COM7

Needs `bleak` (and `pyserial` for --port):

    python -m venv %LOCALAPPDATA%\\jacket-venv          (keep the path short on Windows)
    %LOCALAPPDATA%\\jacket-venv\\Scripts\\pip install -r firmware/tools/requirements.txt

With --port the script also watches the serial log while it runs (resets,
panics, heap, frame time) and resets the board to check persistence. Without
it those parts are skipped.

What it changes on the jacket: the scene, brightness, budget and dev mode are
changed many times and put back at the end; preset slot 2 is saved and deleted
(the check refuses to touch presets if slot 2 is in use, unless
--overwrite-presets is given). Dev mode is switched on and off, which on a
build with WiFi credentials starts and stops WiFi.

Exit status 0 when every check passed.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import re
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from bleak import BleakClient, BleakScanner

SERVICE = "89960001-beff-438f-8710-5e7d3dff8891"
COMMAND = "89960002-beff-438f-8710-5e7d3dff8891"
STATE = "89960003-beff-438f-8710-5e7d3dff8891"
LIVE = "89960004-beff-438f-8710-5e7d3dff8891"

VECTORS_FILE = Path(__file__).resolve().parents[2] / "protocol" / "vectors" / "v1.json"

# Notification types.
N_STATE, N_CATALOG, N_PRESETS, N_INFO, N_ERROR = 0x01, 0x02, 0x04, 0x05, 0x7F

# PROTOCOL.md "Catalogue ids (v1 firmware)".
# Ids 7..9 react to live input (param_flags bit 5 sound, bit 6 motion).
EFFECTS = [(0, "Solid", 0x01), (1, "Comet", 0x1D), (2, "Rainbow", 0x18), (3, "Matrix", 0x1C),
           (4, "Breathe", 0x19), (5, "Heartbeat", 0x19), (6, "Cylon", 0x1C),
           (7, "Beat Pulse", 0x3B), (8, "VU Meter", 0x3C), (9, "Sparkle", 0x5C)]
BEAT_PULSE, VU_METER, SPARKLE = 7, 8, 9
PALETTES = [(0, "Colours"), (1, "Rainbow"), (2, "Matrix"), (3, "Cyberpunk"), (4, "Fire"), (5, "Ocean")]

FLAG_DEV, FLAG_WIFI, FLAG_LIVE = 0x01, 0x02, 0x04


# ---- Decoders, written from PROTOCOL.md ---------------------------------------------


@dataclass
class Segment:
    strip_mask: int
    start: int
    length: int
    flags: int
    config: bytes  # the 10-byte EffectConfig

    @property
    def geometry(self) -> bytes:
        return bytes([self.strip_mask, self.start, self.length, self.flags])


@dataclass
class State:
    raw: bytes
    proto: int
    rev: int
    power: int
    brightness: int
    budget_ma: int
    flags: int
    preset: int
    segments: list[Segment]

    def scene(self) -> tuple:
        """What a preset stores and what must survive a reboot."""
        return (self.brightness, [(s.geometry, s.config) for s in self.segments])


def decode_state(b: bytes) -> State:
    if len(b) < 11 or b[0] != N_STATE:
        raise ValueError(f"not a STATE frame: {b.hex()}")
    count = b[10]
    if not 1 <= count <= 8 or len(b) < 11 + 14 * count:
        raise ValueError(f"STATE with a bad segment count or cut short: {b.hex()}")
    segments = []
    for i in range(count):
        o = 11 + 14 * i
        segments.append(Segment(b[o], b[o + 1], b[o + 2], b[o + 3], bytes(b[o + 4:o + 14])))
    return State(bytes(b), b[1], b[2] | b[3] << 8, b[4], b[5], b[6] | b[7] << 8, b[8], b[9], segments)


def read_str(b: bytes, o: int) -> tuple[str, int]:
    n = b[o]
    if o + 1 + n > len(b):
        raise ValueError("string runs past the end of the frame")
    return b[o + 1:o + 1 + n].decode("utf-8", "replace"), o + 1 + n


def decode_catalog(b: bytes) -> dict:
    if len(b) < 5 or b[0] != N_CATALOG:
        raise ValueError(f"not a CATALOG frame: {b.hex()}")
    kind, start, total, count = b[1], b[2], b[3], b[4]
    entries, o = [], 5
    for _ in range(count):
        entry_id = b[o]
        o += 1
        flags = None
        if kind == 0:
            flags = b[o]
            o += 1
        name, o = read_str(b, o)
        entries.append((entry_id, name, flags) if kind == 0 else (entry_id, name))
    if o != len(b):
        raise ValueError(f"CATALOG has {len(b) - o} stray bytes")
    return {"kind": kind, "start": start, "total": total, "entries": entries}


def decode_presets(b: bytes) -> list[tuple[int, str]]:
    if len(b) < 2 or b[0] != N_PRESETS:
        raise ValueError(f"not a PRESETS frame: {b.hex()}")
    out, o = [], 2
    for _ in range(b[1]):
        slot = b[o]
        name, o = read_str(b, o + 1)
        out.append((slot, name))
    if o != len(b):
        raise ValueError(f"PRESETS has {len(b) - o} stray bytes")
    return out


def decode_info(b: bytes) -> dict:
    if len(b) < 6 or b[0] != N_INFO:
        raise ValueError(f"not an INFO frame: {b.hex()}")
    strips = b[5]
    o = 6
    leds = [b[o + 2 * i] | b[o + 2 * i + 1] << 8 for i in range(strips)]
    o += 2 * strips
    max_segments, preset_slots, effect_count, palette_count = b[o:o + 4]
    build, o = read_str(b, o + 4)
    return {"proto": b[1], "fw": (b[2], b[3], b[4]), "leds": leds, "max_segments": max_segments,
            "preset_slots": preset_slots, "effect_count": effect_count, "palette_count": palette_count,
            "build": build}


def hexs(b: bytes) -> str:
    return bytes(b).hex() or "(empty)"


# ---- Results ---------------------------------------------------------------------------


class Report:
    def __init__(self, verbose: bool):
        self.verbose = verbose
        self.results: list[tuple[str, str, str, str]] = []  # (section, status, name, detail)
        self.section_name = ""
        self.notes: list[str] = []

    def section(self, name: str) -> None:
        self.section_name = name
        print(f"\n== {name}")

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        status = "PASS" if ok else "FAIL"
        self.results.append((self.section_name, status, name, detail))
        if not ok or self.verbose or detail:
            print(f"  {status}  {name}" + (f"  [{detail}]" if detail else ""))
        else:
            print(f"  {status}  {name}")
        return ok

    def equal(self, name: str, expected, actual) -> bool:
        return self.check(name, expected == actual, "" if expected == actual else f"expected {expected!r}, got {actual!r}")

    def skip(self, name: str, why: str) -> None:
        self.results.append((self.section_name, "SKIP", name, why))
        print(f"  SKIP  {name}  [{why}]")

    def note(self, text: str) -> None:
        self.notes.append(text)
        print(f"  note  {text}")

    def summary(self) -> int:
        passed = sum(1 for r in self.results if r[1] == "PASS")
        failed = [r for r in self.results if r[1] == "FAIL"]
        skipped = [r for r in self.results if r[1] == "SKIP"]
        print("\n" + "=" * 78)
        sections: dict[str, list[int]] = {}
        for section, status, _, _ in self.results:
            row = sections.setdefault(section, [0, 0, 0])
            row[("PASS", "FAIL", "SKIP").index(status)] += 1
        for section, (p, f, s) in sections.items():
            mark = "FAIL" if f else "ok  "
            print(f"  {mark}  {section:<44} {p:3d} passed" + (f", {f} FAILED" if f else "") + (f", {s} skipped" if s else ""))
        print("-" * 78)
        for section, _, name, detail in failed:
            print(f"  FAILED   {section}: {name}  [{detail}]")
        for section, _, name, detail in skipped:
            print(f"  skipped  {section}: {name}  [{detail}]")
        if self.notes:
            print("  Observations:")
            for text in self.notes:
                print(f"    - {text}")
        print(f"\nRESULT: {'PASS' if not failed else 'FAIL'} - {passed} passed, {len(failed)} failed, {len(skipped)} skipped")
        return 0 if not failed else 1


# ---- Serial log watcher ----------------------------------------------------------------

HEARTBEAT = re.compile(
    r"\[hb\] up=(?P<up>\d+)s heap=(?P<heap>\d+) min_heap=(?P<min_heap>\d+) stack=(?P<stack>\d+) \| ble=(?P<ble>.*?) "
    r"cmds=(?P<cmds>\d+) dropped=(?P<dropped_cmds>\d+)/(?P<dropped_replies>\d+)(?:/(?P<oversize>\d+))? \| fps=(?P<fps>[\d.]+) "
    r"frame=(?P<frame_avg>\d+)/(?P<frame_max>\d+)us \| power=(?P<power>\w+) bri=(?P<bri>\d+) out=(?P<out>\d+)"
    r"(?P<limited>\(limited\))? budget=(?P<budget>\d+)mA est=(?P<est>\d+)mA")
LIVE_STATUS = re.compile(r"live=(?P<live>\S+) rx=(?P<live_rx>\d+) beat=(?P<bpm>[\d.]+)bpm/(?P<beat>\w+) late=(?P<late>\d+)ms")
# Only on a board with a microphone of its own: whether it is capturing, and who supplies the sound.
MIC_STATUS = re.compile(r"mic=(?P<mic>\w+) .*snd=(?P<snd>\w+)")
TROUBLE = re.compile(r"Guru Meditation|Backtrace:|abort\(\) was called|Rebooting\.\.\.|assert failed|Brownout|"
                     r"task_wdt|stack overflow|CORRUPT HEAP|FAILED", re.IGNORECASE)


class SerialWatcher:
    """Reads the jacket's serial log on a thread, without resetting the board."""

    def __init__(self, port: str):
        import serial  # pyserial; only needed with --port

        self.lines: list[tuple[float, str]] = []
        self.expected_boots = 0
        self._lock = threading.Lock()
        self._stop = False
        self._serial = serial.Serial()
        self._serial.port = port
        self._serial.baudrate = 115200
        self._serial.timeout = 0.2
        self._serial.dtr = False  # both lines released: opening the port must not reset the ESP32
        self._serial.rts = False
        self._serial.open()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        pending = b""
        while not self._stop:
            try:
                data = self._serial.read(4096)
            except Exception as error:  # the port went away
                with self._lock:
                    self.lines.append((time.monotonic(), f"<serial read failed: {error}>"))
                return
            if not data:
                continue
            pending += data
            *complete, pending = pending.split(b"\n")
            now = time.monotonic()
            with self._lock:
                for raw in complete:
                    self.lines.append((now, raw.decode("utf-8", "replace").rstrip("\r")))

    def reset_board(self) -> None:
        """Pulses EN through the devkit's auto-reset circuit (RTS asserted, DTR released)."""
        self.expected_boots += 1
        self._serial.dtr = False
        self._serial.rts = True
        time.sleep(0.15)
        self._serial.rts = False

    def snapshot(self) -> list[tuple[float, str]]:
        with self._lock:
            return list(self.lines)

    def since(self, t: float) -> list[str]:
        return [line for when, line in self.snapshot() if when >= t]

    async def wait_for(self, pattern: str, since: float, timeout: float) -> str | None:
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.since(since):
                if regex.search(line):
                    return line
            await asyncio.sleep(0.1)
        return None

    def close(self) -> None:
        self._stop = True
        self._thread.join(timeout=2)
        self._serial.close()


# ---- The jacket, as a central sees it ----------------------------------------------------


class Jacket:
    def __init__(self, report: Report):
        self.report = report
        self.client: BleakClient | None = None
        self.notes: list[tuple[float, bytes]] = []  # every notification: (monotonic time, frame)
        self._arrived = asyncio.Event()
        self.disconnected = False
        self.last_activity = 0.0             # last write or notification
        self.latencies: list[float] = []     # write-with-response -> STATE notification, seconds

    # -- link --

    @staticmethod
    async def find(timeout: float, address: str | None = None):
        """First advertisement that carries the jacket's service UUID. Returns (device, adv, seconds)."""
        found = asyncio.Event()
        hit: dict = {}

        def on_advertisement(device, adv):
            if SERVICE not in [u.lower() for u in adv.service_uuids]:
                return
            if address and device.address.lower() != address.lower():
                return
            if "device" not in hit or adv.local_name:
                hit.update(device=device, adv=adv)
            hit.setdefault("first", time.monotonic())
            found.set()

        started = time.monotonic()
        async with BleakScanner(on_advertisement, service_uuids=[SERVICE]):
            try:
                await asyncio.wait_for(found.wait(), timeout)
            except asyncio.TimeoutError:
                return None, None, timeout
            # The scan response (which carries the name) is a separate packet:
            # with a weak signal it can take a few more advertising events.
            for _ in range(80):
                if hit["adv"].local_name:
                    break
                await asyncio.sleep(0.1)
        return hit["device"], hit["adv"], hit["first"] - started

    async def connect(self, device, attempts: int = 4) -> None:
        last_error: Exception | None = None
        for attempt in range(1, attempts + 1):
            self.disconnected = False
            client = BleakClient(device, disconnected_callback=self._on_disconnect, timeout=20.0,
                                 winrt={"use_cached_services": False})
            try:
                await client.connect()
                self.client = client
                await client.start_notify(STATE, self._on_notify)
                return
            except Exception as error:  # weak signal: try again
                last_error = error
                print(f"  ....  connect attempt {attempt} failed: {type(error).__name__}: {error}")
                try:
                    await client.disconnect()
                except Exception:
                    pass
                await asyncio.sleep(1.5)
        raise RuntimeError(f"could not connect after {attempts} attempts: {last_error}")

    async def disconnect(self) -> None:
        if self.client is not None:
            try:
                await self.client.disconnect()
            except Exception:
                pass
            self.client = None

    def _on_disconnect(self, _client) -> None:
        self.disconnected = True
        self._arrived.set()

    def _on_notify(self, _sender, data: bytearray) -> None:
        self.last_activity = time.monotonic()
        self.notes.append((self.last_activity, bytes(data)))
        self._arrived.set()

    # -- I/O --

    async def write(self, frame: bytes | str, response: bool = True) -> None:
        if isinstance(frame, str):
            frame = bytes.fromhex(frame)
        await self.client.write_gatt_char(COMMAND, frame, response=response)
        self.last_activity = time.monotonic()

    async def write_live(self, frame: bytes) -> None:
        await self.client.write_gatt_char(LIVE, frame, response=False)
        self.last_activity = time.monotonic()

    async def settle(self, quiet: float = 0.25) -> State:
        """Waits until nothing was written or notified for `quiet` seconds (the jacket
        announces every change within about 100 ms), then reads the state."""
        while True:
            idle = time.monotonic() - self.last_activity
            if idle >= quiet:
                return await self.read_state()
            await asyncio.sleep(quiet - idle)

    async def read_state(self) -> State:
        return decode_state(bytes(await self.client.read_gatt_char(STATE)))

    def mark(self) -> int:
        """A position in the notification log: 'everything after now'."""
        return len(self.notes)

    async def wait(self, predicate, since: int, timeout: float = 3.0) -> bytes | None:
        """The first notification after `since` that satisfies predicate(frame)."""
        deadline = time.monotonic() + timeout
        index = since
        while True:
            while index < len(self.notes):
                frame = self.notes[index][1]
                index += 1
                try:
                    if predicate(frame):
                        return frame
                except Exception:
                    pass
            remaining = deadline - time.monotonic()
            if remaining <= 0 or self.disconnected:
                return None
            self._arrived.clear()
            try:
                await asyncio.wait_for(self._arrived.wait(), remaining)
            except asyncio.TimeoutError:
                return None

    async def quiet(self, since: int, seconds: float = 0.5) -> list[bytes]:
        """Everything that arrives in the next `seconds` (after `since`)."""
        await asyncio.sleep(seconds)
        return [frame for _, frame in self.notes[since:]]

    async def command(self, frame: bytes | str, timeout: float = 3.0) -> State | None:
        """Sends a state-changing command and returns the STATE notification that announces it."""
        before = await self.settle()
        since = self.mark()
        sent = time.monotonic()
        await self.write(frame)
        want = (before.rev + 1) & 0xFFFF
        got = await self.wait(lambda f: f[0] == N_STATE and decode_state(f).rev == want, since, timeout)
        if got is None:
            return None
        arrived = next(t for t, f in self.notes[since:] if f is got)
        self.latencies.append(arrived - sent)
        return decode_state(got)

    async def ask(self, frame: bytes | str, reply_type: int, timeout: float = 3.0) -> bytes | None:
        """Sends a command and returns the first notification of `reply_type` that follows."""
        since = self.mark()
        await self.write(frame)
        return await self.wait(lambda f: f[0] == reply_type, since, timeout)

    async def expect_error(self, name: str, frame: bytes | str, code: int, opcode: int | None = None) -> bool:
        if isinstance(frame, str):
            frame = bytes.fromhex(frame)
        if opcode is None:
            opcode = frame[0] if frame else 0
        since = self.mark()
        await self.write(frame)
        got = await self.wait(lambda f: f[0] in (N_ERROR, N_STATE), since)
        expected = bytes([N_ERROR, opcode, code])
        return self.report.check(name, got == expected,
                                 f"sent {hexs(frame)}, expected {hexs(expected)}, got {hexs(got) if got else 'nothing'}")


# ---- The checks --------------------------------------------------------------------------


def paced(count: int, per_second: float):
    """Yields (index, seconds to sleep) so `count` sends are spread evenly at `per_second`."""
    start = time.monotonic()
    for i in range(count):
        delay = start + i / per_second - time.monotonic()
        yield i, max(0.0, delay)


async def set_scene(jacket: Jacket, state: State) -> None:
    """Puts back a scene read earlier (segments, their configs, brightness)."""
    await jacket.write(bytes([0x05, len(state.segments)]) + b"".join(s.geometry for s in state.segments))
    for i, segment in enumerate(state.segments):
        await jacket.write(bytes([0x03, i]) + segment.config)
    await jacket.write(bytes([0x02, state.brightness]))


async def run(args, report: Report, serial: SerialWatcher | None) -> None:
    jacket = Jacket(report)
    try:
        await checks(args, report, serial, jacket)
    finally:
        await jacket.disconnect()


async def next_heartbeats(serial: SerialWatcher, count: int, timeout: float = 20.0) -> list[dict]:
    """The next `count` heartbeat lines the jacket prints from now on."""
    since = time.monotonic()
    deadline = since + timeout
    while time.monotonic() < deadline:
        beats = heartbeats(serial.since(since))
        if len(beats) >= count:
            return beats[:count]
        await asyncio.sleep(0.2)
    return []


def heartbeats(lines: list[str]) -> list[dict]:
    """The heartbeat lines among `lines`, parsed (with the live / beat status when the firmware prints it)."""
    out = []
    for line in lines:
        m = HEARTBEAT.search(line)
        if not m:
            continue
        beat = m.groupdict()
        live = LIVE_STATUS.search(line)
        if live:
            beat.update(live.groupdict())
        mic = MIC_STATUS.search(line)
        if mic:
            beat.update(mic.groupdict())
        out.append(beat)
    return out


async def checks(args, report: Report, serial: SerialWatcher | None, jacket: Jacket) -> None:
    vectors = {v["name"]: v for v in json.loads(VECTORS_FILE.read_text(encoding="utf-8"))["vectors"]}
    V = {name: v["hex"] for name, v in vectors.items()}
    run_started = time.monotonic()

    # ---------------------------------------------------------------- Transport
    report.section("Transport: advertising and connection")
    device, adv, took = await Jacket.find(args.scan_timeout, args.address)
    if not report.check("jacket found by scanning for its service UUID", device is not None,
                        f"{took:.1f}s" if device else f"nothing in {args.scan_timeout:.0f}s"):
        return
    report.note(f"address {device.address}, RSSI {adv.rssi} dBm, found after {took:.1f}s")
    report.equal("device name is 'Jacket'", "Jacket", adv.local_name)
    try:  # WinRT keeps the advertisement and the scan response apart
        raw = adv.platform_data[1]
        adv_uuids = [str(u).lower() for u in raw.adv.advertisement.service_uuids]
        report.check("service UUID is in the advertisement itself", SERVICE in adv_uuids)
        report.equal("advertisement itself carries no name", "", raw.adv.advertisement.local_name or "")
        if raw.scan is not None:
            report.equal("name is in the scan response", "Jacket", raw.scan.advertisement.local_name)
        else:
            report.skip("name is in the scan response", "no scan response event seen")
    except Exception as error:
        report.skip("advertisement / scan response layout", f"backend gives no raw events ({type(error).__name__})")

    await jacket.connect(device)
    client = jacket.client
    report.check("connected and subscribed to `state`", client.is_connected)
    mtu = client.mtu_size
    report.check("negotiated MTU is at least 185", mtu >= 185, f"MTU {mtu}")
    report.equal("negotiated MTU is the jacket's preferred 247", 247, mtu)

    service = client.services.get_service(SERVICE)
    report.check("service is present", service is not None)
    props = {c.uuid.lower(): set(c.properties) for c in service.characteristics}
    report.equal("exactly three characteristics", {COMMAND, STATE, LIVE}, set(props))
    report.equal("`command` is write + write-without-response", {"write", "write-without-response"}, props.get(COMMAND))
    report.equal("`state` is read + notify", {"read", "notify"}, props.get(STATE))
    report.equal("`live` is write-without-response", {"write-without-response"}, props.get(LIVE))

    original = await jacket.read_state()
    report.equal("reading `state` returns a STATE frame with proto 1", 1, original.proto)
    report.check("STATE is well formed", original.power in (0, 1) and 400 <= original.budget_ma <= 2000
                 and (original.preset == 0xFF or original.preset < 8), hexs(original.raw))
    report.note(f"state found: rev {original.rev}, power {original.power}, brightness {original.brightness}, "
                f"budget {original.budget_ma} mA, flags {original.flags:#04x}, preset {original.preset:#04x}, "
                f"{len(original.segments)} segment(s)")

    # ---------------------------------------------------------------- GET_INFO
    report.section("GET_INFO")
    frame = await jacket.ask(V["get_info"], N_INFO)
    if report.check("GET_INFO answers with INFO", frame is not None):
        info = decode_info(frame)
        golden = vectors["info"]["fields"]
        report.equal("proto", 1, info["proto"])
        report.equal("firmware version 2.0.0", (2, 0, 0), info["fw"])
        report.equal("two strips of 144 LEDs", [144, 144], info["leds"])
        report.equal("max_segments", 8, info["max_segments"])
        report.equal("preset_slots", 8, info["preset_slots"])
        report.equal("effect_count", len(EFFECTS), info["effect_count"])
        report.equal("palette_count", 6, info["palette_count"])
        report.check("build string present", 0 < len(info["build"]) <= 32, info["build"])
        golden_frame = bytes.fromhex(V["info"])
        fixed = len(golden_frame) - 1 - len(golden["build"])  # everything before the build string
        report.equal("INFO equals the golden vector up to the build string", hexs(golden_frame[:fixed]), hexs(frame[:fixed]))
        report.note(f"INFO build string: \"{info['build']}\"")
    after = await jacket.read_state()
    report.equal("GET_INFO does not change rev", original.rev, after.rev)

    # ---------------------------------------------------------------- Catalogues
    report.section("GET_CATALOG: effects and palettes, with paging")
    for kind, label, table in ((0, "effects", EFFECTS), (1, "palettes", PALETTES)):
        entries, start, pages, ok = [], 0, 0, True
        while True:
            frame = await jacket.ask(bytes([0x21, kind, start]), N_CATALOG)
            if frame is None:
                ok = False
                break
            page = decode_catalog(frame)
            pages += 1
            ok = ok and len(frame) <= 180 and page["kind"] == kind and page["start"] == start
            entries += page["entries"]
            if len(entries) >= page["total"] or not page["entries"]:
                break
            start += len(page["entries"])
        report.check(f"{label}: paged through the catalogue as PROTOCOL.md describes", ok and pages >= 1,
                     f"{len(entries)} entries in {pages} page(s)")
        report.equal(f"{label}: ids, names and flags match the catalogue table", [tuple(e) for e in table], entries)
        report.equal(f"{label}: total", len(table), page["total"] if frame else None)

        # A page from the middle, the end, and past the end.
        middle = len(table) - 2
        frame = await jacket.ask(bytes([0x21, kind, middle]), N_CATALOG)
        page = decode_catalog(frame) if frame else None
        report.check(f"{label}: start={middle} returns the last two entries",
                     page is not None and page["start"] == middle and [e[0] for e in page["entries"]] == [middle, middle + 1])
        for past in (len(table), 200):
            frame = await jacket.ask(bytes([0x21, kind, past]), N_CATALOG)
            report.equal(f"{label}: start={past} (>= total) returns an empty page",
                         hexs(bytes([N_CATALOG, kind, past, len(table), 0])), hexs(frame) if frame else None)
    # Golden vectors: the device's first page must start with the same entries.
    frame = await jacket.ask(V["get_catalog_effects"], N_CATALOG)
    golden = bytes.fromhex(V["catalog_effects_page"])
    report.check("effects page begins like the golden vector (header apart from count, then Solid, Comet)",
                 frame is not None and frame[:4] == golden[:4] and frame[5:len(golden)] == golden[5:])
    frame = await jacket.ask("210100", N_CATALOG)
    golden = bytes.fromhex(V["catalog_palettes_page"])
    report.check("palettes page begins like the golden vector (total 6, then Colours)",
                 frame is not None and frame[:4] == golden[:4] and frame[5:len(golden)] == golden[5:])
    after = await jacket.read_state()
    report.equal("GET_CATALOG does not change rev", original.rev, after.rev)

    # ---------------------------------------------------------------- Power, brightness
    report.section("SET_POWER and SET_BRIGHTNESS")
    timed_command = jacket.command
    base = await jacket.settle()
    s = await timed_command("0100")
    report.check("SET_POWER 0: STATE notification with power 0 and rev + 1", s is not None and s.power == 0)
    s = await timed_command(V["set_power_on"])
    report.check("SET_POWER 1 (golden vector): power 1, rev + 1", s is not None and s.power == 1 and s.rev == (base.rev + 2) & 0xFFFF)
    s = await timed_command(V["set_brightness_128"])
    report.check("SET_BRIGHTNESS 128 (golden vector)", s is not None and s.brightness == 128)
    for value in (0, 255, 37):
        s = await timed_command(bytes([0x02, value]))
        report.check(f"SET_BRIGHTNESS {value}", s is not None and s.brightness == value)
    state_read = await jacket.read_state()
    report.equal("a read of `state` returns the same frame as the last notification", hexs(s.raw) if s else None, hexs(state_read.raw))
    # How soon does a read show a write that was just acknowledged? (The jacket
    # applies commands on its loop task, a moment after the Bluetooth stack acks.)
    fresh = 0
    for i in range(20):
        await jacket.write(bytes([0x02, 100 + i]))
        if (await jacket.read_state()).brightness == 100 + i:
            fresh += 1
    report.check("a read straight after an acknowledged write already shows the new value", fresh == 20, f"{fresh} of 20")

    # ---------------------------------------------------------------- Segments, effects, parameters
    report.section("SET_SEGMENTS, SET_EFFECT, SET_PARAM")
    s = await timed_command("050103009000")  # one segment, both strips, all 144
    report.check("SET_SEGMENTS: one segment over both strips",
                 s is not None and len(s.segments) == 1 and s.segments[0].geometry == bytes.fromhex("03009000"))
    s = await timed_command(V["set_effect_all_segments"])
    golden_config = bytes.fromhex(V["set_effect_all_segments"])[2:]
    report.check("SET_EFFECT segment 0xFF (golden vector): Comet on every segment",
                 s is not None and all(seg.config == golden_config for seg in s.segments))
    s = await timed_command(V["set_segments_left_right"])
    report.check("SET_SEGMENTS (golden vector): left and right, right reversed",
                 s is not None and [seg.geometry for seg in s.segments] == [bytes.fromhex("01009000"), bytes.fromhex("02009001")])
    report.check("segment 0 keeps its EffectConfig, the new segment copies segment 0",
                 s is not None and [seg.config for seg in s.segments] == [golden_config, golden_config])

    s = await timed_command("0301" "0403" "2060" "102030" "405060")  # segment 1: Breathe, palette 3
    report.check("SET_EFFECT on one segment changes only that segment",
                 s is not None and s.segments[0].config == golden_config
                 and s.segments[1].config == bytes.fromhex("04032060102030405060"))

    s = await timed_command(V["set_param_speed"])
    report.check("SET_PARAM 3 speed (golden vector): segment 0 speed 200", s is not None and s.segments[0].config[2] == 200
                 and s.segments[1].config[2] == 0x20)
    s = await timed_command(V["set_param_color1"])
    report.check("SET_PARAM 5 color1 (golden vector): segment 1 colour ff0080",
                 s is not None and s.segments[1].config[4:7] == bytes.fromhex("ff0080") and s.segments[0].config[4:7] == golden_config[4:7])
    s = await timed_command("040001" "02")
    report.check("SET_PARAM 1 effect: segment 0 becomes Rainbow", s is not None and s.segments[0].config[0] == 2
                 and s.segments[1].config[0] == 4)
    s = await timed_command("040102" "05")
    report.check("SET_PARAM 2 palette: segment 1 palette 5", s is not None and s.segments[1].config[1] == 5)
    s = await timed_command("040004" "11")
    report.check("SET_PARAM 4 intensity: segment 0 intensity 17", s is not None and s.segments[0].config[3] == 0x11)
    s = await timed_command("040006" "0a0b0c")
    report.check("SET_PARAM 6 color2: segment 0", s is not None and s.segments[0].config[7:10] == bytes.fromhex("0a0b0c"))
    s = await timed_command("04ff03" "40")
    report.check("SET_PARAM with segment 0xFF: every segment", s is not None and [seg.config[2] for seg in s.segments] == [0x40, 0x40])
    s = await timed_command("0101" "ffee")
    report.check("trailing bytes are ignored (SET_POWER with two extra bytes)", s is not None and s.power == 1)

    # ---------------------------------------------------------------- IDENTIFY
    report.section("IDENTIFY")
    before = await jacket.settle()
    since = jacket.mark()
    await jacket.write(V["identify_range"])
    got = await jacket.quiet(since, 0.6)
    report.equal("IDENTIFY (golden vector) is accepted silently: no ERROR, no STATE", [], [hexs(f) for f in got])
    since = jacket.mark()
    await jacket.write("0603000000")
    got = await jacket.quiet(since, 0.4)
    report.equal("IDENTIFY cancel (length 0) is accepted silently", [], [hexs(f) for f in got])
    after = await jacket.read_state()
    report.equal("IDENTIFY does not change rev", before.rev, after.rev)
    await jacket.write("0100")
    since = jacket.mark()
    await jacket.write("06010005" "01")
    got = await jacket.quiet(since, 0.4)
    report.check("IDENTIFY is accepted while power is off", not any(f[0] == N_ERROR for f in got))
    await jacket.write("0601000000")
    await jacket.write("0101")
    for label, hex_frame in (("length 0, strip_mask 0", "0600ff0005"), ("seconds 0, everything else out of range", "06ffffff00")):
        await jacket.settle()  # let the STATE of the power commands above arrive first
        since = jacket.mark()
        await jacket.write(hex_frame)
        got = await jacket.quiet(since, 0.4)
        report.equal(f"an IDENTIFY cancel is never rejected ({label})", [], [hexs(f) for f in got])
    await jacket.expect_error("code 3: IDENTIFY start + length past the end of the strip", "06030a8705", 3)
    await jacket.expect_error("code 3: IDENTIFY strip_mask 0 (not a cancel)", "06000a0a05", 3)

    # The app drags a zone edge: IDENTIFY write-without-response, about 30 per second.
    before = await jacket.settle()
    beat_before = (await next_heartbeats(serial, 1))[0] if serial is not None else None
    count = 90
    since = jacket.mark()
    t0 = time.monotonic()
    for i, delay in paced(count, 30.0):
        await asyncio.sleep(delay)
        await jacket.write(bytes([0x06, 0x03, 10, 1 + i, 5]), response=False)  # the range grows from 1 to 90 pixels
    t1 = time.monotonic()
    got = await jacket.quiet(since, 0.5)
    report.check(f"a burst of {count} IDENTIFY writes at 30 per second is sent", count / (t1 - t0) > 18, f"{count / (t1 - t0):.1f}/s")
    report.equal("the burst causes no ERROR and no STATE notification", [], [hexs(f) for f in got])
    t0 = time.monotonic()
    frame = await jacket.ask(V["get_info"], N_INFO)
    report.check("the jacket still answers at once after the burst", frame is not None and time.monotonic() - t0 < 0.5,
                 f"INFO after {(time.monotonic() - t0) * 1000:.0f} ms")
    await jacket.write("0603000000")
    after = await jacket.settle()
    report.equal("the burst does not change rev or anything else in STATE", hexs(before.raw), hexs(after.raw))
    if serial is None:
        report.skip("every IDENTIFY of the burst reached the engine", "needs --port (the heartbeat's command counter)")
    else:
        beat_after = (await next_heartbeats(serial, 1))[0]
        handled = int(beat_after["cmds"]) - int(beat_before["cmds"])
        report.equal("every IDENTIFY of the burst reached the engine (heartbeat command counter: burst + GET_INFO + cancel)",
                     count + 2, handled)
        report.check("nothing was dropped", beat_after["dropped_cmds"] == "0" and beat_after["dropped_replies"] == "0")

    # ---------------------------------------------------------------- Budget
    report.section("SET_BUDGET")
    s = await timed_command(V["set_budget_900"])
    report.check("SET_BUDGET 900 (golden vector)", s is not None and s.budget_ma == 900)
    for sent, expect in ((100, 400), (0, 400), (300, 400), (399, 400), (5000, 2000), (65535, 2000), (400, 400), (2000, 2000),
                         (1200, 1200)):
        s = await timed_command(bytes([0x30, sent & 0xFF, sent >> 8]))
        report.check(f"SET_BUDGET {sent} -> {expect} mA" + (" (clamped)" if sent != expect else ""),
                     s is not None and s.budget_ma == expect, f"got {s.budget_ma if s else None}")
    await jacket.write(V["set_budget_900"])

    # ---------------------------------------------------------------- Dev mode
    report.section("SET_DEV_MODE")
    s = await timed_command(V["set_dev_mode_on"])
    report.check("SET_DEV_MODE 1 (golden vector): flag bit 0 set", s is not None and s.flags & FLAG_DEV)
    # With one default segment, brightness 128, budget 900 and dev mode on, STATE is the golden frame.
    await jacket.write("050103009000")
    await jacket.write("0300" "06008080" "00ffcc" "ff00ff")
    await jacket.write("0280")
    await asyncio.sleep(0.3)
    s = await jacket.read_state()
    golden = bytearray.fromhex(V["state_one_segment"])
    seen = bytearray(s.raw)
    seen[2:4] = golden[2:4]            # rev differs by design
    seen[8] &= FLAG_DEV | FLAG_LIVE    # the WiFi bit depends on the build and the network
    report.equal("STATE equals the golden vector byte for byte (rev aside)", hexs(golden), hexs(seen))
    s = await timed_command("3100")
    report.check("SET_DEV_MODE 0: flag bit 0 clear", s is not None and not s.flags & FLAG_DEV)

    # ---------------------------------------------------------------- Presets
    report.section("Presets")
    frame = await jacket.ask(V["get_presets"], N_PRESETS)
    presets_before = decode_presets(frame) if frame else None
    report.check("GET_PRESETS answers with PRESETS", presets_before is not None, str(presets_before))
    slot2_used = presets_before is not None and any(slot == 2 for slot, _ in presets_before)
    empty_slot = next((n for n in range(7, -1, -1) if presets_before is not None and n != 2
                       and all(slot != n for slot, _ in presets_before)), None)
    if presets_before is None or (slot2_used and not args.overwrite_presets):
        report.skip("preset save / load / delete", "slot 2 is in use on this jacket (pass --overwrite-presets to use it anyway)")
    else:
        others = [p for p in presets_before if p[0] != 2]
        scene_before = (await jacket.read_state()).scene()
        since = jacket.mark()
        s = await jacket.command(V["preset_save"])
        report.check("PRESET_SAVE (golden vector): STATE with preset = 2", s is not None and s.preset == 2)
        frame = await jacket.wait(lambda f: f[0] == N_PRESETS, since)
        saved_list = sorted(others + [(2, "Neon")])
        report.equal("PRESET_SAVE also answers with PRESETS carrying the updated list", saved_list,
                     decode_presets(frame) if frame else None)
        if not others:
            report.equal("that PRESETS frame is the golden vector", V["presets_one"], hexs(frame) if frame else None)
        frame = await jacket.ask(V["get_presets"], N_PRESETS)
        report.equal("GET_PRESETS lists slot 2 'Neon'", saved_list, decode_presets(frame) if frame else None)

        for label, hex_frame in (("SET_POWER", "0100"), ("SET_POWER", "0101"), ("SET_BUDGET", "30b004"),
                                 ("SET_DEV_MODE", "3101"), ("SET_DEV_MODE", "3100"), ("SET_BRIGHTNESS to the same value", "0280"),
                                 ("SET_SEGMENTS with the same layout", "050103009000"),
                                 ("SET_EFFECT with the same EffectConfig", "0300" "06008080" "00ffcc" "ff00ff"),
                                 ("SET_PARAM with the same value", "040003" "80")):
            s = await jacket.command(hex_frame)
            report.check(f"{label} does not clear the preset marker (rev + 1, STATE notified)", s is not None and s.preset == 2)
        await jacket.write(V["set_budget_900"])

        s = await jacket.command("0240")
        report.check("changing brightness clears the marker to 0xFF", s is not None and s.preset == 0xFF)
        await jacket.write("0300" "00008080" "ff0000" "0000ff")
        await jacket.write(V["set_segments_left_right"])
        s = await jacket.command(V["preset_load"])
        report.check("PRESET_LOAD (golden vector): preset = 2 and the saved scene is back",
                     s is not None and s.preset == 2 and s.scene() == scene_before)
        s = await jacket.command("040003" "99")
        report.check("changing an EffectConfig clears the marker", s is not None and s.preset == 0xFF)
        await jacket.command(V["preset_load"])
        s = await jacket.command(V["set_segments_left_right"])
        report.check("changing the segments clears the marker", s is not None and s.preset == 0xFF)
        await jacket.write("0100")
        s = await jacket.command(V["preset_load"])
        report.check("power is not stored in a preset (loading leaves it off)", s is not None and s.power == 0 and s.preset == 2)
        await jacket.write("0101")

        since = jacket.mark()
        s = await jacket.command(V["preset_delete"])
        report.check("PRESET_DELETE (golden vector) of the marked slot: preset = 0xFF", s is not None and s.preset == 0xFF)
        frame = await jacket.wait(lambda f: f[0] == N_PRESETS, since)
        report.equal("PRESET_DELETE also answers with PRESETS carrying the updated list", sorted(others),
                     decode_presets(frame) if frame else None)
        await jacket.expect_error("PRESET_LOAD of the now empty slot: ERROR code 4", V["preset_load"], 4)
        await jacket.expect_error("PRESET_DELETE of an empty slot: ERROR code 4", V["preset_delete"], 4)

        if empty_slot is not None:
            name16 = "Sixteen bytes!!!".encode()
            since = jacket.mark()
            await jacket.write(bytes([0x10, empty_slot, 16]) + name16)
            frame = await jacket.wait(lambda f: f[0] == N_PRESETS, since)
            listed = dict(decode_presets(frame)) if frame else {}
            report.equal(f"a 16-byte name is accepted (slot {empty_slot})", name16.decode(), listed.get(empty_slot))
            # Deleting a slot that is not the marked one changes nothing visible, and still counts.
            s = await jacket.command(V["preset_save"])
            report.check("saving slot 2 again moves the marker to it", s is not None and s.preset == 2)
            since = jacket.mark()
            s = await jacket.command(bytes([0x12, empty_slot]))
            report.check(f"PRESET_DELETE of slot {empty_slot}, not the marked one: rev + 1, STATE notified, marker stays 2",
                         s is not None and s.preset == 2)
            frame = await jacket.wait(lambda f: f[0] == N_PRESETS, since)
            report.equal("... and PRESETS no longer lists it", saved_list, decode_presets(frame) if frame else None)
            since = jacket.mark()
            await jacket.write(V["preset_delete"])
            await jacket.wait(lambda f: f[0] == N_PRESETS, since)

    # ---------------------------------------------------------------- Errors
    report.section("ERROR codes and validation")
    before = await jacket.settle()
    await jacket.expect_error("code 1: unknown opcode 0x42 gives the golden ERROR frame", "42", 1)
    since = jacket.mark()
    await jacket.write("42")
    frame = await jacket.wait(lambda f: f[0] == N_ERROR, since)
    report.equal("... byte for byte", V["error_unknown_opcode"], hexs(frame) if frame else None)
    await jacket.expect_error("code 1: opcode 0x00", "00", 1)
    await jacket.expect_error("code 2: SET_EFFECT cut short", "03ff0100", 2)
    await jacket.expect_error("code 2: SET_POWER without a value", "01", 2)
    await jacket.expect_error("code 2: SET_BUDGET with one byte", "3084", 2)
    # BOOT_OTHER_SLOT is only ever sent here in forms the jacket must refuse: the
    # real command (32 5a) would restart it into another firmware. See boot_other_slot.py.
    await jacket.expect_error("code 2: BOOT_OTHER_SLOT without its confirmation byte", "32", 2)
    await jacket.expect_error("code 3: BOOT_OTHER_SLOT with a wrong confirmation byte", "3200", 3)
    await jacket.expect_error("code 3: ... and with 0xA5", "32a5", 3)
    await jacket.expect_error("code 2 wins over a bad value: SET_EFFECT, bad segment, one byte short", "0309630080c800ffccff00", 2)
    await jacket.expect_error("code 2 wins: PRESET_SAVE slot 9, name cut short", "1009044e65", 2)
    await jacket.expect_error("code 3 where the length is unknowable: unknown SET_PARAM id", "040007", 3)
    await jacket.expect_error("code 3 where the length is unknowable: segment count 9", "0509", 3)
    await jacket.expect_error("code 3 where the length is unknowable: name length 17", "100011", 3)
    try:
        await jacket.expect_error("empty write: ERROR(opcode 0, code 2)", b"", 2, opcode=0)
    except Exception as error:
        report.skip("empty write: ERROR(opcode 0, code 2)", f"this Bluetooth stack will not send an empty write ({type(error).__name__})")
    await jacket.expect_error("code 3: SET_POWER 2", "0102", 3)
    await jacket.expect_error("code 3: SET_DEV_MODE 2", "3102", 3)
    await jacket.expect_error(f"code 3: unknown effect {len(EFFECTS)}",
                              bytes([0x03, 0x00, len(EFFECTS)]) + bytes.fromhex("008080" "000000" "0000ff"), 3)
    await jacket.expect_error(f"code 3: unknown palette {len(PALETTES)}",
                              bytes([0x03, 0x00, 0x00, len(PALETTES)]) + bytes.fromhex("8080" "000000" "0000ff"), 3)
    await jacket.expect_error("code 3: SET_PARAM effect 99", "04000163", 3)
    await jacket.expect_error("code 3: SET_PARAM palette 99", "04000263", 3)
    count = len(before.segments)
    await jacket.expect_error(f"code 3: SET_EFFECT segment {count} (count is {count})",
                              bytes([0x03, count]) + bytes.fromhex("00008080000000" "0000ff"), 3)
    await jacket.expect_error(f"code 3: SET_PARAM segment {count}", bytes([0x04, count, 0x03, 0x80]), 3)
    await jacket.expect_error("code 3: preset slot 8 (save)", "100800", 3)
    await jacket.expect_error("code 3: preset slot 8 (load)", "1108", 3)
    await jacket.expect_error("code 3: preset slot 255 (delete)", "12ff", 3)
    await jacket.expect_error("code 3: preset name of 17 bytes", "100011" + "41" * 17, 3)
    await jacket.expect_error("code 3: SET_SEGMENTS strip_mask 0", "050100009000", 3)
    await jacket.expect_error("code 3: SET_SEGMENTS strip_mask 4", "050104009000", 3)
    await jacket.expect_error("code 3: SET_SEGMENTS unknown flag bit", "050103009002", 3)
    await jacket.expect_error("code 3: SET_SEGMENTS count 0", "0500", 3)
    await jacket.expect_error("code 3: SET_SEGMENTS start + length > 144", "050103019000", 3)
    await jacket.expect_error("code 3: IDENTIFY strip_mask 0", "06000a0a05", 3)
    await jacket.expect_error("code 3: GET_CATALOG kind 2", "210200", 3)
    if empty_slot is not None:
        await jacket.expect_error(f"code 4: PRESET_LOAD on empty slot {empty_slot}", bytes([0x11, empty_slot]), 4)
    else:
        report.skip("code 4: PRESET_LOAD on an empty slot", "no empty slot")
    report.skip("code 5: storage failure", "cannot be provoked on working hardware; covered by the host tests")
    after = await jacket.settle()
    report.equal("rejected commands change nothing (STATE identical, rev unchanged)", hexs(before.raw), hexs(after.raw))
    try:
        await jacket.write(bytes([0x02]) + bytes(180))
        report.check("a write longer than 180 bytes is refused by the jacket", False, "it was accepted")
    except Exception as error:
        report.check("a write longer than 180 bytes is refused by the jacket", True, type(error).__name__)

    # ---------------------------------------------------------------- Pipelined commands
    report.section("Pipelined commands: every reply arrives")
    # Forty GET_CATALOG written without waiting for anything, more than the
    # jacket's reply queue holds at once. Each must be answered, in order.
    pipelined = 40
    since = jacket.mark()
    for i in range(pipelined):
        await jacket.write(bytes([0x21, i % 2, 0]), response=False)
    deadline = time.monotonic() + 6.0
    while time.monotonic() < deadline and sum(1 for _, f in jacket.notes[since:] if f[0] == N_CATALOG) < pipelined:
        await asyncio.sleep(0.05)
    pages = [f for _, f in jacket.notes[since:] if f[0] == N_CATALOG]
    report.equal(f"{pipelined} GET_CATALOG sent back to back: {pipelined} CATALOG replies", pipelined, len(pages))
    report.equal("the replies come in the order asked (effects, palettes, effects, ...)",
                 [i % 2 for i in range(len(pages))], [f[1] for f in pages])
    report.check("every reply is complete", all(len(decode_catalog(f)["entries"]) == (len(EFFECTS), len(PALETTES))[f[1]] for f in pages))
    # The same with commands that change something and write flash: PRESET_SAVE
    # and PRESET_DELETE each answer with a PRESETS frame.
    if presets_before is not None and not (slot2_used and not args.overwrite_presets):
        since = jacket.mark()
        rounds = 6
        for _ in range(rounds):
            await jacket.write(V["preset_save"], response=False)
            await jacket.write(V["preset_delete"], response=False)
        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline and sum(1 for _, f in jacket.notes[since:] if f[0] == N_PRESETS) < 2 * rounds:
            await asyncio.sleep(0.05)
        lists = [f for _, f in jacket.notes[since:] if f[0] == N_PRESETS]
        errors = [hexs(f) for _, f in jacket.notes[since:] if f[0] == N_ERROR]
        report.equal(f"{rounds} PRESET_SAVE / PRESET_DELETE pairs back to back: {2 * rounds} PRESETS replies, no ERROR",
                     (2 * rounds, []), (len(lists), errors))
        frame = await jacket.ask(V["get_presets"], N_PRESETS)
        report.equal("and the preset list is as before", presets_before, decode_presets(frame) if frame else None)
    await jacket.settle()

    # ---------------------------------------------------------------- rev
    report.section("rev")
    before = await jacket.settle()
    await jacket.write(bytes([0x02, before.brightness]))
    await jacket.write(bytes([0x02, before.brightness]))
    await jacket.write(bytes([0x01, before.power]))
    await jacket.write(bytes([0x30, before.budget_ma & 0xFF, before.budget_ma >> 8]))
    await asyncio.sleep(0.4)
    after = await jacket.read_state()
    report.equal("rev increments on every accepted command even when the value is unchanged (4 commands)",
                 (before.rev + 4) & 0xFFFF, after.rev)
    report.equal("... and nothing else changed", hexs(before.raw[4:]), hexs(after.raw[4:]))
    await jacket.write(V["get_presets"])
    await jacket.write(V["get_info"])
    await jacket.write(V["get_catalog_effects"])
    await jacket.write(V["identify_range"])
    await jacket.write("0603000000")
    await asyncio.sleep(0.4)
    report.equal("GET_* and IDENTIFY do not change rev", after.rev, (await jacket.read_state()).rev)

    # ---------------------------------------------------------------- Burst
    report.section("Burst of write-without-response (a dragged slider)")
    before = await jacket.settle()
    count, rate = 120, 30.0
    since = jacket.mark()
    t0 = time.monotonic()
    for i, delay in paced(count, rate):
        await asyncio.sleep(delay)
        value = 10 + (i * 7) % 200 if i < count - 1 else 77
        await jacket.write(bytes([0x02, value]), response=False)
    t1 = time.monotonic()
    final = await jacket.wait(lambda f: f[0] == N_STATE and decode_state(f).rev == (before.rev + count) & 0xFFFF, since, 3.0)
    await asyncio.sleep(0.5)
    after = await jacket.read_state()
    achieved = count / (t1 - t0)
    report.check(f"sent {count} writes at about {rate:.0f} per second", achieved > rate * 0.6, f"{achieved:.1f}/s over {t1 - t0:.1f}s")
    report.equal("nothing lost: rev advanced by exactly the number of writes", (before.rev + count) & 0xFFFF, after.rev)
    report.equal("the last value won", 77, after.brightness)
    report.check("the final value was announced by a STATE notification", final is not None and decode_state(final).brightness == 77)
    times = [t for t, f in jacket.notes[since:] if f[0] == N_STATE and t <= t1 + 0.5]
    window = max((sum(1 for u in times if t <= u < t + 1.0) for t in times), default=0)
    overall = len(times) / (t1 - t0 + 0.5)
    report.check("STATE notifications stay at about 10 per second", window <= 12 and overall <= 10.5,
                 f"{len(times)} notifications, {overall:.1f}/s on average, at most {window} in any one second")
    revs = [decode_state(f).rev for _, f in jacket.notes[since:] if f[0] == N_STATE]
    steps = [(b - a) & 0xFFFF for a, b in zip(revs, revs[1:])]
    report.check("notifications arrive in order (rev only moves forward)", all(0 < step < 1000 for step in steps))
    if len(times) > 2:
        gaps = [b - a for a, b in zip(times, times[1:])]
        report.note(f"burst: {achieved:.1f} writes/s achieved; STATE notifications {len(times)} in {t1 - t0:.1f}s, "
                    f"gap median {statistics.median(gaps) * 1000:.0f} ms, min {min(gaps) * 1000:.0f} ms")

    # ---------------------------------------------------------------- Live input
    report.section("Live input")
    before = await jacket.settle()
    report.check("live flag is clear to begin with", not before.flags & FLAG_LIVE)
    live = bytes.fromhex(V["live_frame"])
    since = jacket.mark()
    await jacket.write_live(live[:13])
    got = await jacket.quiet(since, 0.5)
    report.equal("a 13-byte live frame is dropped silently", [], [hexs(f) for f in got])
    report.check("... and does not set the flag", not (await jacket.read_state()).flags & FLAG_LIVE)

    since = jacket.mark()
    last_sent = 0.0
    for i, delay in paced(45, 30.0):  # 1.5 s at 30 frames per second
        await asyncio.sleep(delay)
        await jacket.write_live(live[:1] + bytes([i & 0xFF]) + live[2:])
        last_sent = time.monotonic()
    on = await jacket.wait(lambda f: f[0] == N_STATE and decode_state(f).flags & FLAG_LIVE, since, 0.5)
    report.check("live frames (golden vector) set flag bit 2", on is not None)
    off = await jacket.wait(lambda f: f[0] == N_STATE and not decode_state(f).flags & FLAG_LIVE, since, 3.0)
    off_time = next((t for t, f in jacket.notes[since:] if f[0] == N_STATE and not decode_state(f).flags & FLAG_LIVE), None)
    delay_ms = (off_time - last_sent) * 1000 if off_time else None
    report.check("the flag clears about 500 ms after the last frame", off is not None and 350 <= delay_ms <= 1000,
                 f"{delay_ms:.0f} ms after the last write" if delay_ms else "never cleared")
    states = [decode_state(f) for _, f in jacket.notes[since:] if f[0] == N_STATE]
    report.equal("exactly two STATE notifications: flag set, flag cleared", 2, len(states))
    after = await jacket.read_state()
    report.equal("rev advanced by 2 (set and clear), not once per frame", (before.rev + 2) & 0xFFFF, after.rev)
    since = jacket.mark()
    await jacket.write_live(live + b"\x01\x02\x03")
    on = await jacket.wait(lambda f: f[0] == N_STATE and decode_state(f).flags & FLAG_LIVE, since, 1.0)
    report.check("a live frame with trailing bytes still counts", on is not None)
    await jacket.wait(lambda f: f[0] == N_STATE and not decode_state(f).flags & FLAG_LIVE, since, 3.0)

    # ---------------------------------------------------------------- Reconnect
    report.section("Disconnect and reconnect")
    hidden_device, _, _ = await Jacket.find(4.0, device.address)
    report.check("the jacket does not advertise while a central is connected", hidden_device is None)
    before = await jacket.settle()
    await jacket.disconnect()
    device2, adv2, took = await Jacket.find(args.scan_timeout, device.address)
    if report.check("advertises again after the disconnect", device2 is not None, f"found after {took:.1f}s"):
        # The whole sequence PROTOCOL.md gives the app: (MTU, subscribe,) read state,
        # GET_INFO, both catalogues page by page, GET_PRESETS.
        t_connect = time.monotonic()
        await jacket.connect(device2)
        t_linked = time.monotonic()
        after = await jacket.read_state()
        complete = await jacket.ask(V["get_info"], N_INFO) is not None
        for kind, total in ((0, len(EFFECTS)), (1, len(PALETTES))):
            start = 0
            while complete and start < total:
                frame = await jacket.ask(bytes([0x21, kind, start]), N_CATALOG)
                entries = decode_catalog(frame)["entries"] if frame else []
                complete = bool(entries)
                start += len(entries)
        frame = await jacket.ask(V["get_presets"], N_PRESETS) if complete else None
        t_done = time.monotonic()
        report.check("the app's connect sequence completes, ending with GET_PRESETS", frame is not None,
                     f"link and subscription {t_linked - t_connect:.1f}s, then state + info + catalogues + presets "
                     f"{(t_done - t_linked) * 1000:.0f} ms")
        report.equal("state is unchanged by the reconnect (rev carries on)", hexs(before.raw), hexs(after.raw))
        s = await jacket.command("0101")
        report.check("commands and notifications work on the new connection", s is not None)
    else:
        return

    # ---------------------------------------------------------------- Render loop
    report.section("Render loop, from the serial heartbeat")
    if serial is None:
        report.skip("frame rate when animating and when static", "needs --port to read the heartbeat")
    else:
        await jacket.write("050103009000")
        await jacket.write("0300" "01008080" "00ffcc" "ff00ff")  # Comet: animates
        await jacket.write("0101")
        beats = await next_heartbeats(serial, 2)
        if report.check("heartbeat seen while animating", len(beats) == 2):
            beat = beats[1]  # the first one may straddle the change
            report.check("animating: 60 frames per second", 59.0 <= float(beat["fps"]) <= 61.0, f"fps {beat['fps']}")
            report.check("animating: a frame (render + send) fits easily in the 16.7 ms slot", int(beat["frame_max"]) < 10000,
                         f"average {int(beat['frame_avg']) / 1000:.2f} ms, worst {int(beat['frame_max']) / 1000:.2f} ms")
        await jacket.write("0100")  # power off: one black frame, then only the 2 s refresh
        beats = await next_heartbeats(serial, 2)
        if report.check("heartbeat seen while off", len(beats) == 2):
            report.check("power off: only the 2 s refresh (about 0.5 frames per second)", 0.3 <= float(beats[1]["fps"]) <= 0.7,
                         f"fps {beats[1]['fps']}")
        await jacket.write("0300" "00008080" "202020" "000000")  # Solid: a static picture
        await jacket.write("0101")
        beats = await next_heartbeats(serial, 2)
        if report.check("heartbeat seen with a static scene", len(beats) == 2):
            report.check("static scene: only the 2 s refresh", 0.3 <= float(beats[1]["fps"]) <= 0.7, f"fps {beats[1]['fps']}")
        # A command during the idle wait is still answered at once.
        t0 = time.monotonic()
        frame = await jacket.ask(V["get_info"], N_INFO)
        report.check("the idle loop wakes for a command", frame is not None and time.monotonic() - t0 < 0.5,
                     f"INFO after {(time.monotonic() - t0) * 1000:.0f} ms")

    # ---------------------------------------------------------------- Crossfade
    report.section("Crossfade, from the serial heartbeat")
    if serial is None:
        report.skip("the render loop wakes for a fade and idles again", "needs --port to read the heartbeat")
    else:
        # The scene is static (Solid) and the loop idle. Four power changes, each a
        # 350 ms fade at 60 fps, all inside one 5 s heartbeat window: about 85
        # frames instead of 2 or 3; and the window after that is idle again.
        await next_heartbeats(serial, 1)
        for hex_frame in ("0100", "0101", "0100", "0101"):
            s = await jacket.command(hex_frame)
            report.check(f"SET_POWER {hex_frame[2:]} is acknowledged while fading", s is not None and s.power == int(hex_frame[2:]))
            await asyncio.sleep(0.45)
        beats = await next_heartbeats(serial, 2)
        if report.check("heartbeats seen around the fades", len(beats) == 2):
            report.check("four fades in the window: the loop rendered them (far more frames than the idle refresh, far fewer than 60 fps)",
                         8.0 <= float(beats[0]["fps"]) <= 35.0, f"fps {beats[0]['fps']}")
            report.check("after the fades the loop idles again", float(beats[1]["fps"]) <= 0.7, f"fps {beats[1]['fps']}")
            report.check("a faded frame still fits easily in its slot", int(beats[0]["frame_max"]) < 10000,
                         f"average {int(beats[0]['frame_avg']) / 1000:.2f} ms, worst {int(beats[0]['frame_max']) / 1000:.2f} ms")

    # ---------------------------------------------------------------- Reactive effects
    report.section("Reactive effects with a live stream (30 Hz, 120 BPM)")
    await jacket.write("0503" "01009000" "02004800" "02484800")        # left strip; right strip in two halves
    await jacket.write(bytes([0x03, 0x00, BEAT_PULSE]) + bytes.fromhex("008080" "00ffcc" "ff00ff"))
    await jacket.write(bytes([0x03, 0x01, VU_METER]) + bytes.fromhex("038080" "00ffcc" "ff00ff"))
    await jacket.write(bytes([0x03, 0x02, SPARKLE]) + bytes.fromhex("038080" "00ffcc" "ff00ff"))
    await jacket.write("0101")
    state = await jacket.settle()
    report.equal("Beat Pulse, VU Meter and Sparkle are accepted, one per segment",
                 [BEAT_PULSE, VU_METER, SPARKLE], [seg.config[0] for seg in state.segments])
    report.check("live flag is clear before the stream", not state.flags & FLAG_LIVE)
    if serial is not None:
        beats = await next_heartbeats(serial, 2)
        if report.check("heartbeat seen with the reactive effects on their fallback", len(beats) == 2):
            report.check("fallback: they animate at 60 frames per second without any live input",
                         59.0 <= float(beats[1]["fps"]) <= 61.0 and beats[1].get("live") == "off", f"fps {beats[1]['fps']}, live={beats[1].get('live')}")

    # The phone: a frame every 33 ms with all three groups valid, a 120 BPM beat
    # counted on this computer's clock, loudness and movement that swing slowly.
    stream_seconds, sent = 12.0, 0
    since = jacket.mark()
    t_stream = time.monotonic()
    for i, delay in paced(int(stream_seconds * 30), 30.0):
        await asyncio.sleep(delay)
        now = time.monotonic()
        beat = (now - t_stream) * 2.0                      # 120 BPM
        swing = 0.5 + 0.5 * math.sin((now - t_stream) * 1.3)
        frame = bytes([0x07, i & 0xFF, int(60 + 180 * swing), int(255 * (1 - beat % 1.0)), int(120 * swing), int(200 * (1 - swing))])
        frame += (1200).to_bytes(2, "little") + bytes([int((beat % 1.0) * 256) & 0xFF, int(40 + 200 * swing)])
        frame += bytes([int(100 * math.sin(now)) & 0xFF, int(100 * math.cos(now)) & 0xFF])
        frame += (int(now * 1000) & 0xFFFF).to_bytes(2, "little")
        await jacket.write_live(frame)
        sent += 1
    t_end = time.monotonic()
    report.check(f"sent {sent} live frames at about 30 per second", sent / (t_end - t_stream) > 25, f"{sent / (t_end - t_stream):.1f}/s")
    on = await jacket.wait(lambda f: f[0] == N_STATE and decode_state(f).flags & FLAG_LIVE, since, 1.0)
    report.check("STATE announced live input", on is not None)
    off = await jacket.wait(lambda f: f[0] == N_STATE and not decode_state(f).flags & FLAG_LIVE, since, 3.0)
    report.check("and its end, when the stream stopped", off is not None)
    if serial is None:
        report.skip("beat clock lock, frame rate and frame time during the stream", "needs --port to read the heartbeat")
    else:
        # Heartbeats whose whole 5 s window lay inside the stream.
        inside = heartbeats([line for when, line in serial.snapshot() if t_stream + 5.6 <= when <= t_end])
        if report.check("a full heartbeat window fell inside the stream", len(inside) >= 1, f"{len(inside)} heartbeat(s)"):
            beat = inside[-1]
            report.equal("the jacket saw audio, motion and beat data", "amb", beat.get("live"))
            report.check("nearly every frame reached the engine", int(beat.get("live_rx", 0)) >= 120, f"{beat.get('live_rx')} of about 150 in 5 s")
            report.check("the beat clock is locked at 120.0 BPM", beat.get("beat") == "lock" and beat.get("bpm") == "120.0",
                         f"beat={beat.get('bpm')}bpm/{beat.get('beat')}")
            report.check("still 60 frames per second with three reactive effects following the stream",
                         59.0 <= float(beat["fps"]) <= 61.0, f"fps {beat['fps']}")
            report.check("frame time with reactive effects (render + send) fits easily in the 16.7 ms slot", int(beat["frame_max"]) < 10000,
                         f"average {int(beat['frame_avg']) / 1000:.2f} ms, worst {int(beat['frame_max']) / 1000:.2f} ms")
            report.note(f"reactive effects under a live stream: frame average {int(beat['frame_avg']) / 1000:.2f} ms, worst "
                        f"{int(beat['frame_max']) / 1000:.2f} ms, heap {beat['heap']} (lowest ever {beat['min_heap']}), "
                        f"{beat.get('live_rx')} frames in the window, last frame {beat.get('late')} ms later than the best")
        # Afterwards: the beat clock coasts for 3 s, then reports no beat; the effects stay at 60 fps on their fallback.
        later = []
        for _ in range(4):
            later += await next_heartbeats(serial, 1)
            if later and later[-1].get("beat") == "none":
                break
        if later and later[-1].get("snd") == "mic" and later[-1].get("live") == "off":
            # A board with a microphone of its own takes over when the phone stops: if it hears a beat, the
            # clock never reports "no beat". Which it hears depends on the room, so there is nothing to compare.
            report.skip("after the stream the beat clock coasts, then reports no beat",
                        "this board's own microphone supplies the sound now ("
                        + " -> ".join(f"{b.get('live')}/{b.get('beat')}" for b in later) + ")")
        else:
            report.check("after the stream the beat clock coasts, then reports no beat", bool(later) and later[-1].get("beat") == "none"
                         and later[-1].get("live") == "off", " -> ".join(f"{b.get('live')}/{b.get('beat')}" for b in later))
        report.check("and the effects carry on at 60 frames per second", bool(later) and 59.0 <= float(later[-1]["fps"]) <= 61.0,
                     f"fps {later[-1]['fps'] if later else '-'}")

    # ---------------------------------------------------------------- Persistence
    report.section("Persistence across a reset")
    if serial is None:
        report.skip("scene, budget and power after a reset", "needs --port to reset the board")
    else:
        await jacket.write(V["set_segments_left_right"])
        await jacket.write("0300" "01028c50" "ff8000" "0040ff")
        await jacket.write("0301" "0404a03c" "00ff40" "8000ff")
        await jacket.write("0263")       # brightness 99
        await jacket.write("304c04")     # budget 1100 mA
        await jacket.write("0100")       # power off: it must come back on
        await asyncio.sleep(0.3)
        wanted = await jacket.read_state()
        t_changed = time.monotonic()
        saved = await serial.wait_for(r"\[nvs\] saved", t_changed, 9.0)
        waited = time.monotonic() - t_changed
        report.check("the debounced save happens about 5 s after the last change", saved is not None and 4.0 <= waited <= 7.5,
                     f"'[nvs] saved' after {waited:.1f}s" if saved else "no '[nvs] saved' line within 9 s")
        await jacket.write("0211")       # an unsaved change, lost by the reset below
        await asyncio.sleep(0.3)
        t_reset = time.monotonic()
        serial.reset_board()
        banner = await serial.wait_for(r"\[boot\] jacket fw=", t_reset, 8.0)
        report.check("the board restarts and prints its boot banner", banner is not None, banner or "no banner")
        if banner:
            report.check("banner: clean reset, no crash count, not in safe mode",
                         "crashes=0" in banner and "safe_mode=no" in banner and "reset=power-on" in banner)
        await jacket.disconnect()
        await asyncio.sleep(1.0)
        device3, _, took = await Jacket.find(args.scan_timeout, device.address)
        if report.check("advertises again after the reset", device3 is not None, f"found after {took:.1f}s"):
            await jacket.connect(device3)
            s = await jacket.read_state()
            report.equal("rev starts at 0 at boot", 0, s.rev)
            report.equal("power is on after boot although it was off before", 1, s.power)
            report.equal("budget came back", 1100, s.budget_ma)
            report.equal("brightness came back (the saved 99, not the unsaved 17)", 99, s.brightness)
            report.equal("segments and their EffectConfigs came back", wanted.scene()[1], s.scene()[1])
            report.equal("preset marker came back", wanted.preset, s.preset)
        else:
            return

    # ---------------------------------------------------------------- Put things back
    report.section("Restoring the state found at the start")
    await set_scene(jacket, original)
    await jacket.write(bytes([0x30, original.budget_ma & 0xFF, original.budget_ma >> 8]))
    await jacket.write(bytes([0x31, 1 if original.flags & FLAG_DEV else 0]))
    if original.preset != 0xFF:
        await jacket.write(bytes([0x11, original.preset]))
    await jacket.write(bytes([0x01, original.power]))
    await asyncio.sleep(0.4)
    s = await jacket.read_state()
    report.check("scene, budget, power and dev mode are as they were", s.scene() == original.scene()
                 and s.budget_ma == original.budget_ma and s.power == original.power
                 and (s.flags & FLAG_DEV) == (original.flags & FLAG_DEV))
    frame = await jacket.ask(V["get_presets"], N_PRESETS)
    report.equal("preset list is as it was", presets_before, decode_presets(frame) if frame else None)
    if serial is not None:
        t0 = time.monotonic()
        saved = await serial.wait_for(r"\[nvs\] saved", t0 - 0.5, 9.0)
        report.check("the restored state was written to flash", saved is not None)

    if jacket.latencies:
        report.note(f"write-with-response to STATE notification: median {statistics.median(jacket.latencies) * 1000:.0f} ms, "
                    f"max {max(jacket.latencies) * 1000:.0f} ms over {len(jacket.latencies)} commands")
    report.note(f"Bluetooth part took {time.monotonic() - run_started:.0f}s")


def check_serial(report: Report, serial: SerialWatcher, started: float) -> None:
    report.section("Serial log during the run")
    lines = serial.since(started)
    beats = heartbeats(lines)
    boots = [line for line in lines if "[boot] jacket fw=" in line]
    trouble = [line for line in lines if TROUBLE.search(line)]
    report.check("heartbeat kept running", len(beats) >= 5, f"{len(beats)} heartbeats")
    report.equal("no reset other than the one this check asked for", serial.expected_boots, len(boots))
    report.check("no panic, watchdog, brown-out or FAILED line", not trouble, "; ".join(trouble[:3]))
    if not beats:
        return
    heap = [int(b["heap"]) for b in beats]
    min_heap = [int(b["min_heap"]) for b in beats]
    stack = [int(b["stack"]) for b in beats]
    report.check("no command or reply was dropped, and no frame was too long for the link",
                 all(b["dropped_cmds"] == "0" and b["dropped_replies"] == "0" and (b.get("oversize") or "0") == "0" for b in beats))
    # The heap moves a little with connections; a leak shows as a steady decline.
    connected = [int(b["heap"]) for b in beats if b["ble"].startswith("connected")]
    drift = connected[-1] - connected[0] if len(connected) >= 2 else 0
    report.check("free heap does not decline while connected", drift > -2048,
                 f"first {connected[0] if connected else '-'}, last {connected[-1] if connected else '-'} bytes")
    drawn = [b for b in beats if int(b["frame_max"]) > 0]
    if drawn:
        worst = max(int(b["frame_max"]) for b in drawn)
        report.check("no frame took longer than 12 ms during the whole run (render + send)", worst < 12000,
                     f"worst {worst / 1000:.2f} ms, typical average {statistics.median(int(b['frame_avg']) for b in drawn) / 1000:.2f} ms")
    report.check("no frame rate above 60 per second", all(float(b["fps"]) <= 61.0 for b in beats),
                 f"highest {max(float(b['fps']) for b in beats):.1f}")
    report.note(f"heap free {min(heap)}..{max(heap)} bytes, lowest ever (min_heap) {min(min_heap)} bytes, "
                f"loop stack headroom {min(stack)} bytes")
    for line in lines:
        if "[ble] params" in line or "[ble] mtu" in line or "[ble] connected" in line:
            report.note("serial: " + line.strip())
            if "[ble] params" in line:
                break


def main() -> int:
    parser = argparse.ArgumentParser(description="Check a jacket against PROTOCOL.md over Bluetooth LE.")
    parser.add_argument("--port", help="serial port of the jacket (e.g. COM7): watch its log and reset it for the persistence check")
    parser.add_argument("--address", help="Bluetooth address, if more than one jacket is in range")
    parser.add_argument("--scan-timeout", type=float, default=25.0, help="seconds to look for the jacket (default 25)")
    parser.add_argument("--overwrite-presets", action="store_true", help="use preset slot 2 even if it holds something")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    report = Report(args.verbose)
    serial = None
    started = time.monotonic()
    if args.port:
        serial = SerialWatcher(args.port)
        time.sleep(2.5)  # if opening the port reset the board anyway, let it boot
        opening = [line for line in serial.since(started) if "[boot] jacket fw=" in line]
        if opening:
            print("  note  opening the serial port reset the board; waiting for it")
            serial.expected_boots += len(opening)
            time.sleep(2.0)
    try:
        asyncio.run(run(args, report, serial))
    except Exception as error:
        report.check("the check ran to the end", False, f"{type(error).__name__}: {error}")
    finally:
        if serial is not None:
            time.sleep(6.0)  # at least one more heartbeat
            check_serial(report, serial, started)
            serial.close()
    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
