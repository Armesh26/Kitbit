// =====================================================================
//  KitBit - configuration
//  Everything you might want to tune lives here.
// =====================================================================
#pragma once

// ---------------- Pins (Seeed XIAO ESP32C3) ---------------------------
#define PIN_SDA            D7      // GPIO20 (RX pin). Moved from D4: GPIO6 is shorted to GND on this board
#define PIN_SCL            D5      // GPIO7
#define PIN_BUZZER         D1      // GPIO3, passive buzzer (CuteBuzzerSounds)

// Optional capacitive "head pat" pad (a wire / copper tape on a touch pin).
// Not available on the ESP32-C3 (no touch hardware) - classic ESP32 / S2 / S3 only.
#define ENABLE_TOUCH       0
#define PIN_TOUCH          T0
#define TOUCH_RATIO        0.70f   // touched when reading drops below baseline*ratio (classic ESP32)

// ---------------- Display ----------------------------------------------
// 1.3" OLEDs almost always use the SH1106 chip, 0.96" ones the SSD1306.
// Wrong choice = garbled screen / noisy strip at the edge.
#define OLED_SH1106        1       // 1 = SH1106 (1.3"), 0 = SSD1306 (0.96")
#define SCREEN_WIDTH       128
#define SCREEN_HEIGHT      64
#define OLED_ADDR          0x3C
#define DISPLAY_ROTATION   2       // 0 = normal, 2 = display mounted upside down (180 deg)
#define EYES_FPS           20      // each frame keeps the I2C bus busy ~27 ms; 30 fps starved the motion sensor
// The OLED and the MPU6050 share the I2C bus. 400 kHz is the OLED's rated
// maximum; going faster garbles the picture on many modules.
#define I2C_DISPLAY_HZ     400000
#define I2C_IDLE_HZ        400000

// Eye geometry (smaller eyes = more room to look around)
#define EYE_W              40
#define EYE_H              40
#define EYE_GAP            14
#define EYE_RADIUS         10
#define EYE_WIDE_EXTRA     12      // extra height for "wide eyed" surprise
#define EYE_MARGIN         4       // keep the eyes this far from the edges (curious eyes grow, wobbles shake)
#define SCREEN_HIDDEN_BOTTOM 6     // rows at the bottom covered by the case: battery icon, text and eyes stay above

// ---------------- MPU6050 axis mapping ----------------------------------
// Body frame of the pet, seen from the front (looking at the screen):
//   RIGHT = towards your right, UP = top of the screen, OUT = out of the screen towards you.
// Pick which sensor axis (0 = X, 1 = Y, 2 = Z) and sign maps to each body axis.
// Default: MPU board lying flat in the base, chip facing up, sensor +X to the right,
// sensor +Y pointing to the back of the pet.
// These are only the fallback: run the calibration (serial "calibrate") and the exact
// mounting - any angle - is measured and saved to flash.
// Check with the serial command "imu": standing upright should show  up ~ +1.00
#define IMU_RIGHT_AXIS     0
#define IMU_RIGHT_SIGN     (+1)
#define IMU_UP_AXIS        2
#define IMU_UP_SIGN        (+1)
#define IMU_OUT_AXIS       1
#define IMU_OUT_SIGN       (-1)

// ---------------- Motion thresholds (in g / deg per second) ----------
// Taps are measured from the sensor's own 250 Hz buffer: the peak kick (g) away from gravity.
// Watch "tap=" in the "imu" stream while tapping to tune these.
#define TAP_G              0.20f   // a kick bigger than this counts as a tap / knock
#define BUMP_G             3.00f   // anything harder is a startling bump / desk slam (firm taps reach ~1.8 g)
#define TAP_DEBOUNCE_MS    120

