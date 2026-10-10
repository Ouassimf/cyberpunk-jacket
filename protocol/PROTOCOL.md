# Jacket BLE protocol — v1

Single source of truth for the link between the firmware (`firmware/`) and the Android app (`android/`).
Both sides test their codec against `protocol/vectors/v1.json`. If code, vectors and this document disagree, this document wins.

## Transport

| | |
|---|---|
| Device name | `Jacket` (in the scan response) |
| Service | `89960001-beff-438f-8710-5e7d3dff8891` (in the advertisement) |
| `command` | `89960002-beff-438f-8710-5e7d3dff8891` — write, write-without-response |
| `state` | `89960003-beff-438f-8710-5e7d3dff8891` — read, notify |
| `live` | `89960004-beff-438f-8710-5e7d3dff8891` — write-without-response |

- The jacket is the peripheral. One central at a time.
- The app requests an ATT MTU of 247 after connecting. No frame is longer than 180 bytes; the app must not use the link if the negotiated MTU is below 185.
- On connect the app: negotiates MTU, subscribes to `state`, reads `state`, sends `GET_INFO`, pages through `GET_CATALOG` for effects and palettes, then sends `GET_PRESETS`.
- If `proto` in STATE or INFO is not 1, the app disconnects and tells the user the app and jacket do not match.
- The app sends drag previews write-without-response and everything else (final values, taps, `GET_*`) with response.

### Diagnostics

Not part of the protocol an app speaks, and an app need not know it. The jacket also has a second service, `8996d001-beff-438f-8710-5e7d3dff8891`, with one characteristic, `status`, `8996d002-beff-438f-8710-5e7d3dff8891`, read only: a line of text of at most 240 bytes with what the serial heartbeat reports (uptime, last reset, frame rate and time, free memory and, on a board with a microphone, what the microphone task sees), refreshed every 5 s. It reads `dev mode is off` unless dev mode is on. `firmware/tools/read_status.py` prints it. Its wording may change with any firmware.

## Encoding

- Every frame is `[type u8][payload]`. On `command` the type is an opcode; on `state` it is a notification type.
- Integers are little-endian. `u16` is 2 bytes.
- `rgb` is 3 bytes: red, green, blue.
- `str` is `[len u8][UTF-8 bytes]`, no terminator.
- A receiver ignores trailing bytes it does not understand, and replies `ERROR` to frames that are too short.
- Strings are opaque bytes to the jacket; the app decodes them leniently as UTF-8.

### EffectConfig (10 bytes)

| Offset | Field | Notes |
|---|---|---|
| 0 | `effect` u8 | id from the effect catalogue |
| 1 | `palette` u8 | id from the palette catalogue |
| 2 | `speed` u8 | time multiplier = 2^((speed − 128) / 32); 128 = 1× |
| 3 | `intensity` u8 | effect-specific amount (tail length, density, …) |
| 4 | `color1` rgb | primary colour |
| 7 | `color2` rgb | secondary colour |

### Segment geometry (4 bytes)

| Offset | Field | Notes |
|---|---|---|
| 0 | `strip_mask` u8 | bit 0 = left strip, bit 1 = right strip; both bits = the same pixels on both strips |
| 1 | `start` u8 | first pixel index on the strip |
| 2 | `length` u8 | 1…144, `start + length` ≤ strip length |
| 3 | `flags` u8 | bit 0 = reversed |

A segment renders `length` virtual pixels once and copies them to every strip in `strip_mask`.
Up to 8 segments. Where segments overlap, the higher index wins.

## Commands (`command` characteristic)

| Opcode | Name | Payload |
|---|---|---|
| `0x01` | `SET_POWER` | `on` u8 (0/1) |
| `0x02` | `SET_BRIGHTNESS` | `value` u8 |
| `0x03` | `SET_EFFECT` | `segment` u8, EffectConfig |
| `0x04` | `SET_PARAM` | `segment` u8, `param` u8, value |
| `0x05` | `SET_SEGMENTS` | `count` u8, `count` × geometry |
| `0x06` | `IDENTIFY` | `strip_mask` u8, `start` u8, `length` u8, `seconds` u8 |
| `0x10` | `PRESET_SAVE` | `slot` u8, `name` str (≤ 16 bytes) |
| `0x11` | `PRESET_LOAD` | `slot` u8 |
| `0x12` | `PRESET_DELETE` | `slot` u8 |
| `0x13` | `GET_PRESETS` | — |
| `0x20` | `GET_INFO` | — |
| `0x21` | `GET_CATALOG` | `kind` u8 (0 = effects, 1 = palettes), `start` u8 |
| `0x30` | `SET_BUDGET` | `milliamps` u16 |
| `0x31` | `SET_DEV_MODE` | `on` u8 (0/1) |
| `0x32` | `BOOT_OTHER_SLOT` | `confirm` u8, always `0x5A` |

