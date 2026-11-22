// =====================================================================
//  KitBit - configuration
//  Everything you might want to tune lives here.
// =====================================================================
#pragma once

// ---------------- Pins (Seeed XIAO ESP32C3) ---------------------------
#define PIN_SDA            D4      // GPIO6
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
#define DISPLAY_ROTATION   0       // 0 = normal, 2 = display mounted upside down
#define EYES_FPS           30
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

// ---------------- MPU6050 axis mapping ----------------------------------
// Body frame of the pet, seen from the front (looking at the screen):
//   RIGHT = towards your right, UP = top of the screen, OUT = out of the screen towards you.
// Pick which sensor axis (0 = X, 1 = Y, 2 = Z) and sign maps to each body axis.
// Default: MPU board lying flat in the base, chip facing up, sensor +X to the right,
// sensor +Y pointing to the back of the pet.
// Check with the serial command "imu": standing upright should show  up ~ +1.00
#define IMU_RIGHT_AXIS     0
#define IMU_RIGHT_SIGN     (+1)
#define IMU_UP_AXIS        2
#define IMU_UP_SIGN        (+1)
#define IMU_OUT_AXIS       1
#define IMU_OUT_SIGN       (-1)

// ---------------- Motion thresholds (in g / deg per second) ----------
#define TAP_JERK_G         0.30f   // sample-to-sample jump that counts as a tap/knock
#define BUMP_JERK_G        1.60f   // anything harder is a startling bump / desk slam
#define TAP_DEBOUNCE_MS    120
#define TAP_WINDOW_MS      450     // taps closer than this are grouped (double / triple tap)
#define HANDLE_GYRO_DPS    25.0f   // rotation that means "someone is holding me"
#define SHAKE_START        1.5f    // shake meter level to start "being shaken"
#define SPIN_DPS           220.0f  // spinning around the vertical axis
#define DIZZY_SHOW         1.6f    // dizziness level that makes the eyes go swirly
#define FREEFALL_G         0.35f
#define LAND_G             2.0f
#define ORIENT_SETTLE_MS   600     // how long a new orientation must hold
#define TILT_GAIN          3.0f    // how strongly the eyes roll towards the low side

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

// ---------------- Temperature feelings (MPU6050 die temperature) -------
// The MPU's internal sensor reads a few degrees warm; correct with the offset.
#define TEMP_FEELINGS      1
#define TEMP_OFFSET_C      (-3.0f)
#define TEMP_HOT_C         30.0f   // sweats above this
#define TEMP_COLD_C        15.0f   // shivers below this

// ---------------- Memory -------------------------------------------------
#define REMEMBER_MOOD      1       // keep happiness/energy/age in flash across power cycles
#define SAVE_EVERY_MIN     15