// ---------------- Microphone (INMP441, I2S) -----------------------------
// VDD 3V3, GND, L/R -> GND, SCK D2, WS D3, SD D10.
// Taps are counted from the sharp click the mic hears; the IMU only has to agree
// that the pet itself got touched (otherwise typing next to it would count).
// If the mic doesn't answer at boot, taps fall back to the IMU alone.
#define ENABLE_MIC         1
#define PIN_MIC_SCK        D2
#define PIN_MIC_WS         D3
#define PIN_MIC_SD         D10
#define TAP_SOURCE         1       // 0 = IMU only, 1 = mic click + IMU jolt together (best), 2 = mic only
#define MIC_CLICK_RATIO    4.0f    // a click is this many times louder than the background (a hand muffles the mic)
#define MIC_CLICK_MIN      8000.0f // ...and at least this loud (watch "mic" on serial to tune)
// All IMU jolt sizes below are measured on the FAST part of the motion only (above
// ~20 Hz): a tap is a sharp spike, moving / tilting / holding the pet is slow and is
// filtered out, so the same numbers work on a desk and in your hand.
#define HF_CUTOFF_HZ       20.0f
#define TAP_CONFIRM_G      0.04f   // the tiny IMU jolt that confirms a mic click was on the pet
#define TAP_MATCH_MS       150     // click and jolt must be this close in time (measured: usually <35 ms, seen 115)
// Tap or desk slam? Measured: after a slam the desk keeps the pet shaking for ~70-85 ms,
// after a finger tap the jolt is gone at once. So a jolt that is still above SLAM_RING_G
// SLAM_RING_FROM_MS after it started (checked until SLAM_RING_TO_MS) was a slam.
#define SLAM_RING_G        0.10f
#define SLAM_RING_FROM_MS  45
#define SLAM_RING_TO_MS    130
#define SLAM_MIN_G         0.45f   // and it has to be at least this big (mugs, nudges, heavy typing stay below)
#define SLAM_NEEDS_BANG    1       // ...and the mic must hear a bang within 200 ms (if the mic works)
#define SLAM_COOLDOWN_MS   8000    // at most one startle this often
// Slams only exist when it stands on something solid. Measured: a still hand is as quiet
// as a desk for this sensor, so "on a surface" = standing upright in its calibrated pose
// (a hand practically never holds it that exactly) and not rotating.
#define SURFACE_GYRO_DPS   2.5f
#define SURFACE_TILT_DEG   8.0f
#define TAP_MAX_GYRO_DPS   150.0f  // turning faster than this = being swung around, not tapped
#define PICKUP_IGNORE_MS   700     // grabbing it makes clicks and jolts too

// Head scratches: measured per 100 ms window, scratching = mic 7-15x louder than the room
// and the pet vibrating ~0.04 g in 65-85 % of windows; typing / taps: under 10 %.
#define SCRATCH_MIC_RATIO  4.0f    // a window is "scratchy" if the mic is this much louder than the room...
#define SCRATCH_IMU_G      0.02f   // ...and the pet itself vibrates at least this much (fast part)
#define SCRATCH_START_OF_10 6      // scratchy windows out of the last 10 (1 s) to start purring
#define SCRATCH_STOP_OF_10  2      // ...and to stop
#define TAP_WINDOW_MS      450     // taps closer than this are grouped (double / triple tap)
#define HANDLE_GYRO_DPS    25.0f   // rotation that means "someone is holding me"
#define SHAKE_G            1.0f    // movement (g) that counts towards shaking; gentle handling stays below
#define SHAKE_START        1.5f    // shake meter level to start "being shaken" (~0.6 s of real shaking)
#define SHAKE_GRUMPY_MS    6000    // shaken longer than this -> grumpy
#define SPIN_DPS           300.0f  // spinning around the vertical axis
#define DIZZY_SHOW         2.5f    // dizziness that makes the eyes go swirly (~3-4 s of hard shaking)
#define FREEFALL_G         0.35f
#define LAND_G             2.0f
#define ORIENT_SETTLE_MS   600     // how long a new orientation must hold
#define TILT_GAIN          3.0f    // how strongly the eyes roll towards the low side (full at ~20 deg)
#define TILT_DEADZONE_DEG  4.0f    // smaller tilts are ignored

