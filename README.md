# KitBit: an always-on desk pet

KitBit is a desk pet for the Seeed XIAO ESP32C3. It has animated robot eyes on an OLED, plays cute buzzer sounds, and uses an MPU6050 to feel what happens to it and around it on the desk.

## Hardware

| Part | XIAO ESP32C3 pin |
|---|---|
| 1.3" SH1106 128x64 OLED (I2C, 0x3C); for a 0.96" SSD1306 set `OLED_SH1106 0` | SDA D4, SCL D5 |
| MPU6050 or clone (I2C, 0x68 or 0x69) | SDA D4, SCL D5 (same bus) |
| Passive buzzer | D1 |

Other ESP32 boards work too: change the pins in [kitbit/config.h](kitbit/config.h). On a classic ESP32 / S2 / S3 you can also add a touch pad (a wire or copper tape on the pet's head) with `ENABLE_TOUCH 1`. The C3 has no touch hardware.

## Libraries

Adafruit GFX, Adafruit SH110X (or Adafruit SSD1306 for 0.96" screens), [FluxGarage RoboEyes](https://github.com/FluxGarage/RoboEyes), CuteBuzzerSounds. The sketch was built against ESP32 core 3.3.7, board "XIAO_ESP32C3" (keep "USB CDC On Boot" enabled, the default, so the serial monitor works over USB).

```sh
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3 kitbit
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32C3 -p /dev/ttyACM0 kitbit
```

The motion sensor is driven directly (no MPU library), so clone "MPU6050" boards that are really an MPU6500/MPU9250 or report an odd ID work too. At boot the serial log lists every I2C device found, the sensor's WHO_AM_I, and a gravity check that should read about 1.00 g.

## First-time setup

### Calibrate the sensor mounting (once)

The pet needs to know which way the sensor is mounted inside it. Start calibration by typing `calibrate` in the serial monitor (115200 baud), or by **tapping the pet 6 times quickly**. The screen walks you through it:

1. **"stand me upright"**: put it in its normal pose on the desk and let go. After about 1.5 s of stillness it beeps.
2. **"lay me on my back"**: lay it down with the screen facing the ceiling and let go. After about 1.5 s it says "Calibrated!".

This measures the exact mounting, at any angle, plus the gyro's zero-offset, and saves both to flash so it survives power cycles. `calreset` clears it and falls back to the `IMU_*` mapping in [kitbit/config.h](kitbit/config.h).

To check it worked, type `imu`. Upright should show `up` at about `+1.00`, and tilting the pet to its right should make `right` go negative.

### Tune desk-activity detection

Type `imu` and watch `desk=` while you type on your keyboard. If it never shows `1`, lower `DESK_MARGIN_G`. If it's always `1`, raise it.

## What it does

### Handling it
| You do | KitBit does |
|---|---|
| Tilt it | Eyes roll towards the lower side. This overrides all other eye movement, including while you hold it. |
| Tap it once | Boop: it blinks, looks towards your finger and chirps |
| Double tap | Pet: happy eyes, floating hearts, cuddly sound or purr |
| Triple tap | It sings a little tune with music notes (if it isn't too tired) |
| Tap more than 10 times in 10 s | Gets annoyed |
| Pick it up | "Wheee", looks around while you carry it |
| Shake it a little | Giggles |
| Shake or spin it a lot | Dizzy: wobbly eyes and circling stars, then grumpy if it went on too long |
| Drop it | Screams while falling, then "oof" when it lands. If you catch it, it sighs with relief. |
| Slam the desk | Startled: wide eyes and a "!" |
| Turn it upside down | The screen flips so the face stays upright, and it complains "help!". After 20 s it decides it's a bat, and after a minute it takes a bat nap. |
| Lay it on its side | Bedtime: it yawns and falls asleep |
| Put it face down | Hiding / do not disturb: screen off and completely silent |
| Turn it back over | Peekaboo! |
| Lay it on its back | Stargazing |

### Living on your desk
- **Notices you working.** It feels your typing through the desk and watches your keyboard while you work. While you're working it keeps its idle sounds to itself.
- **Break buddy.** After 50 min of continuous desk activity it starts sweating, looks up at you and shows "break?". It repeats this every 5 min until you double-tap it, pick it up, or walk away for 5 min.
- **Sleeps when you leave and greets you when you're back.** After a while with nobody around it gets drowsy and then falls asleep: the display dims, "Zzz" floats up, it snores a little while drifting off and then sleeps silently. When you sit back down after 15+ min it wakes with hearts.
- **Sleep behaviour.** If you tap it right after it fell asleep, one eye opens, it grumbles and goes back to sleep. Now and then it peeks with one eye while dreaming.
- **Needs.** *Happiness* goes up with attention and down when it's ignored or mistreated. *Energy* drains while it's awake and recharges while it sleeps. Together they set its resting face: neutral, happy, or tired/sad.
- **Random quirks.** It winks, looks around, whistles, yawns, sighs when bored, asks for attention with a "?", giggles to itself, gets hiccups (any tap cures them) and sneezes.
- **Memory.** Happiness, energy and age are saved to flash. Every 24 h of total uptime it has a birthday and sings.

## Serial commands (115200 baud)

`help status imu calibrate calreset mute unmute chatty <0-3> sleep wake pet boop sing scare annoy dizzy hiccup sneeze greet break birthday happy sad angry sound <id> reset`

`chatty` sets how talkative it is:

| Level | Sounds |
|---|---|
| 0 | Silent |
| 1 | Reactions only |
| 2 (default) | Also idle chatter, but only when you're not working |
| 3 | Always chatty |

## How it's built

The structure follows Robo_Lamp_ai. Each part of the work runs in its own FreeRTOS task, so the eye animation never stutters:

| Task | Priority | What it does |
|---|---|---|
| eyes | 2 | ~30 fps; draws the RoboEyes frame plus overlays |
| sound | 1 | Plays queued CuteBuzzerSounds and custom sounds |
| loop | 1 | Reads the MPU at ~100 Hz, then runs the pet's behaviour logic at 50 Hz |

The C3 is single-core, so all three share it; each task sleeps between frames or sounds, which leaves plenty of time.

The OLED and MPU share the I2C bus behind one mutex. Every threshold and timing is in [kitbit/config.h](kitbit/config.h).

Eyes are drawn with FluxGarage RoboEyes by Dennis Hoelscher ([fluxgarage.com](https://www.fluxgarage.com)).
