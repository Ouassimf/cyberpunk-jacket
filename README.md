# Cyberpunk jacket

A jacket with two addressable LED strips, driven by an ESP32 and controlled from an Android phone over Bluetooth LE. The phone picks effects, colours, zones and presets, and can feed the jacket live sound and motion so the effects follow the music and the wearer. On a controller with a microphone of its own the jacket can hear the music itself.

This repository holds the jacket firmware, the Android app and the protocol the two share.

## Status

Read this first: a lot of the code has only ever run in tests.

| | Verified how |
|---|---|
| Portable firmware core (protocol, scene, effects, beat clock, audio analysis, boot decisions) | Host unit tests (`pio test -e native`) |
| Firmware on the ESP32: boot, Bluetooth, every protocol command, persistence, frame rate, crash guard | On a real devkit, with `firmware/tools/ble_check.py` driving it from a PC |
| Firmware on the Gledopto controller, the board in the jacket | `gledopto_bench` installed once through its WLED page. It advertises, answers `GET_INFO`, and a PC camera showed both front strips: solid red, green and blue in the right colours, dark when off, Rainbow, Comet moving. **Not checked there:** the button, the microphone, the way back to WLED, the strips on the back, the current drawn. |
| **How the light looks to the eye** (crossfades, flicker, beat timing, brightness against the budget) | **Never judged.** The camera check above is all there is. |
| Installing through WLED's update page, and starting under WLED's old bootloader | On the devkit set up as a WLED board, many times, and once on the Gledopto. See [What was rehearsed](#what-was-rehearsed). |
| The way back to the previous firmware, all three ways (crash guard, button, `BOOT_OTHER_SLOT`), ending with WLED up and answering; both firmwares keeping their settings across the trip | On the devkit set up as a WLED board. The button was pressed from the PC, through the USB adapter's DTR line. |
| A start-up that crashes or hangs is counted; safe mode; every refusal when there is no firmware to go back to | On the devkit. |
| Built-in microphone: the capture task and what it costs | Measured on the devkit with no microphone wired, in silence and with a synthetic beat in place of the samples: see [Built-in microphone](#built-in-microphone). **It has never heard a sound.** |
| **WiFi, OTA upload, the log stream, the bootloader's rollback** | **Never run on hardware.** The code compiles and links (`jacket_ci`, `gledopto_ci`). |
| **Power budget numbers** | Estimates from datasheets. Not measured. See [Power budget](#power-budget). |
| **The Android app** | Unit tests and a demo mode. It has connected to the Gledopto and changed the effect; nothing more was checked. |

## Hardware

Two WS2812B strips, 144 LEDs each, colour order GRB, and one of two controllers. The firmware is built for one or the other (`JACKET_BOARD_DEVKIT`, `JACKET_BOARD_GLEDOPTO`); the wiring of each is one block of constants in `firmware/src/config.h`.

| | ESP32 devkit | Gledopto GL-C-010WL |
|---|---|---|
| What it is | ESP32-WROOM-32 devkit, 4 MB flash, wired by hand. The bench board. | "ESP32 WLED Digital LED Controller with Mic", sold running WLED. **The board the jacket's strips are wired to.** |
| Left strip data | GPIO 14 | GPIO 16 (first output) |
| Right strip data | GPIO 12 | GPIO 2 (second output) |
| Status LED | GPIO 2: on while a phone is connected | none (GPIO 2 is a data output) |
| Button | BOOT, GPIO 0 | OPT, GPIO 0 |
| Microphone | none (`devkit_mic` runs the code on GPIO 26 / 5 / 21 for measurements) | I2S, data GPIO 26, word select GPIO 5, clock GPIO 21, no MCLK |
| LED power switch | none | none: WLED had no relay configured and the strips lit |
| USB / serial | yes | **none** |
| Power | 5 V USB power bank feeding strips and board | 5–24 V on screw terminals, here a USB power bank |
| Flash layout | this project's own bootloader and `min_spiffs` table: two app slots of 0x1E0000 | WLED's bootloader (ESP-IDF 3.3) and table: two app slots of 0x180000 |

- Devkit: GPIO 12 is a boot strapping pin. It must not be pulled high while the ESP32 resets, or the chip selects the wrong flash voltage and does not boot. A WS2812B data input does not pull it up; do not add a pull-up to that line.
- Both: GPIO 0 is the boot-mode pin. **A button on GPIO 0 that is held down while power is applied keeps the chip in its ROM's download mode: no firmware runs until power is applied again with the button released.** Everything this firmware does with the button therefore begins after power-on.
- Gledopto: the pin numbers above are what its WLED 0.14.3 reported for this unit (two LED outputs of 144, one button, a "generic I2S" microphone that WLED had working, no relay). Which half of the I2S frame the microphone answers in is not in that configuration; see [Built-in microphone](#built-in-microphone).
- Gledopto, as its owner ran it under WLED: a current limit of 2800 mA, and each strip split at LED 100 (0–99 and 100–143), which is a sensible first guess for zones. This firmware's budget is lower: see [Power budget](#power-budget).
- Both boards advertise as "Jacket". Power only one at a time when a phone is to find the right one.

## Repository layout

| Path | What |
|---|---|
| `protocol/PROTOCOL.md` | The Bluetooth protocol. Single source of truth for firmware and app. |
| `protocol/vectors/v1.json` | Golden frames. Both codecs are tested against them. |
| `firmware/` | PlatformIO project for the ESP32. |
| `firmware/lib/core/` | Portable core: no Arduino, no ESP-IDF. Runs on the ESP32 and in host tests. |
| `firmware/src/` | Device code: Bluetooth, LED output, storage, WiFi/OTA, boot guard. |
| `firmware/test/` | Host unit tests for the core. |
| `firmware/tools/ble_check.py` | Hardware check over Bluetooth, run from a PC. |
| `firmware/tools/boot_other_slot.py` | Sends a jacket back to the firmware in its other app slot, over Bluetooth. |
| `firmware/tools/read_status.py` | Reads a jacket's status line over Bluetooth: the heartbeat of a board without a serial port. |
| `firmware/partitions_wled_4mb.csv` | WLED's partition table, so the Gledopto builds are checked against its slot size. Never flashed. |
| `android/` | The Android app (Kotlin, Compose). |
| `docs/` | The old Web Bluetooth control page. **It only works with the legacy firmware**, not with this one. |

The original prototype firmware is kept under the git tag `legacy-v1` (`git checkout legacy-v1`).

## Prerequisites (Windows)

- **PlatformIO Core.** The VS Code extension installs it at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`. Run it from **PowerShell, not Git Bash**: under Git Bash the ESP32 toolchain install fails with "MSys/Mingw is not supported".
- **MinGW-w64** (`g++` on `PATH`) for the host tests. Open a new terminal after installing it so `PATH` is picked up.
- **Python 3** for the hardware check, plus a Bluetooth adapter on the PC.
- **JDK 17 or newer and the Android SDK** for the app. The SDK is found through `ANDROID_HOME`, or through `sdk.dir` in `android/local.properties` (gitignored).

The commands below assume PowerShell in the `firmware` folder and this shorthand:

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
```

Replace `COMx` with the devkit's serial port. Run PlatformIO for one thing at a time: changing `platformio.ini` makes the next run delete every environment's build folder, images included.

## Firmware

### Environments

| Environment | Board | What it is for |
|---|---|---|
| `native` | host | Host unit tests of `lib/core`. No ESP32 code. |
| `jacket_bench` | devkit | USB build that **never** contains WiFi credentials, even if `secrets.ini` exists. WiFi and OTA can never start. Use it for work on Bluetooth and LEDs. |
| `jacket_usb` | devkit | USB build with the credentials from `secrets.ini`, dev mode on at every boot. First flash, and recovery. This is the default environment. |
| `jacket_ota` | devkit | The same build, uploaded over WiFi to a devkit that already runs a build with the same `secrets.ini`. |
| `jacket_ci` | devkit | Build check only, never flashed: the `jacket_usb` code paths with made-up credentials. |
| `devkit_mic` | devkit | `jacket_bench` with the microphone code running on GPIO 26 / 5 / 21: to measure what it costs, or for an I2S microphone wired there. |
| `gledopto_bench` | Gledopto | No credentials, no microphone: the plainest image, and the first one to install through WLED's page. |
| `gledopto_bench_mic` | Gledopto | The same with the microphone. |
| `gledopto_ota` | Gledopto | Microphone and the credentials from `secrets.ini`. Builds the file for WLED's page; with `-t upload` sends it over WiFi to `gledopto_ip`. |
| `gledopto_ci` | Gledopto | Build check only, never installed: the `gledopto_ota` code paths with made-up credentials. |
| `gledopto_crashtest` | Gledopto | **Test only, never worn.** Crashes on purpose two seconds after the boot guard, at every boot, to prove the way back to the previous firmware. |

```powershell
& $pio test -e native                                    # host tests
& $pio test -e native -f test_beat -v                    # one suite, with its printed measurements
& $pio run -e jacket_bench                               # build
& $pio run -e jacket_bench -t upload --upload-port COMx  # build and flash over USB
& $pio run -e jacket_ci -e gledopto_ci                   # check that the WiFi/OTA code builds and fits, for both boards
& $pio device monitor --port COMx --baud 115200          # serial log
```

A plain `& $pio run` builds `jacket_usb`, credentials included.

Every ESP32 build ends with a line about the image file and the app slot it must fit, and fails if it does not:

```
Image: firmware.bin is 850672 bytes, 54.1 % of its 1572864-byte app slot (722192 bytes free)
```

The limit is `custom_max_image_bytes` in `platformio.ini`: 1 966 080 for the devkit's own layout, 1 572 864 for the Gledopto, whose layout is WLED's and is not replaced. *(The failing case was tried by lowering the limit below an image's size.)*

### Serial log

A banner at boot, then one heartbeat line every 5 s:

```
[boot] jacket fw=2.0.0 build="bench ..." slot=app0 reset=power-on crashes=0 safe_mode=no
[boot] board=devkit core=3.3.12 idf=v5.5.5 cpu=80MHz storage=ok
[boot] slot app0 at 0x10000, 1966080 bytes, holds image 994bfa9d71038092 of 850704 bytes, confirmed healthy earlier
[boot] other slot app1 at 0x1f0000, 1966080 bytes: empty
[hb] up=5s heap=... min_heap=... stack=... | ble=advertising cmds=0 dropped=0/0/0 | fps=60.1 frame=2663/3801us |
     power=on bri=64 out=64 budget=900mA est=500mA | wifi=none | live=off rx=0 beat=0.0bpm/none late=0ms
```

The two slot lines say where this firmware runs and what the other app slot holds, read from the partition table on the chip: `empty`, or `a firmware, image ..., ESP-IDF v...` (an earlier jacket firmware, or WLED). They are what tells whether there is a firmware to go back to.

`frame` is average/worst time to render and send one frame. `out` is the brightness after the power budget; `est` is the modelled current. `dropped` counts commands, replies and over-long frames that were lost (all three should stay 0). `live` and `beat` show the phone's live input and the beat clock. A build with the microphone adds `| mic=... snd=...`, explained under [Built-in microphone](#built-in-microphone). Markers at the end of the line: `SAFE-MODE`, `FIRMWARE-UNCONFIRMED` (the bootloader is waiting), `firmware-not-yet-confirmed` (this image has not proven itself yet), `leds-suspended`, `recovery-cue`.

The Gledopto has no serial port. The essentials of its heartbeat can be read over Bluetooth, see [Reading the status over Bluetooth](#reading-the-status-over-bluetooth); the whole log only over WiFi (the log stream below), which takes a build with credentials.

### WiFi credentials (`secrets.ini`)

WiFi exists only to carry OTA uploads and the log stream.

1. Copy `firmware/secrets.ini.example` to `firmware/secrets.ini` (gitignored) and fill it in.
2. Build `jacket_usb` or `jacket_ota` (devkit), or `gledopto_ota` (Gledopto).

**The values are compiled into the firmware image.** Anyone with the `.bin` file or the flashed chip can read them. Do not share images built with real credentials; share `jacket_bench` builds instead. With `wifi_ssid` or `ota_password` empty, WiFi and OTA stay off.

Limits on the values, because of how they reach the compiler and the upload tool:

- No `"`, `'`, `\` or `$` in any value; write a literal `%` as `%%`.
- **OTA password: letters, digits and `-` `_` `.` only.**

WiFi, OTA and the log stream run only when the build has credentials **and** dev mode is on. `jacket_usb`, `jacket_ota` and `gledopto_ota` builds switch dev mode on at every boot; the app can switch it off until the next boot. In safe mode they run whatever dev mode says.

Away from its network the jacket does not keep searching: joining is retried with growing pauses, and if no address was obtained within 3 minutes WiFi is switched off (`wifi=gave-up` in the heartbeat) until the next boot, or until dev mode is switched off and on from the app. In safe mode it never gives up: it tries once a minute for as long as it is powered.

### First flash, then OTA (devkit)

*Not verified on hardware. This is how it is configured.*

1. First flash over USB:
   ```powershell
   & $pio run -e jacket_usb -t upload --upload-port COMx
   ```
2. Read the jacket's address from the serial heartbeat (`wifi=<address> <signal>`) and put it in `secrets.ini` as `ota_ip`.
3. Allow the upload through the PC's firewall, once. The jacket connects back to the PC on **TCP 45678** (fixed in `platformio.ini`). From an administrator PowerShell:
   ```powershell
   New-NetFirewallRule -DisplayName "Jacket OTA" -Direction Inbound -Protocol TCP -LocalPort 45678 -Action Allow -Profile Private
   ```
4. Upload over WiFi:
   ```powershell
   & $pio run -e jacket_ota -t upload
   ```

The jacket listens for uploads on port **3232** and asks for the OTA password. Checking the password takes the ESP32 about 4 seconds, during which the LEDs freeze on their last frame; during the upload itself they are dark, and the scene is saved first.

**After an upload, do not power the jacket off until the serial log (or the log stream) says `new firmware confirmed`.** Until then every heartbeat ends with `FIRMWARE-UNCONFIRMED`, a reset brings the previous firmware back, and further uploads are not accepted. See [Rollback and safe mode](#rollback-and-safe-mode).

**Log stream:** while WiFi is up, the same lines as the serial log are available on TCP port **23**, read-only, to any raw TCP client. A reader that connects first receives the most recent couple of kilobytes.

### Rollback and safe mode

The flash holds two app slots. One runs; the other holds whatever ran before the last update. What protects against a bad firmware depends on what it does:

- **It crashes at every start.** Every boot caused by a panic, a watchdog or a brown-out is counted, from the first lines of start-up (a start-up that hangs for 5 s counts too). After three in a row:
  - a firmware that was **never confirmed** gives way: the firmware in the other slot is read and verified in full, selected, and the chip is reset into it. This needs nothing from the bootloader, so it also works under WLED's. *(Counting and the decision: verified on the devkit and in host tests. The switch: see [The way back](#the-way-back-to-the-previous-firmware).)*
  - a firmware that was confirmed once, or one with nothing usable in the other slot, starts in **safe mode** *(verified on the devkit)*: the stored scene is not restored (the default one plays), the budget is kept, the microphone stays off, and on a build with credentials WiFi and the upload listener run whatever dev mode says, so a fixed firmware can be uploaded. Running 20 s without a crash clears the counter; the stored scene is untouched and comes back at the next normal boot. The boot banner shows `safe_mode=YES` and every heartbeat ends with `SAFE-MODE`.
- **It runs, but is not what you want, or cannot be reached for an update.** Go back by hand: [The way back](#the-way-back-to-the-previous-firmware).
- **It hangs.** A loop pass that hangs for 5 s resets the chip (watchdog), which counts as a crash. So does a start-up that hangs *(verified on the devkit, with a firmware that stops in `setup()` on purpose)*.

**Confirmed** means: the firmware ran 20 s healthy and, on a build with credentials and dev mode on, WiFi is connected and the upload listener is running again, so a firmware that cannot be replaced over the air does not count as proven. The log says `this firmware is confirmed healthy`; until then every heartbeat ends with `firmware-not-yet-confirmed`. It is remembered per image, in flash.

This cannot make two firmwares bounce to each other for ever. The firmware that takes over after an automatic switch is marked as having arrived that way and may not switch back by itself: if it crashes too it ends in safe mode and stays. The mark goes when that firmware is confirmed, or when someone installs another. So there is at most one automatic switch per installed firmware; the worst case is three crashes of the new one, the switch, three crashes of the old one, safe mode. And there is no switch at all unless the note about it could be written to flash and read back. The reasoning is at the top of `firmware/lib/core/src/jacket/boot_policy.h` and played through in `firmware/test/test_boot`.

**Bootloader rollback** *(devkit only; not verified on hardware)*. The devkit's own bootloader also starts a newly uploaded firmware "pending" and brings the previous one back by itself if the chip resets before the firmware is confirmed. Until then the boot banner shows `slot=... (unconfirmed)`, the log says `new firmware NOT CONFIRMED: waiting for ...`, every heartbeat ends with `FIRMWARE-UNCONFIRMED`, and no further upload is accepted. If WiFi never connects (the 3-minute rule above), the firmware stays unconfirmed and the next reset rolls it back. WLED's bootloader on the Gledopto has no such thing; there the three crashes above are the only automatic way back.

What none of this covers: a firmware that dies before its first line runs (the boot guard is the first thing `setup()` does after opening the serial port and the storage). On a board without a serial port that needs a serial adapter and an opened case.

### The way back to the previous firmware

Three ways lead to the firmware in the other app slot. All three do the same thing, in this order:

1. Read and verify the whole firmware in the other slot. If it is missing or damaged, refuse: nothing changes.
2. Note in flash which firmware takes over, and select the other slot.
3. Reset the whole chip.

Nothing is erased, no app slot is written and none is marked invalid. Both firmwares keep their settings: the jacket's scene, presets and budget are in the settings partition both share, WLED's are in its own filesystem partition, and each finds its own again after the other has run *(rehearsed with WLED 0.14.3, in both directions)*.

Step 3 is a reset as deep as software can make it, through the chip's RTC watchdog, not the restart a firmware normally gives itself. The ordinary restart resets the processors and leaves the rest of the chip as the jacket firmware set it up, and WLED 0.14.3, built on a much older ESP-IDF, cannot always start from that: see [What was rehearsed](#what-was-rehearsed). The serial log shows `rst:0x10 (RTCWDT_RTC_RESET)`. That reset reads the boot-mode pin again, which is one more reason the button's switch happens on release.

| How | What to do | What you see |
|---|---|---|
| By itself | Nothing: a firmware that was never confirmed crashes three times in a row. | The previous firmware starts, about ten seconds after the first crash. |
| Button | Apply power **first**. Within 10 s press the button (OPT on the Gledopto, BOOT on the devkit) and hold it for 3 s. When the first 12 LEDs of both strips turn **green**, let go: the jacket restarts into the other firmware. **Red** means there is no firmware to go back to; nothing changes. | Devkit: the status LED flickers while the button counts. The switch happens when the button is released; a button that stays down for another 10 s is taken for a stuck pin and ignored. |
| Bluetooth | `python firmware\tools\boot_other_slot.py` from a PC (same virtual environment as `ble_check.py`; add `--yes` to skip the question). It sends `BOOT_OTHER_SLOT` (`32 5a`, see `PROTOCOL.md`). | The script prints `ACCEPTED` when the jacket drops the link, or `REFUSED: ERROR code 4` when there is no firmware to go back to. |

- Do not hold the button while plugging the power in: with a button on GPIO 0 that is the ROM's download mode, and nothing runs until power is applied again.
- On the devkit a serial tool that asserts DTR alone pulls GPIO 0 low and looks like the button. Releasing it between 3 and 13 s after a reset would switch firmware, if the other slot holds one.
- After the first install through WLED the other slot holds **WLED**, and stays WLED for as long as every jacket firmware is installed through WLED's own update page: WLED always writes the slot it is not running from. The first upload over WiFi (`gledopto_ota -t upload`) overwrites WLED with jacket firmware; from then on "the previous firmware" is the previous jacket firmware and WLED is gone from the chip.
- *Verified on the devkit set up as a WLED board: all three ways, each ending in `rst:0x10` and WLED answering HTTP; and under this project's own layout every refusal (empty other slot). In host tests: the decisions. Not exercised: the fallback when the full reset does not happen, power lost during a switch, and a switch between two jacket firmwares (the other slot always held WLED).*
- With the devkit's **own** bootloader do not try the way back to a WLED in the other slot: that bootloader waits for the firmware it starts to confirm itself, WLED never does, and it would be abandoned at its second start. The Gledopto's bootloader is WLED's and has no such rule.

### Reading the status over Bluetooth

For a board without a serial port. Beside the protocol's service the jacket has a small second one with a single readable value, `status`: a line with the essentials of the heartbeat (`PROTOCOL.md`, "Diagnostics"). It is filled in only while dev mode is on.

```powershell
& "$env:LOCALAPPDATA\jacket-venv\Scripts\python.exe" tools\read_status.py --address <the board's Bluetooth address> --count 6
```

```
21:23:26  up=76s reset=power-on crashes=0 fps=60.0 frame=2601/3338us heap=129204 min_heap=129076 | mic=on load=26.0% worst=29699us ovf=0 stack=3944 slot=L pk=11779/0 fr=166 snd=mic
```

The script switches dev mode on if it was off and off again when it leaves (on a build with WiFi credentials that starts WiFi meanwhile). `--effect 8` also puts VU Meter on every zone while it reads, which makes a built-in microphone run, and puts the effects back afterwards; that changes the scene, so the preset mark is lost. *(Verified on the devkit.)*

**With more than one board powered, always give `--address`,** to this script, to `ble_check.py` and to `boot_other_slot.py`: both boards advertise the same service, and without it the first one heard is used.

### Hardware check over Bluetooth

`firmware/tools/ble_check.py` connects to a real jacket from the PC, sends every command in the protocol and compares the answers with `PROTOCOL.md`. With `--port` it also watches the serial log (resets, heap, frame time) and resets the board to check persistence.

Create the virtual environment once. Keep its path short on Windows:

```powershell
python -m venv "$env:LOCALAPPDATA\jacket-venv"
& "$env:LOCALAPPDATA\jacket-venv\Scripts\python.exe" -m pip install -r tools\requirements.txt
```

Run it (about three minutes):

```powershell
& "$env:LOCALAPPDATA\jacket-venv\Scripts\python.exe" tools\ble_check.py --port COMx
```

It prints a pass/fail line per check and a summary, and exits with 0 when everything passed. It changes the scene, brightness, budget and dev mode many times and puts them back at the end. `BOOT_OTHER_SLOT` is only sent in forms the jacket must refuse. It saves and deletes preset slot 2, and refuses to touch presets if that slot is in use unless `--overwrite-presets` is given. On a build with credentials, toggling dev mode starts and stops WiFi.

Close any serial monitor first: the script needs the port.

## Gledopto controller

*On the Gledopto itself only the first install has been done, once. Everything else in this section was rehearsed on the devkit set up as a WLED board; see the end of the section.*

The Gledopto has no USB or serial port. Firmware gets onto it in two ways only: through the update page of the WLED it was sold with, and, once a jacket firmware with WiFi credentials runs on it, over WiFi. A firmware that does not start cannot be replaced without a serial adapter and an opened case, which is why the first image is the plainest one and why there are three ways back.

Only the app is ever replaced. The bootloader and the partition table stay WLED's (ESP-IDF 3.3, two app slots of 0x180000 = 1 572 864 bytes), and the firmware reads the layout from the chip instead of assuming its own. An image larger than the slot fails the build.

### First install, through WLED

1. Build the image. Copy it somewhere else before doing anything more with PlatformIO:
   ```powershell
   & $pio run -e gledopto_bench
   Copy-Item .pio\build\gledopto_bench\firmware.bin $env:TEMP\gledopto_bench.bin
   ```
   Use `firmware.bin`. **Not** `firmware.factory.bin`, which also contains a bootloader and a partition table.
2. Power the Gledopto and reach its WLED: on your network, or on its own access point (`WLED-AP`, address `4.3.2.1`).
3. Open `http://<address>/update`, choose the file, press *Update!*. From a script it is one multipart field named `update`:
   ```powershell
   curl.exe -F "update=@$env:TEMP\gledopto_bench.bin" http://<address>/update
   ```
   WLED 0.14.3 answers `Update successful! Rebooting...` after a few seconds and restarts. It checks that the file is an ESP32 firmware image, nothing more; it must have its update lock off (*Settings → Security*), as this unit's had. **No answer is not a failure:** in the rehearsals one upload in five got no HTTP reply at all (the client gave up after two minutes) although WLED had taken the image and restarted into it. What tells is what starts afterwards.
4. The jacket firmware now runs from the slot WLED was not in. WLED is still in its own. Look for "Jacket" over Bluetooth: `ble_check.py` without `--port` works against it.
5. If the firmware crashes at start, WLED is back about ten seconds later. If neither "Jacket" nor WLED ever appears, the image did not start at all: see the last paragraph of [Rollback and safe mode](#rollback-and-safe-mode).

To replace a `gledopto_bench` firmware (it has no WiFi): go [back to WLED](#the-way-back-to-the-previous-firmware) and install the new image from there. Both keep their settings through all of this.

### With WiFi

*Not verified on any hardware.*

`gledopto_ota` is the build with the credentials from `secrets.ini` and the microphone. Installed through WLED's page like any other image, it joins the network; read its address from the router (host name `jacket`) and put it in `secrets.ini` as `gledopto_ip`. From then on:

```powershell
& $pio run -e gledopto_ota -t upload
```

The firewall rule and everything else under [First flash, then OTA](#first-flash-then-ota-devkit) apply. Remember what the first such upload costs: it overwrites WLED.

### What was rehearsed

On the devkit, erased and set up as a WLED board: stock WLED 0.14.3 with its own bootloader (Arduino core 1.0.6, ESP-IDF 3.3) and its 4 MB partition table. The `jacket_bench` firmware.bin of the day (the devkit's pins, built before the Gledopto profile existed) was sent to WLED's `/update` page with `curl` from a phone joined to `WLED-AP`. WLED answered `Update successful! Rebooting...` in 7 s; the image, built on ESP-IDF 5.5, started from slot `app1` under the old bootloader, advertised, opened its storage and drew 60 frames a second, and `ble_check.py` gave 226 passed, 1 failed, 2 skipped. The failure was the device name in the scan response, with the PC hearing the board at −84 to −91 dBm; the same signal was measured under this project's own bootloader afterwards.

The one new thing in the log under WLED's partition table is two lines `E esp_core_dump_flash: No core dump partition found!` at every boot. They come from ESP-IDF's start-up, before any of this firmware's code, and are harmless: WLED's table has no core dump partition, so a crash leaves no dump.

Later rehearsals, same set-up, with the first `gledopto_*` images:

- `gledopto_crashtest` installed through WLED's page: three boots that each ended in the deliberate crash, then `this firmware crashed 3 times in a row and never ran healthy`, the other slot recognised as `a firmware, image ..., ESP-IDF v3.3.6-...`, verified (1 324 832 bytes, 0.7 s at 80 MHz), selected, and WLED started. **But WLED then aborted about six seconds into every start, for ever**, until the EN pin was pulsed. Erasing the settings partition first changed nothing.
- `gledopto_bench` installed the same way, brightness changed and a preset saved over Bluetooth, then `boot_other_slot.py`: accepted, and WLED came up and stayed up, answering HTTP, with the jacket's entries still in the shared settings partition. The same image installed again from WLED started with that brightness and that preset.

The difference between the two: the second firmware had started its Bluetooth radio before handing over, the first had not started anything. In both cases the hand-over ended in an ordinary restart, which resets the processors only. So something the newer ESP-IDF sets up in the rest of the chip while the radio is unused survives that restart, and the old ESP-IDF in WLED cannot bring its radio up from it; a real reset clears it. What exactly was not established. The answer does not depend on it: every way back now ends in a reset of the whole chip.

That version, rehearsed the same way, all three ways back:

- By itself: `gledopto_crashtest` installed from WLED, crashes counted 0, 1, 2, then `crashed 3 times in a row and never ran healthy`, 1.5 s later `slot app0 verified (1324832 bytes) and selected. Resetting the whole chip into it`, `rst:0x10 (RTCWDT_RTC_RESET)`, and WLED up 1.2 s after that, answering HTTP, still up two minutes later.
- Bluetooth: `gledopto_bench` installed from WLED and confirmed healthy; `boot_other_slot.py` reported `ACCEPTED: the jacket dropped the link 1.9 s after the command`; `rst:0x10` two seconds after the command arrived; WLED answering HTTP.
- Button: GPIO 0 pulled low from 2.0 s to 6.1 s after a reset; `button down` at 2.3 s, `button held: let go now` at 5.3 s, the switch 1.6 s after the release, `rst:0x10`, WLED answering HTTP.
- Settings: a brightness and a preset set over Bluetooth were there again after going to WLED and installing the jacket firmware again, three times over. WLED kept its own.
- `ble_check.py` against `gledopto_bench` under WLED's bootloader and partition table: 230 passed, 0 failed, 1 skipped.
- Uploads to WLED's `/update`: seven of eight answered `Update successful! Rebooting...` within 10 to 17 s; one got no reply although the image was installed.

Then the same image went onto the Gledopto, through its own WLED page (answered in 17 s). What was seen there is in the [Status](#status) table.

What all of this does not show: the Gledopto's button, its microphone, and the way back on that board; the microphone build on any board with a microphone; anything over WiFi.

### Built-in microphone

*The analysis is tested on the host against synthetic sound (`test_audio`). The capture task has run on the devkit with nothing wired to its pins. It has never heard a sound, on any board.*

With a microphone on the board the jacket makes for itself what a phone in live mode sends: `firmware/src/mic.cpp` reads the microphone at 16 kHz, `firmware/lib/core/src/jacket/audio/` (the app's analyser, ported) turns it into level, bass, mid, treble, tempo and beat phase, and 33 times a second that reaches the effects by the same path as a phone's live frame.

- **Phone and microphone together.** A phone frame that carries sound makes the phone the source of sound for the next 500 ms, and the microphone's frames are dropped meanwhile. Otherwise the microphone supplies sound and beat, and the phone's frames speak for motion and tilt only: a phone that sends motion while the jacket hears the music itself works. The hand-over is a glide, not a cut. The live-input flag in `STATE` still means "a phone is sending": the app cannot see that the jacket is listening by itself.
- **Only while needed.** The microphone runs while the jacket is on and some zone plays an effect that reacts to sound (Beat Pulse, VU Meter), and for 5 s after. Otherwise the I2S clock is stopped and the task sleeps. Never in safe mode, never during a firmware upload.
- **Which slot.** An I2S microphone answers in the left or the right half of the frame, depending on how one of its pins is strapped, which nothing reports. `kMicRightSlot` in `config.h` chooses; the heartbeat shows the peak of both, the one in use first. `pk=0/1800` over music means the other one is right.
- **Level.** `kMicDropBits` in `config.h`: 24-bit samples become 16-bit by dropping 7 bits, about a phone's level, clipping from about 114 dB SPL for the usual microphones. Not checked against this board's microphone.

The heartbeat of a build with the microphone, and the status line read over Bluetooth, end like this:

```
... | mic=on load=23.0% worst=26920us ovf=0 stack=4140 slot=L pk=11811/0 fr=166 snd=mic
```

`load` is the share of one core spent on conversion and analysis, `worst` the longest single processing call, `ovf` the number of 10 ms driver buffers lost since boot because they were not read in time, `stack` the task's lowest free stack, `slot` the half of the I2S frame in use, `pk` the peak in that slot and in the other since the previous line (32767 would be clipping), `fr` the frames made, `snd` who supplies the sound now (`mic`, `phone`, `none`).

**What it costs**, measured on the devkit at 80 MHz (`devkit_mic`, nothing wired to the pins). A pin with nothing on it reads as perfect silence, so the second column was measured with the samples replaced by a synthetic drum beat at 120 BPM (`JACKET_TEST_MIC_SYNTH`, a test-only build flag):

| | Silence | Synthetic beat |
|---|---|---|
| Microphone task, share of core 0 | 12.1 to 12.7 % | 22.3 to 23.1 %; 26 to 29 % while a phone or PC is connected |
| Longest single processing call | 2.6 to 9.1 ms | 24 to 27 ms; up to 40 ms while connected |
| Driver buffers lost | 0 | 0 |
| Frames per second | 60.0 | 60.0 |
| Frame time, average / worst | 2.5 to 2.9 ms / 4.5 ms | 2.5 to 2.6 ms / 4.3 ms |
| Frame time with a phone's 30 Hz live stream on top (`ble_check.py`) | | 3.6 ms / 5.4 ms |
| Free memory while it runs | 17.8 KB less (129.5 KB free) | the same |
| Task stack never used | 4.1 KB of 6 KB | 3.9 KB of 6 KB |

- 80 MHz is enough, with room: the task has core 0 to share only with the radio and takes under a third of it. The LEDs are drawn on core 1, and neither the frame rate nor the frame time moved. The clock was not raised.
- The long call is the tempo estimate, four times a second. It is longer than one LED frame, which is why it is on the other core, and the driver holds 120 ms of sound while it runs.
- With the synthetic beat the beat clock locked at 120.0 BPM from the board's own analysis, and an effect that reacts to sound followed it: the whole chain from samples to effect, except the microphone.
- When the last effect that reacts to sound goes, the task stops 5 s later and the memory is back *(seen in the log)*.
- `ble_check.py` against that build: 230 passed, 1 failed, 1 skipped. The failure was the check that the beat clock reports no beat a few seconds after the phone's stream ends; with a beat of the board's own it never does, as intended. The script now skips that check when the heartbeat says the microphone supplies the sound. *(The changed script was not run again against that build.)*
- **Which slot**, again: if the slot in use stays dead silent for two seconds while the other carries signal, the task changes over by itself and logs it; a living microphone has noise of its own. Not exercised: it takes a microphone.

**On the first board with a real microphone, look at** `slot` and `pk` in a quiet room and with music: a peak of a few counts in silence, hundreds to a few thousand with music at a normal level, and the other slot at 0. A peak pinned at 32767 means `kMicDropBits` is too small for that microphone; a peak that stays under about 50 with loud music means it is too large, or the samples are not arriving as 24 bits left-justified. `snd=mic` with an effect that reacts to sound selected, and `ovf` staying where it is.

## Power budget

`SET_BUDGET` is the total current the jacket may draw at the battery, 400 to 2000 mA, default 900. The firmware lowers the brightness until its model of the draw fits:

```
total mA = system + idle + light x brightness
```

| Term | Value | Where |
|---|---|---|
| system | 80 mA with Bluetooth, 180 mA with WiFi up | `kSystemMaBle`, `kSystemMaWifi`, per board (the Gledopto's are the devkit's, assumed) |
| idle | 1 mA per LED with every channel off: 288 mA | `kLedIdleMaX10` |
| light | per LED at full: 16 mA red, 11 mA green, 15 mA blue | FastLED's figures |

The constants are in `firmware/src/config.h`; the model is described in `firmware/src/led_output.h`.

- **About 370 mA is drawn with every LED dark** (about 470 mA with WiFi up). That part of the budget produces no light, and "power off" still draws it: nothing switches the strips off. A budget that leaves nothing for light dims the picture to its lowest step instead of switching it off, which goes slightly over the budget.
- **The numbers are estimates, not measurements.** Calibration is still to do: measure the current with the strips dark and with a known colour, and correct the three constants. The heartbeat prints the modelled draw as `est=`.
- FastLED's built-in limiter is not used, because it scales the idle current with brightness and would let the jacket exceed its budget.
- The Gledopto's owner ran WLED with a limit of 2800 mA. `SET_BUDGET` stops at 2000 mA and starts at 900: at the same brightness setting the jacket firmware will be dimmer than WLED was, until the budget is raised.

## Adding an effect

1. Write one file in `firmware/lib/core/src/jacket/effects/` with a render function. The rules are at the top of `firmware/lib/core/src/jacket/effect.h`: draw from effect time and per-segment state only, never from the frame count. A reactive effect reads the smoothed `reactive` values and must have a fallback.
2. Declare the function in `effects/effects.h`.
3. Append one row to the table in `firmware/lib/core/src/jacket/effect_registry.cpp`. The row index is the catalogue id. Ids are published to the app: append only, never reorder.
4. Tests in `firmware/test/test_effects/test_main.cpp`: add the id and name to the catalogue test and raise `kEffectIds`, so the generic checks (speed, frame-rate independence, honest `param_flags`, no writes outside the segment) cover it. Add tests for its own look. Record its snapshot hash; `-v` prints the new value.
5. Update the effect count in the engine tests and in `firmware/tools/ble_check.py` (`EFFECTS`).
6. Add the row to the catalogue table in `protocol/PROTOCOL.md`, and raise `effect_count` in the `info` vector and `total` in `catalog_effects_page` in `protocol/vectors/v1.json`. The firmware tests compare against those vectors exactly.

The app needs no change: it reads the catalogue from the jacket.

## Android app

*Built and unit-tested on a PC. On a real phone it has connected to the Gledopto and changed the effect; nothing more was checked.*

From the `android` folder:

```powershell
.\gradlew.bat assembleDebug        # builds app\build\outputs\apk\debug\app-debug.apk
.\gradlew.bat testDebugUnitTest    # unit tests, including the golden protocol vectors
.\gradlew.bat installDebug         # installs on a phone connected through adb
```

The app needs Android 12 or newer. It asks for the Bluetooth permission, and for the microphone when live mode is used.

**Demo mode:** Settings → Demo mode replaces the Bluetooth link with a simulated jacket inside the app. Nothing is sent over Bluetooth, so every screen can be tried without the hardware, including on an emulator.

## Restoring an earlier firmware from a flash backup

*Devkit only: this needs the serial port.*

A full image of the 4 MB flash can be saved before experimenting and written back later. `esptool` ships with PlatformIO:

```powershell
$esptool = "$env:USERPROFILE\.platformio\penv\Scripts\esptool.exe"
& $esptool --port COMx --baud 460800 read-flash 0 0x400000 backup.bin    # save
& $esptool --port COMx --baud 460800 write-flash 0 backup.bin            # restore
```

Restoring overwrites everything on the chip: firmware, stored scene, presets and settings. Keep backups out of the repository (`firmware/.backup/` is gitignored): an image of a jacket that ran a build with credentials contains them.