// Desk-activity ("you are typing next to me") detection. The pet learns the
// quiet noise floor itself; vibration above floor*RATIO + MARGIN means activity.
#define DESK_RATIO         2.2f
#define DESK_MARGIN_G      0.006f

// ---------------- Personality / timing -----------------------------------
#define CHATTINESS         2       // 0 silent, 1 reactions only, 2 + idle chatter when you're not working, 3 always chatty
#define QUIRK_MIN_S        25      // random idle quirks every QUIRK_MIN_S..QUIRK_MAX_S seconds
#define QUIRK_MAX_S        70
#define BORED_AFTER_MIN    4       // no interaction -> bored, sighs, asks for attention
#define DROWSY_AFTER_MIN   8       // nobody around (no taps, no desk activity) -> drowsy
#define SLEEP_AFTER_MIN    12      // ... -> asleep
#define AWAY_MIN           15      // you were gone this long -> welcome-back greeting
#define SNORE_MINUTES      8       // only snores while drifting off, then sleeps silently
#define ENERGY_AWAKE_HOURS 6.0f    // full -> empty while awake
#define ENERGY_RECHARGE_MIN 45.0f  // empty -> full while asleep

// Break buddy: nags you to take a break after continuous desk activity
#define BREAK_REMINDER_MIN 50      // 0 disables
#define BREAK_NAG_EVERY_MIN 5
#define BREAK_RESET_MIN    5       // this long without desk activity = you took a break

// ---------------- Battery (1S LiPo on the XIAO's BAT+/BAT- pads) ---------
// The XIAO charges the LiPo by itself, but has no way to measure it. Add a
// voltage divider:   BAT+ --[220k]-- D0 --[220k]-- GND   (+ 100nF from D0 to GND
// for steadier readings). Without the divider the battery features switch
// themselves off at boot (the pin is checked).
#define ENABLE_BATTERY     1
#define PIN_BATTERY        D0      // A0 / GPIO2
#define BATT_DIVIDER       2.0f    // (R1 + R2) / R2
#define BATT_CAL           1.00f   // fixed correction; usually leave at 1: the pet calibrates itself
                                   // (charging plateau = 4.20 V) or use serial "battcal <multimeter volts>"
#define BATT_CV_V          4.20f   // the charger holds the cell exactly here at the end of charging
// Optional real "plugged in" signal: 5V pin --[10k]-- D6 --[20k]-- GND.
// Without it, charging is detected from USB-to-computer and from the voltage
// jumping up / down when the cable goes in / out.
#define PIN_USB_SENSE      -1      // -1 = not fitted, else e.g. D6

#define BATT_LOW_PCT       20      // "I'm hungry..."
#define BATT_CRITICAL_PCT  7       // "feed me!", screen dims
#define BATT_NAG_MIN       10      // repeat the hungry reminder (critical: every 3 min)
#define BATT_FULL_V        4.15f   // charging, above this and no longer rising = full

// Over-discharge protection: below this (for BATT_SHUTDOWN_S seconds) the pet says
// goodnight and goes into deep sleep (~0.1 mA), checking once a minute whether a
// charger was plugged in. 3.3 V leaves the cell some reserve so it isn't damaged.
#define BATT_SHUTDOWN_V    3.30f
#define BATT_SHUTDOWN_S    20
#define BATT_OFF_CHECK_S   60

// ---------------- Power off -------------------------------------------------
// "Off" = deep sleep with the screen and motion sensor powered down.
// It wakes up when you move / turn it (or press RESET, or plug in a charger).
#define AUTO_OFF_FACEDOWN_MIN 0    // 0 = never: face down is the low-power mode that face-up wakes (see below)