- `segment` = `0xFF` means every segment.
- `SET_PARAM` ids and value types: 1 `effect` u8, 2 `palette` u8, 3 `speed` u8, 4 `intensity` u8, 5 `color1` rgb, 6 `color2` rgb.
- `SET_SEGMENTS` replaces the layout. Segment *i* keeps the EffectConfig it had; new segments copy segment 0. It carries no effects, so an app that reorders segments follows it with `SET_EFFECT` per moved segment.
- `IDENTIFY` lights the given range dim white for `seconds`, over whatever is playing, even when power is off. `length` 0 or `seconds` 0 cancels it. A new `IDENTIFY` replaces a running one, and the app may send it write-without-response while a zone edge is dragged.
- `SET_BUDGET` is the total current allowed at the battery, clamped to 400…2000 mA. Default 900. The jacket draws roughly 370 mA with every LED dark (the LEDs' idle current plus the ESP32), so that part of the budget produces no light.
- `SET_DEV_MODE` turns WiFi, OTA and the log stream on or off.
- `BOOT_OTHER_SLOT` restarts the jacket into the firmware in its other app slot: the firmware that ran before the last update, which on a board bought with other firmware and updated once is that other firmware. The jacket reads and verifies that image first. When it is good the jacket answers nothing, does not change `STATE` or `rev`, closes the link and resets about two seconds later; whatever starts then may not speak this protocol at all. The jacket's stored scene, presets and settings are kept, and are there again when jacket firmware next runs. When the other slot holds no firmware that verifies, the answer is `ERROR` code 4 and nothing changes. `confirm` must be `0x5A`, so that a stray write cannot trigger it. It is a maintenance command: an app should not offer it next to everyday controls.
- Preset slots are 0…7. A preset stores the whole scene: power is not stored, brightness, segments and their EffectConfigs are.
- Commands that only read (`GET_*`) answer with one notification. Commands that change something cause a `STATE` notification.
- `PRESET_SAVE` and `PRESET_DELETE`, when accepted, also answer with a `PRESETS` notification carrying the updated list. Its order relative to `STATE` is not defined.

### Validation

A rejected command changes nothing and is answered with `ERROR`.

- Code 2 (too short) takes precedence, unless a bad value makes the required length unknowable (`SET_PARAM` id, segment count, name length); that is code 3.
- An empty write is answered `ERROR(opcode 0, code 2)`.
- Code 3: `on` other than 0/1; a `BOOT_OTHER_SLOT` `confirm` other than `0x5A`; unknown effect, palette or `SET_PARAM` id; segment index ≥ the current count (other than `0xFF`); slot ≥ 8; name longer than 16 bytes; `strip_mask` 0 or with unknown bits; unknown geometry flag bits; segment count 0 or above 8; geometry `length` 0 or `start + length` past the end of the strip (also for `IDENTIFY`, unless it is a cancel); `GET_CATALOG` kind above 1.
- Code 4: `PRESET_LOAD` or `PRESET_DELETE` on an empty slot; `BOOT_OTHER_SLOT` when the other app slot holds no firmware that verifies.
- Code 5: the jacket could not write to its storage (`PRESET_SAVE`, `PRESET_DELETE`).

### State rules

- `rev` starts at 0 at boot and increments on every accepted command other than `IDENTIFY`, `BOOT_OTHER_SLOT` and `GET_*`, even when nothing visible changes, and when the WiFi or live-input flag changes. Each increment is followed by a `STATE` notification, so `STATE` doubles as the acknowledgement. The app must not rely on `rev` for ordering across reconnects.
- `preset` is cleared to `0xFF` when brightness, segments or any EffectConfig actually change value, and when the marked slot is deleted. Power, budget and dev mode do not clear it.
- Power is not stored: the jacket always boots on, with the last scene, budget and dev-mode setting restored.
- The effect state of a segment restarts when its effect or length changes, or when a preset is loaded.

## Notifications (`state` characteristic)

Reading the characteristic returns the latest `STATE` frame.

### `0x01` STATE

| Offset | Field | Notes |
|---|---|---|
| 0 | type = `0x01` | |
| 1 | `proto` u8 | 1 |
| 2 | `rev` u16 | increments on every change, wraps |
| 4 | `power` u8 | 0/1 |
| 5 | `brightness` u8 | |
| 6 | `budget_ma` u16 | |
| 8 | `flags` u8 | bit 0 dev mode, bit 1 WiFi connected, bit 2 live input active |
| 9 | `preset` u8 | slot last loaded or saved if the scene is unchanged since, else `0xFF` |
| 10 | `segment_count` u8 | 1…8 |
| 11 | segments | `segment_count` × (geometry 4 bytes + EffectConfig 10 bytes) |

The jacket coalesces changes and notifies at most about 10 times per second.

### `0x02` CATALOG

`kind` u8, `start` u8, `total` u8, `count` u8, then `count` entries.

- Effect entry: `id` u8, `param_flags` u8, `name` str.
- Palette entry: `id` u8, `name` str.
- `param_flags`: bit 0 uses color1, bit 1 uses color2, bit 2 uses palette, bit 3 uses speed, bit 4 uses intensity, bit 5 reacts to sound, bit 6 reacts to motion.
- The app asks again with `start` + `count` until it has `total` entries.
- A page holds as many entries as fit in 180 bytes. `start` ≥ `total` returns an empty page.
- Colour bits mean the effect uses that colour directly. The palette bit means the effect also takes colours from the selected palette; because palette 0 is built from `color1` and `color2`, the app shows both colour pickers whenever an effect has the palette bit and palette 0 is selected.

### `0x04` PRESETS

`count` u8, then `count` × (`slot` u8, `name` str). Only used slots are listed.

### `0x05` INFO

`proto` u8, `fw_major` u8, `fw_minor` u8, `fw_patch` u8, `strip_count` u8, `strip_count` × `leds` u16, `max_segments` u8, `preset_slots` u8, `effect_count` u8, `palette_count` u8, `build` str.

### `0x7F` ERROR

`opcode` u8, `code` u8. Codes: 1 unknown opcode, 2 frame too short, 3 value out of range, 4 not found (empty preset slot, no firmware in the other app slot), 5 storage failure.

Notification type `0x03` is reserved. The app ignores notification types it does not know.

## Live input (`live` characteristic)

14 bytes, sent by the phone about 30 times per second while live mode is on.

| Offset | Field | Notes |
|---|---|---|
| 0 | `flags` u8 | bit 0 audio fields valid, bit 1 motion fields valid, bit 2 beat fields valid |
| 1 | `seq` u8 | increments per frame |
| 2 | `level` u8 | overall loudness |
| 3 | `bass` u8 | |
| 4 | `mid` u8 | |
| 5 | `treble` u8 | |
| 6 | `bpm_x10` u16 | tempo × 10 |
| 8 | `beat_phase` u8 | position within the current beat, 0…255 (see below) |
| 9 | `motion` u8 | movement magnitude |
| 10 | `tilt_x` i8 | |
| 11 | `tilt_y` i8 | |
| 12 | `t_ms` u16 | low 16 bits of the phone's millisecond clock when the frame was built; wraps every 65.5 s |

Any frame of at least 14 bytes sets the live-input flag; shorter frames are dropped silently. If no frame arrives for 500 ms the jacket clears the flag and reactive effects fall back to their non-reactive behaviour (beat-locked effects later, see below).

Field meaning:

- A group whose `flags` bit is clear is ignored, and the phone sends its fields as 0. When a group disappears the jacket fades its influence out over about 300 ms. The beat bit implies the audio bit.
- `level`, `bass`, `mid`, `treble`, `motion` are auto-ranged on the phone against the recent loudest / most vigorous moment: 0 = silence / still, 255 = at that ceiling (so a steady sound reads high). The phone sends responsive values, each frame carrying the peak since the previous one; the jacket uses them linearly and smooths them itself.
- `tilt_x`, `tilt_y` come from the direction of gravity in the phone's own axes: (0, 0) with the phone lying flat, `tilt_y` +127 with it upright, `tilt_x` +127 with its right edge up. There is no calibration to how the phone is carried.
- `bpm_x10` outside 300…3000 (30…300 BPM) counts as no beat; the phone sends 0 when the beat bit is clear. The phone tracks about 66…186 BPM, so faster or slower music arrives as half or double tempo, and the beat bit can drop for under a second on a tempo change.
- `beat_phase` is `floor(phase × 256)`, 0 on the beat and rising through it, taken at the instant `t_ms` was sampled plus the user's latency offset (see the last point).
- `t_ms` is a monotonic millisecond clock and is valid in every frame. The jacket estimates the phone-to-jacket clock offset from it, runs its own beat clock between frames, and counts beats itself to place accents (the frame carries no bar position).
- Beat-locked effects keep running on the jacket's beat clock for up to 3 s after the last valid beat before falling back, so short dropouts do not break the rhythm.
- The app compensates for link and audio latency by shifting the `beat_phase` it sends (the user's latency offset); the jacket applies none.

## Catalogue ids (v1 firmware)

Ids are stable once published. The app never hard-codes names or parameters; it reads the catalogue.

| Id | Effect | `param_flags` |
|---|---|---|
| 0 | Solid | `0x01` |
| 1 | Comet | `0x1D` |
| 2 | Rainbow | `0x18` |
| 3 | Matrix | `0x1C` |
| 4 | Breathe | `0x19` |
| 5 | Heartbeat | `0x19` |
| 6 | Cylon | `0x1C` |
| 7 | Beat Pulse | `0x3B` |
| 8 | VU Meter | `0x3C` |
| 9 | Sparkle | `0x5C` |

| Id | Palette |
|---|---|
| 0 | Colours |
| 1 | Rainbow |
| 2 | Matrix |
| 3 | Cyberpunk |
| 4 | Fire |
| 5 | Ocean |

Palette 0 is always "Colours": a gradient from `color1` to `color2`.
