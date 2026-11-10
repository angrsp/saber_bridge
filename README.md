# saber_bridge

BLE ↔ USB CDC/ACM bridge firmware for OpenCore lightsabers (Anima NXT), running on a Seeed XIAO ESP32-S3.

## About

OpenCore sabers are configured over a USB serial port (CDC/ACM) with a simple text protocol
(`WR?`, `c?`, `B=`, `SAVE`, …). This firmware turns a XIAO ESP32-S3 into a small battery-powered dongle
that plugs into the saber's USB port through an OTG adapter, acts as the USB host and exposes the same
serial link over Bluetooth Low Energy as a Nordic UART Service (NUS). A phone app can then read and change
the saber's colours wirelessly, without a cable to a PC.

Besides the NUS, the bridge exposes a status characteristic (saber connected, lost bytes, …) and the
standard Device Information Service. A "fake" build variant emulates the saber, so the app can be developed
and tested with the XIAO alone.

The development plan and the BLE contract shared with the app are in
[`piano-FW-bridge.md`](piano-FW-bridge.md); the progress summary is in
[`riepilogo-sviluppi.md`](riepilogo-sviluppi.md) (both in Italian).

## Building

Toolchain: ESP-IDF 5.5.6. Without installing it, using Docker:

```
tools/build-all.sh        # real variant in build/, fake variant in build-fake/
tools/idf-docker.sh build # real variant only
tools/test-host.sh        # PC-side tests: NXT emulator and pipe
```

With ESP-IDF installed (Windows installer): `idf.py build`; for the fake variant
`idf.py -B build-fake -D SDKCONFIG=build-fake/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.fake" build`.

Options live in the "Saber Bridge" menu (`tools/idf-docker.sh menuconfig`).

## Variants

- **Real**: USB CDC/ACM host towards the saber (only 0483:5740 NXT and 16C0:0483 EVO, unless the option
  is changed). The console is on UART0 (D6 = TX, D7 = RX): logs need a 3.3 V USB-UART adapter.
- **Fake** (`sdkconfig.fake`): no USB host; an NXT emulator answers the colour commands.
  The console stays on the XIAO's USB; from the serial monitor, `unplug` and `plug` simulate the saber
  being disconnected and reconnected. Use it to test the app over BLE with just the board.

## LED (GPIO21)

| LED | State |
|---|---|
| slow blink | advertising, no client |
| fast blink | client connected, saber absent |
| solid | client connected and saber attached |

## Flashing

The container cannot see COM ports: flash from the host with esptool (`pip install esptool`),
with the OTG adapter removed and the XIAO connected to the PC (if it does not show up, hold BOOT while
plugging it in):

```
cd build
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash "@flash_args"
```

For the fake variant, do the same from `build-fake/`.

## F0: real saber responses

With the saber powered on and connected to the PC through the cable with the red wire cut:

```
pip install pyserial
python tools/f0_probe.py
```

The script sends the phase F0 commands, compares the responses with the emulator, checks colour
persistence across a power cycle (skip with `--skip-persistence`) and writes an `f0-report-*.md` report.
Before changing anything it saves the main colours to `f0-backup-*.json` and restores them at the end;
if it is interrupted halfway: `python tools/f0_probe.py --restore f0-backup-<date>.json`.

## F5: testing the bridge over BLE

With the bridge powered on (fake variant, or real variant with the saber attached) and the PC's Bluetooth on:

```
pip install bleak
python tools/bridge_test.py
```

The script finds the bridge (by the NUS UUID), reads the DIS and the status, sends the read-only commands,
requests a long response that must arrive over several notifications, then sends 500 commands in a row
(`--count` to change it) and measures latency as the app sees it. At the end it checks that the status
reports no lost bytes and writes a `bridge-test-*.md` report. It does not change the saber's colours.

With `--serial COM5` it runs the same tests on the saber connected directly to the PC, without the bridge:
useful to compare latencies (and, with the emulator on a pty, for the PC-side tests in `tools/test-host.sh`).