// Face down = power save: screen + mic off, CPU at 80 MHz in light sleep, waking every
// PS_WAKE_MS to read the motion sensor (its FIFO keeps recording meanwhile). Turned face
// up -> everything is back within ~0.4 s. Face down + double tap -> really off (deep sleep).
#define PS_WAKE_MS         200
// Switching off by hand: put it face down and double-tap it.
// Switching back on: plug in a charger / USB (or press RESET).
// OFF_WAKE_BY_TAP 1 also allows: turn it over -> "tap tap to wake" -> double tap.
#define OFF_WAKE_BY_TAP    0
#define OFF_CHECK_S        3       // how often the sleeping pet checks for a charger (and, with taps, for being moved)
#define OFF_CHARGE_RISE_V  0.07f   // battery voltage rise that means "a charger was plugged in"
#define OFF_WAKE_DEG       30.0f   // that much turning makes it listen for the double tap
#define OFF_LISTEN_MS      6000    // how long it listens (screen hint on) before sleeping again
#define OFF_TAP_G          0.10f   // jolt that counts as a tap while it is listening (IMU only)
#define HIDE_TAP_G         0.06f   // face down a tap on its back is soft

// ---------------- Music: dance + sing along, repeat whistles / hums -------
// Needs the INMP441. Watch the serial command "ears" while playing music to tune.
#define ENABLE_EARS        1
// Judged once a second (see "ears" on serial). Measured: music and talking overlap in
// loudness, continuity, tonality and beat - but music stays in one key (fit over ~6 s
// stays high), talking wanders. Music needs 5 of the last 8 seconds to start.
#define MUSIC_MIN_DB       5.0f    // average level over the second, above the quiet room...
#define MUSIC_SOUND_DB     3.0f    // (a 16 ms moment counts as "sound" above this)
#define MUSIC_MIN_CONT     0.75f   // ...sound most of the second (talking has pauses)...
#define MUSIC_MIN_TONAL    1.6f    // ...some clear notes (chroma max/mean; noise ~1.2)...
#define MUSIC_MIN_KEY      0.45f   // ...and staying in one key over ~6 s (talking ~0.3)...
#define MUSIC_MIN_BEAT     0.30f   // ...or a clear beat with a fairly steady key
#define MUSIC_GRACE_MS     8000    // detector dropped out but steady sound goes on: keep dancing this long
#define SING_ALONG         0       // 1 = hum the song's own notes along (off: it only dances)...
#define SING_MIN_BEAT      0.25f   // beat clearer than this: sings on the beat...
#define SING_BARS_ON       2       // ...this many bars singing...
#define SING_BARS_OFF      1       // ...then this many listening
#define HUM_NOTE_MS        700     // weaker beat: hums freely, one note every this long...
#define HUM_PHRASE         3       // ...this many notes, then one pause to keep listening
#define SING_BASE_MIDI     88      // E6: the buzzer sounds best ~1.3-2.7 kHz (it resonates ~2.6/3.6 kHz)
// Bass along: plucks the ROOT of the current chord. Clear beat: on beats 1 and 3;
// weak beat: whenever the chord changes. Only chords of the detected key, and only
// when one chord clearly wins - otherwise it stays quiet.
#define BASS_ALONG         1
#define BASS_SUSTAIN       0.70f   // hold the note this share of the chord (the rest: listening for the next one)
#define BASS_MAX_MS        1400
#define BASS_MARGIN        0.03f   // how clearly the best chord must beat the next one
#define REPEAT_AFTER_ME    0       // 1 = whistle / hum a short tune and it plays it back
// Someone singing / humming also makes it dance: share of the last second spent on held
// notes (a pitch kept within half a semitone for SUNG_HOLD_MS); talking glides, ~0.
#define SUNG_MIN           0.30f
#define SUNG_HOLD_MS       200
#define SING_END_MS        700     // no voice pitch at all this long: singing has stopped
#define REPEAT_START_MS    400     // a steady pitch this long starts listening
#define REPEAT_END_MS      900     // this long without a pitch ends the tune
#define REPEAT_MAX_MS      8000

// ---------------- Memory -------------------------------------------------
#define REMEMBER_MOOD      1       // keep happiness/energy/age in flash across power cycles
#define SAVE_EVERY_MIN     15
