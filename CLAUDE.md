# KitBit: notes for working on this repo

There are two sketches:
- `kitbit/` is the desk pet. All tunables are in `kitbit/config.h`.
- `kitbit_diagnosis/` tests each component one by one.

The user-facing documentation is `README.md`.

## Build

`arduino-cli` is not on PATH. Use the copy bundled with the IDE:

```sh
A=/home/armesh/Downloads/arduino-ide_2.3.8_Linux_64bit/resources/app/lib/backend/resources/arduino-cli
$A compile --fqbn esp32:esp32:XIAO_ESP32C3 --warnings all kitbit
```

This uses ESP32 core 3.3.7. Compile with `--warnings all`. The sketch currently builds with no warnings from `kitbit.ino`.

## Hardware as built (current board)

| Part | Pin / connection |
|---|---|
| Board | Seeed XIAO ESP32C3 (single core, no touch hardware) |
| OLED | 1.3" SH1106, mounted upside down (`DISPLAY_ROTATION 2`) |
| IMU | MPU6050 clone that is really an MPU6500 (WHO_AM_I 0x70). Driven by direct register access, with no MPU library, because libraries reject the chip ID. The mounting calibration is stored in flash. |
| I2C | SDA on **D7**, SCL on D5 |
| Buzzer | Passive, on D1 |
| Battery | 1S LiPo, 700 mAh, on the BAT pads |
| Battery divider | Two equal resistors (100k to 130k) from BAT+ to D0 to GND; `BATT_DIVIDER 2.0` |
| INMP441 mic | Planned on D2 (SCK), D3 (WS) and D10 (SD); not used by the pet sketch yet |

**D4 / GPIO6 is shorted to GND on this board.** Don't use it. That's why SDA moved to D7.

Use D7 rather than D6 for I2C, because the boot ROM prints log output on D6 (TX).

## Battery charging detection

This board has no USB/5V sense wire (`PIN_USB_SENSE -1`), so `battMeasure()` in `kitbit.ino` has to guess whether it's charging. Its signals are:

- **USB connected to a computer** (`HWCDC::isPlugged()`). This is reliable, but it is ignored for the first 5 s after boot, because it misreported when the battery was connected before USB.
- **The battery voltage jumping up and staying up**, which suggests a charger was plugged in.
  - Normal check: +0.08 V, confirmed after 20 s, plus a slow-rise rule of +0.07 V over 5 min.
  - **Long check**, used while asleep or hiding, for 10 min after waking, and for 10 min after boot (`battSettling()`): +0.10 V confirmed after 60 s, and the slow-rise rule is off. Under lighter load in sleep, the voltage creeps up by itself and caused false "charging" readings.
- **At boot it always assumes "on battery"**, unless it is waking from the empty-battery deep sleep.

These heuristics were tuned after false positives seen on real hardware. If you change them, keep the logging: each state change prints its reason, and the `batt` command shows the current reason.

## Next hardware build: TODO

**Sense USB power directly instead of guessing from the battery voltage.** Add a divider from the XIAO's 5V pin:

```
5V --[10k]-- D6 --[20k]-- GND
```

Then set `#define PIN_USB_SENSE D6` in `config.h`. Charging then comes straight from the 5V pin. That covers phone chargers too, and all the voltage-jump heuristics above stop being used, because the `#if PIN_USB_SENSE >= 0` branch replaces them.

First check that the 5V pin reads about 0 V when the board runs on battery alone. If the battery back-feeds that pin, the sense divider won't work.

## Motion sensing

- **Taps come from the sensor FIFO.** It samples the accelerometer at 250 Hz with about a 184 Hz filter. On the 6500 family, FIFO_MODE makes it stop when full instead of scrambling. `imuFifoPeak()` drains the whole FIFO, drops implausible samples, and gives each jolt its exact time from its position in the FIFO. Mic clicks carry exact times as well, and a tap is a click plus a jolt within `TAP_MATCH_MS` of each other.
- **Taps vs desk slams.** Measured with `tools/tap_test.py --slam`: after a slam the desk keeps the pet shaking for 45–250 ms, while a tap dies out at once. Size and direction overlap between the two, so they can't separate them. Each jolt is judged after `SLAM_RING_TO_MS`; a ringing jolt is a slam, which startles the pet and swallows its mic click.
- **The OLED hogs the I2C bus.** A frame takes about 27 ms. At 30 fps, the loop and IMU ran at only about 12 Hz, and the FIFO overflowed into garbage. Keep `EYES_FPS` at 20 or lower.
- **If the FIFO never delivers data, taps fall back to plain samples.** A message is printed when this happens.
- **Each tap counts once.** Taps are ignored while the buzzer plays and for 150 ms after, because its vibration used to count as taps. After a tap, the next one only counts once the sensor has calmed below `TAP_G / 2`. Calibration has no tap gesture, because it used to trigger by accident; it starts only from serial `calibrate`.
- **Shaking was too sensitive before.** It now needs movement above `SHAKE_G`, and dizziness builds up more slowly. If you lower these thresholds again, test them on the hardware.

## Power

- **"Off"** is deep sleep. It is entered three ways: face down plus a double tap (handled in `onTaps` while hiding), automatically after `AUTO_OFF_FACEDOWN_MIN` face down on battery, or with the serial command `off`.
- **Waking from "off"** happens only when a charger is plugged in, or on RESET, because jostling in a bag used to trigger the tap wake. `offModeCheck()` runs every `OFF_CHECK_S` seconds and looks for three things:
  - the battery voltage rising more than `OFF_CHARGE_RISE_V` above the slowly tracked rest voltage
  - the optional `PIN_USB_SENSE`
  - every 4th check, a 300 ms look for a *new* USB host, via the start-of-frame counter in `USB_SERIAL_JTAG_FRAM_NUM_REG`
  The flip + double-tap wake (`offListenForDoubleTap()`) is still in the code behind `OFF_WAKE_BY_TAP`.
- **Below `BATT_SHUTDOWN_V` (3.3 V) for 20 s**, it goes into a protective deep sleep and checks every 60 s for a charger.
- **`offModeCheck()`** runs first in `setup()` and returns to sleep quickly when there is no reason to wake.

## Code conventions

- **Declare all structs and enums near the top of `kitbit.ino`.** Arduino generates function prototypes, and they break if a type is declared after them.
- **Include RoboEyes last.** It defines macros such as N, S, E, W, DEFAULT, ON and OFF.
- **The core already defines `FAIL`**, which is why the diagnosis sketch uses `R_PASS`, `R_FAIL` and `R_CHECK`.
- **Make sure CuteBuzzerSounds `bendTones()` always advances.** It steps with an int, so check that `freq * (step - 1) >= 1` at the low end of every bend; otherwise it loops forever.
- **Keep text wrap off on the display.** Glyphs near the edge otherwise wrap to the other side.
- **Keep things above the bottom rows hidden by the case.** `SCREEN_HIDDEN_BOTTOM` sets how many rows are covered. Place new bottom-of-screen elements relative to it. `keepEyesOnScreen()` clamps the eye position every frame.
- **Commit messages end with** `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
