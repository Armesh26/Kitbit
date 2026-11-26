// =====================================================================
//  KitBit - an always-on desk pet
//
//  Hardware : Seeed XIAO ESP32C3 + 1.3" SH1106 (or 0.96" SSD1306) 128x64 I2C OLED
//             + MPU6050 (or a clone: MPU6500 / MPU9250 / unknown ID) + passive buzzer
//             (also runs on other ESP32s - change the pins in config.h)
//  Libraries: Adafruit GFX, Adafruit SH110X (or Adafruit SSD1306),
//             FluxGarage RoboEyes, CuteBuzzerSounds
//             (the motion sensor is driven directly, no MPU library needed)
//
//  Eyes are drawn by FluxGarage RoboEyes (Dennis Hoelscher, www.fluxgarage.com),
//  task layout follows the Robo_Lamp_ai sketch (eyes / sound in their own
//  FreeRTOS tasks so animations never stutter).
//
//  What it does (see README.md for the full list):
//   - eyes roll towards whichever side you tilt it
//   - "calibrate" (serial): learns how the sensor is mounted, saved to flash
//   - tap = boop, double tap = pet (hearts), triple tap = it sings
//   - shake / spin it -> dizzy, shake too long -> grumpy
//   - drop it -> screams, then "oof"
//   - upside down -> complains (screen flips), after a while turns into a bat and naps
//   - lay it on its side -> bedtime, face down -> hiding / do-not-disturb, flip back -> peekaboo!
//   - feels you typing through the desk: watches you work, nags you to take
//     breaks, falls asleep when you leave, greets you when you come back
//   - needs: happiness and energy drift over time and change its baseline mood
//   - random quirks: winks, whistles, yawns, sighs, hiccups, sneezes, giggles
//   - remembers its mood and age across power cycles, celebrates every 24h of uptime
//   - battery: munches when you plug it in, burps when full, gets hungry when low,
//     says goodnight and deep-sleeps before the LiPo is over-discharged
//   - "off" (or 5 min face down on battery): deep sleep, move it to wake it up
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <CuteBuzzerSounds.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <soc/usb_serial_jtag_reg.h>
#include "config.h"
#include "ears.h"
#if ENABLE_MIC
#include <ESP_I2S.h>
#endif

#if ENABLE_TOUCH && !SOC_TOUCH_SENSOR_SUPPORTED
#error "This chip has no capacitive touch (e.g. ESP32-C3) - set ENABLE_TOUCH 0 in config.h"
#endif

#if OLED_SH1106
#include <Adafruit_SH110X.h>
typedef Adafruit_SH1106G OledDisplay;
#define OLED_WHITE        SH110X_WHITE
#define OLED_BLACK        SH110X_BLACK
#define OLED_ON_CMD       SH110X_DISPLAYON
#define OLED_OFF_CMD      SH110X_DISPLAYOFF
#define oledBegin()       display.begin(OLED_ADDR, true)
#define oledCommand(c)    display.oled_command(c)
#define oledDim(d)        display.setContrast((d) ? 0x01 : 0xFF)
#else
#include <Adafruit_SSD1306.h>
typedef Adafruit_SSD1306 OledDisplay;
#define OLED_WHITE        SSD1306_WHITE
#define OLED_BLACK        SSD1306_BLACK
#define OLED_ON_CMD       SSD1306_DISPLAYON
#define OLED_OFF_CMD      SSD1306_DISPLAYOFF
#define oledBegin()       display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)
#define oledCommand(c)    display.ssd1306_command(c)
#define oledDim(d)        display.dim(d)
#endif

OledDisplay display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1, I2C_DISPLAY_HZ, I2C_IDLE_HZ);

// RoboEyes defines short macros (N, S, E, W, DEFAULT, ON, OFF, ...), so it is included last.
#include <FluxGarage_RoboEyes.h>
RoboEyes<OledDisplay> eyes(display);

Preferences prefs;

// ---------------------------------------------------------------------
//  Types (declared before any function so Arduino's auto-prototypes compile)
// ---------------------------------------------------------------------
enum CustomSound : int {
  SND_BOOT = 100, SND_YAWN, SND_SNORE, SND_HICCUP, SND_SNEEZE_BUILD, SND_SNEEZE,
  SND_SIGH, SND_GIGGLE, SND_PURR, SND_OOF, SND_WHEE, SND_HELP, SND_WHISTLE,
  SND_SONG_A, SND_SONG_B, SND_PEEKABOO, SND_BREAK, SND_GRUMBLE, SND_CHIRP,
  SND_BIRTHDAY, SND_RELIEF, SND_NOM, SND_BURP, SND_TUMMY, SND_UNPLUG, SND_GOODNIGHT
};

enum BattState : uint8_t { BATT_NONE, BATT_OK, BATT_LOW, BATT_CRITICAL, BATT_CHARGING, BATT_FULL };
enum OffReason : uint8_t { OFF_NONE, OFF_USER, OFF_BATTERY };

struct Note { float hz; int ms; };

enum Overlay : uint8_t { OV_NONE, OV_ZZZ, OV_HEARTS, OV_ALERT, OV_QUESTION, OV_DIZZY, OV_NOTES, OV_BREAK, OV_TEXT };

struct Face {
  uint8_t mood = DEFAULT;
  bool open = true;
  bool sweat = false;
  bool curious = true;
  bool wide = false;
  uint8_t hFlicker = 0;  // 0 = off, else amplitude
  uint8_t vFlicker = 0;
  bool operator==(const Face &o) const {
    return mood == o.mood && open == o.open && sweat == o.sweat && curious == o.curious &&
           wide == o.wide && hFlicker == o.hFlicker && vFlicker == o.vFlicker;
  }
};

struct Vec3 {
  float x = 0, y = 0, z = 0;  // body frame: x = right, y = up, z = out of the screen
  Vec3 operator-(const Vec3 &o) const { Vec3 r; r.x = x - o.x; r.y = y - o.y; r.z = z - o.z; return r; }
  float len() const { return sqrtf(x * x + y * y + z * z); }
};

enum Orient : uint8_t { OR_UPRIGHT, OR_TILTED, OR_UPSIDE_DOWN, OR_LYING_LEFT, OR_LYING_RIGHT, OR_FACE_UP, OR_FACE_DOWN };

struct MotionEvents {
  uint8_t taps = 0;
  float tapSide = 0;
  bool bump = false, freefall = false, landed = false, caught = false;
  bool shakeStart = false, shakeEnd = false;
  uint32_t shakeMs = 0;
  bool pickup = false, putdown = false;
  bool orientChanged = false;
  bool scratchStart = false, scratchEnd = false;
  uint32_t scratchMs = 0;
};

enum Activity : uint8_t { ACT_AWAKE, ACT_DROWSY, ACT_ASLEEP, ACT_HIDING };

// ---------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------
static inline bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }
static inline uint32_t since(uint32_t t) { return millis() - t; }
static inline uint32_t minutes(float m) { return (uint32_t)(m * 60000.0f); }
static inline float frand(float a, float b) { return a + (b - a) * (random(0, 10001) / 10000.0f); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The OLED (eyes task) and the MPU6050 (main loop) share the
// I2C bus, and the eyes' state is read while drawing. One recursive mutex guards both.
SemaphoreHandle_t busMutex;
struct BusLock {
  BusLock() { xSemaphoreTakeRecursive(busMutex, portMAX_DELAY); }
  ~BusLock() { xSemaphoreGiveRecursive(busMutex); }
};

// =====================================================================
//  SOUND
// =====================================================================
#define SND_LIB_RESET 255  // not a CuteBuzzerSounds sound: play() with it only clears the library's stop flag
#define SND_NOTE_FLAG 0x40000000  // id = flag | midi << 16 | ms : one note (midi 0 = rest)

QueueHandle_t soundQueue;
volatile bool soundAbort = false;
volatile bool soundPlaying = false;  // buzzer vibration must not count as desk activity
volatile bool muted = false;
volatile bool hidingQuiet = false;   // face down = do not disturb
uint8_t chattiness = CHATTINESS;

// level 1 = reaction, 2 = idle chatter, 3 = chatter while you are working
void say(int id, uint8_t level = 1, bool urgent = false) {
  if (muted || hidingQuiet || chattiness < level) return;
  if (urgent) {
    xQueueReset(soundQueue);
    soundAbort = true;
    cute.stop();
  }
  xQueueSend(soundQueue, &id, 0);
}

void hush() {
  xQueueReset(soundQueue);
  soundAbort = true;
  cute.stop();
}

// Custom-sound building blocks. They stop early if a more urgent sound comes in.
// Note: CuteBuzzerSounds' bendTones() steps with an int, so every bend below keeps
// freq*(step-1) >= 1 at its low end, otherwise it would never finish.
static void tn(float hz, int ms, int gapMs = 10) { if (!soundAbort) cute._tone(hz, ms, gapMs); }
static void bend(float from, float to, float step, int ms, int gapMs = 1) { if (!soundAbort) cute.bendTones(from, to, step, ms, gapMs); }
static void hold(int ms) { while (ms > 0 && !soundAbort) { vTaskDelay(pdMS_TO_TICKS(10)); ms -= 10; } }

static void tune(const Note *n, size_t count, int gapMs = 25) {
  for (size_t i = 0; i < count && !soundAbort; i++) {
    if (n[i].hz <= 0) hold(n[i].ms);
    else tn(n[i].hz, n[i].ms, gapMs);
  }
}

static const Note SONG_A[] = {
  {NOTE_C6, 120}, {NOTE_E6, 120}, {NOTE_G6, 120}, {NOTE_E6, 120}, {NOTE_F6, 120}, {NOTE_A6, 240},
  {NOTE_G6, 120}, {NOTE_E6, 120}, {NOTE_C6, 120}, {NOTE_D6, 120}, {NOTE_E6, 360}, {0, 80},
  {NOTE_G6, 100}, {NOTE_C7, 300}
};
static const Note SONG_B[] = {
  {NOTE_A5, 150}, {NOTE_C6, 150}, {NOTE_E6, 300}, {NOTE_D6, 150}, {NOTE_C6, 150}, {NOTE_A5, 300},
  {NOTE_G5, 150}, {NOTE_A5, 150}, {NOTE_C6, 450}, {0, 100}, {NOTE_E6, 120}, {NOTE_D6, 120}, {NOTE_E6, 360}
};
// "Happy Birthday" (public domain)
static const Note SONG_BIRTHDAY[] = {
  {NOTE_G5, 180}, {NOTE_G5, 120}, {NOTE_A5, 300}, {NOTE_G5, 300}, {NOTE_C6, 300}, {NOTE_B5, 560},
  {NOTE_G5, 180}, {NOTE_G5, 120}, {NOTE_A5, 300}, {NOTE_G5, 300}, {NOTE_D6, 300}, {NOTE_C6, 560},
  {NOTE_G5, 180}, {NOTE_G5, 120}, {NOTE_G6, 300}, {NOTE_E6, 300}, {NOTE_C6, 300}, {NOTE_B5, 300}, {NOTE_A5, 560},
  {NOTE_F6, 180}, {NOTE_F6, 120}, {NOTE_E6, 300}, {NOTE_C6, 300}, {NOTE_D6, 300}, {NOTE_C6, 700}
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void playCustom(int id) {
  switch (id) {
    case SND_BOOT:
      tn(NOTE_C6, 60, 20); tn(NOTE_E6, 60, 20); tn(NOTE_G6, 60, 20);
      bend(1568, 3136, 1.04, 8);
      break;
    case SND_YAWN:
      bend(440, 880, 1.02, 22, 2); hold(80);
      bend(880, 300, 1.015, 22, 2); tn(280, 120);
      break;
    case SND_SNORE:
      bend(180, 320, 1.02, 30, 4); hold(300);
      bend(500, 200, 1.04, 20, 3);
      break;
    case SND_HICCUP:
      tn(1200, 20, 5); bend(1400, 2400, 1.12, 6);
      break;
    case SND_SNEEZE_BUILD:
      tn(660, 160, 260); tn(740, 180, 320); tn(830, 260, 150);
      break;
    case SND_SNEEZE:
      bend(3500, 900, 1.12, 5); tn(300, 60);
      break;
    case SND_SIGH:
      bend(1000, 520, 1.012, 24, 2);
      break;
    case SND_GIGGLE:
      for (int i = 0; i < 4; i++) { tn(1760, 35, 25); tn(2093, 35, 70); }
      break;
    case SND_PURR:
      for (int i = 0; i < 8; i++) { tn(220, 40, 8); tn(247, 40, 8); }
      break;
    case SND_OOF:
      bend(700, 220, 1.07, 14, 2); tn(196, 90);
      break;
    case SND_WHEE:
      bend(700, 2800, 1.03, 9);
      break;
    case SND_HELP:
      for (int i = 0; i < 2; i++) { tn(1319, 90, 40); tn(988, 160, 180); }
      break;
    case SND_WHISTLE: {
      static const float scale[] = {NOTE_C6, NOTE_D6, NOTE_E6, NOTE_G6, NOTE_A6, NOTE_C7};
      int count = random(4, 7);
      for (int i = 0; i < count; i++) tn(scale[random(0, 6)], random(80, 200), random(20, 60));
      break;
    }
    case SND_SONG_A:   tune(SONG_A, COUNT(SONG_A)); break;
    case SND_SONG_B:   tune(SONG_B, COUNT(SONG_B)); break;
    case SND_BIRTHDAY: tune(SONG_BIRTHDAY, COUNT(SONG_BIRTHDAY), 40); break;
    case SND_PEEKABOO:
      tn(988, 70, 30); bend(1000, 2400, 1.05, 7); tn(2637, 90);
      break;
    case SND_BREAK:
      for (int i = 0; i < 3; i++) { tn(1568, 90, 50); tn(2093, 150, 220); }
      break;
    case SND_GRUMBLE:
      for (int i = 0; i < 3; i++) { bend(320, 200, 1.03, 18, 2); hold(60); }
      break;
    case SND_CHIRP:
      bend(1800, 2700, 1.06, 9);
      break;
    case SND_RELIEF:
      bend(1200, 700, 1.03, 14, 2); tn(1047, 80, 20); tn(1319, 120);
      break;
    case SND_NOM:  // nom nom nom
      for (int i = 0; i < 3; i++) { bend(420, 760, 1.06, 7, 2); hold(110); }
      break;
    case SND_BURP:  // low burp, then a little embarrassed giggle
      bend(330, 140, 1.04, 14, 3); tn(130, 110); hold(220);
      tn(2093, 40, 30); tn(2637, 60);
      break;
    case SND_TUMMY:  // tummy rumble
      for (int i = 0; i < 5; i++) { tn(110, 45, 0); tn(92, 45, 0); }
      hold(120);
      bend(520, 300, 1.03, 16, 2);
      break;
    case SND_UNPLUG:  // pop!
      tn(1600, 15, 20); bend(600, 1900, 1.06, 8);
      break;
    case SND_GOODNIGHT:  // little lullaby going down
      tn(NOTE_E6, 220, 40); tn(NOTE_C6, 220, 40); tn(NOTE_G5, 220, 40); tn(NOTE_C5, 420, 60);
      bend(520, 260, 1.02, 20, 3);
      break;
  }
}

void soundTask(void *) {
  int id;
  for (;;) {
    if (xQueueReceive(soundQueue, &id, portMAX_DELAY) == pdTRUE) {
      soundAbort = false;
      soundPlaying = true;
      if (id & SND_NOTE_FLAG) {  // a single note (singing along / repeating a tune)
        cute.play(SND_LIB_RESET);
        int midi = (id >> 16) & 0xFF, ms = id & 0xFFFF;
        if (midi) tn(440.0f * powf(2.0f, (midi - 69) / 12.0f), ms, 0);
        else hold(ms);
      } else if (id < 100) {
        cute.play(id);
      } else {
        cute.play(SND_LIB_RESET);
        playCustom(id);
      }
      noTone(PIN_BUZZER);
      vTaskDelay(pdMS_TO_TICKS(60));  // let the case stop ringing
      soundPlaying = false;
    }
  }
}

// =====================================================================
//  DISPLAY: eyes + little overlays (Zzz, hearts, !, ?, dizzy stars, notes, break)
// =====================================================================
const char *OVERLAY_NAMES[] = {"none", "zzz", "hearts", "alert", "question", "dizzy", "notes", "break", "text"};
volatile const char *overlayLine1 = "";  // for OV_TEXT (calibration prompts)
volatile const char *overlayLine2 = "";

volatile Overlay overlay = OV_NONE;
volatile uint32_t overlayStart = 0;
volatile uint32_t overlayUntil = 0;  // 0 = until cleared

void showOverlay(Overlay o, uint32_t ms) {
  overlayStart = millis();
  overlayUntil = ms ? millis() + ms : 0;
  overlay = o;
}
void clearOverlay(Overlay o) { if (overlay == o) overlay = OV_NONE; }

volatile uint8_t wantRotation = DISPLAY_ROTATION;
volatile bool wantDisplayOn = true;
volatile bool wantDim = false;

static const uint8_t HEART_BMP[] PROGMEM = {0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18};             // 8x7
static const uint8_t NOTE_BMP[]  PROGMEM = {0x1C, 0x16, 0x12, 0x10, 0x70, 0xF0, 0xF0, 0x60};       // 8x8
static const uint8_t CUP_BMP[]   PROGMEM = {0x24, 0x48, 0x00, 0xFC, 0xFA, 0xFA, 0xFC, 0x78};       // 8x8

// Black "badge" behind an icon so it stays visible on top of the white eyes.
static void icon(int x, int y, const uint8_t *bmp, int w, int h) {
  display.fillRect(x - 1, y - 1, w + 2, h + 2, OLED_BLACK);
  display.drawBitmap(x, y, bmp, w, h, OLED_WHITE);
}
static void glyph(int x, int y, char c, uint8_t size) {
  display.setTextWrap(false);  // never wrap a glyph that touches the edge onto the other side
  display.setTextSize(size);
  display.setTextColor(OLED_WHITE, OLED_BLACK);
  display.setCursor(x, y);
  display.print(c);
}

void drawOverlay() {
  Overlay o = overlay;
  if (o == OV_NONE) return;
  uint32_t now = millis();
  if (overlayUntil && reached(overlayUntil)) { overlay = OV_NONE; return; }
  uint32_t t = now - overlayStart;

  switch (o) {
    case OV_ZZZ:
      for (int i = 0; i < 3; i++) {
        float p = ((t + i * 900) % 2700) / 2700.0f;  // 0..1, rising
        int x = 94 + (int)(p * 20);  // the big Z is 12 px wide: stay left of 116
        int y = 34 - (int)(p * 34);
        glyph(x, y, p > 0.5f ? 'Z' : 'z', p > 0.66f ? 2 : 1);
      }
      break;
    case OV_HEARTS:
      for (int i = 0; i < 2; i++) {
        float p = ((t + i * 700) % 1400) / 1400.0f;
        int y = 56 - (int)(p * 54);
        icon(i == 0 ? 4 + (int)(p * 6) : 116 - (int)(p * 6), y, HEART_BMP, 8, 7);
      }
      break;
    case OV_ALERT:
      if ((t / 200) % 2 == 0) glyph(116, 0, '!', 2);
      break;
    case OV_QUESTION:
      glyph(114, 2 + (int)(2 * sinf(t * 0.008f)), '?', 2);
      break;
    case OV_DIZZY:
      for (int i = 0; i < 3; i++) {
        float a = t * 0.009f + i * 2.094f;
        int x = 64 + (int)(30 * cosf(a));
        int y = 5 + (int)(4 * sinf(a));
        display.fillCircle(x, y, 3, OLED_BLACK);
        display.fillCircle(x, y, 2, OLED_WHITE);
      }
      break;
    case OV_NOTES:
      for (int i = 0; i < 2; i++) {
        int y = 4 + (int)(4 * sinf(t * 0.012f + i * 3.1f));
        icon(i == 0 ? 3 : 117, y, NOTE_BMP, 8, 8);
      }
      break;
    case OV_TEXT:
      display.fillRect(0, 43 - SCREEN_HIDDEN_BOTTOM, SCREEN_WIDTH, 21 + SCREEN_HIDDEN_BOTTOM, OLED_BLACK);
      display.setTextSize(1);
      display.setTextColor(OLED_WHITE, OLED_BLACK);
      display.setCursor(1, 45 - SCREEN_HIDDEN_BOTTOM);
      display.print((const char *)overlayLine1);
      display.setCursor(1, 55 - SCREEN_HIDDEN_BOTTOM);
      display.print((const char *)overlayLine2);
      break;
    case OV_BREAK:
      if ((t / 600) % 2 == 0) {
        icon(2, 2, CUP_BMP, 8, 8);
        display.setTextSize(1);
        display.setTextColor(OLED_WHITE, OLED_BLACK);
        display.setCursor(13, 2);
        display.print(F("break?"));
      }
      break;
    default:
      break;
  }
}

// ---- battery badge: small battery at the bottom centre, between the eyes ----
const char *BATT_NAMES[] = {"none", "ok", "low", "critical", "charging", "full"};
volatile BattState battState = BATT_NONE;
volatile uint8_t battPct = 100;
volatile bool battSave = false;          // critical: dim the screen to last longer
volatile uint32_t battBadgeUntil = 0;    // also show it while on battery until then
static const uint8_t BOLT_BMP[] PROGMEM = {0x30, 0x60, 0xC0, 0xF8, 0x30, 0x60, 0x40, 0x80};  // 5x8

void drawBattBadge() {
  BattState s = battState;
  if (s == BATT_NONE || overlay == OV_TEXT) return;
  if (s == BATT_OK && reached(battBadgeUntil)) return;
  uint32_t t = millis();
  const int x = 57, y = 56 - SCREEN_HIDDEN_BOTTOM;  // 14x8 body + 2px nub, above the part the case covers
  int fill;  // 0..10 px
  switch (s) {
    case BATT_CHARGING: fill = ((t / 350) % 5) * 10 / 4; break;  // filling up, over and over
    case BATT_FULL:     fill = 10; break;
    case BATT_LOW:      if ((t / 700) % 2) return; fill = 2; break;
    case BATT_CRITICAL: if ((t / 250) % 2) return; fill = 0; break;
    default:            fill = battPct / 10; break;
  }
  display.fillRect(x - 2, y - 2, 20, 10, OLED_BLACK);
  display.drawRect(x, y, 14, 8, OLED_WHITE);
  display.fillRect(x + 14, y + 2, 2, 4, OLED_WHITE);
  if (fill) display.fillRect(x + 2, y + 2, fill, 4, OLED_WHITE);
  if (s == BATT_CHARGING) icon(x - 8, y, BOLT_BMP, 5, 8);
  if (s == BATT_CRITICAL) glyph(x + 19, y, '!', 1);
  if (s == BATT_FULL && (t / 300) % 8 == 0) {  // sparkle
    display.fillRect(x + 17, y - 4, 5, 5, OLED_BLACK);
    display.drawFastHLine(x + 17, y - 2, 5, OLED_WHITE);
    display.drawFastVLine(x + 19, y - 4, 5, OLED_WHITE);
  }
}

// Clamp where the eyes are heading so they never leave the visible screen: curious eyes
// grow 8 px when looking sideways, wide eyes are taller, wobbles add a few px.
static void keepEyesOnScreen() {
  int maxX = max(EYE_MARGIN, eyes.getScreenConstraint_X() - EYE_MARGIN);
  int maxY = max(EYE_MARGIN, eyes.getScreenConstraint_Y() - max(EYE_MARGIN, SCREEN_HIDDEN_BOTTOM));
  eyes.eyeLxNext = constrain(eyes.eyeLxNext, EYE_MARGIN, maxX);
  eyes.eyeLyNext = constrain(eyes.eyeLyNext, EYE_MARGIN, maxY);
}

void eyesTask(void *) {
  uint8_t rotation = DISPLAY_ROTATION;
  bool on = true, dimmed = false;
  const TickType_t period = pdMS_TO_TICKS(1000 / EYES_FPS);
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    {
      BusLock lock;
      if (wantRotation != rotation) { rotation = wantRotation; display.setRotation(rotation); }
      if (wantDisplayOn != on) { on = wantDisplayOn; oledCommand(on ? OLED_ON_CMD : OLED_OFF_CMD); }
      bool dim = wantDim || battSave;
      if (dim != dimmed) { dimmed = dim; oledDim(dimmed); }
      if (on) {
        keepEyesOnScreen();
        eyes.drawEyes();   // autoDisplayEnabled is off: this only draws into the buffer
        drawOverlay();
        drawBattBadge();
        display.display();
      }
    }
    // Always give the bus back for a moment so the motion sensor can be read.
    TickType_t now = xTaskGetTickCount();
    if (now - last < period) vTaskDelayUntil(&last, period);
    else { last = now; vTaskDelay(1); }
  }
}

// =====================================================================
//  FACE + GAZE
// =====================================================================

Face shown;
bool faceDirty = true;
uint32_t reactionUntil = 0;

void showFace(const Face &f) {
  if (!faceDirty && f == shown) return;
  BusLock lock;
  eyes.setMood(f.mood);
  eyes.setSweat(f.sweat);
  eyes.setCuriosity(f.curious);
  eyes.setHFlicker(f.hFlicker > 0, f.hFlicker);
  eyes.setVFlicker(f.vFlicker > 0, f.vFlicker);
  if (f.open) {
    uint8_t h = f.wide ? EYE_H + EYE_WIDE_EXTRA : EYE_H;
    if (faceDirty || f.wide != shown.wide || !shown.open) eyes.setHeight(h, h);
    if (faceDirty || !shown.open) { eyes.open(); eyes.setAutoblinker(ON, 3, 3); }
  } else if (faceDirty || shown.open) {
    eyes.setAutoblinker(OFF);
    eyes.setHeight(EYE_H, EYE_H);
    eyes.close();
  }
  shown = f;
  faceDirty = false;
}

// Show a face for a while; afterwards the pet's baseline mood takes over again.
void react(const Face &f, uint32_t ms) {
  showFace(f);
  reactionUntil = millis() + ms;
}
bool reacting() { return !reached(reactionUntil); }

Face faceWith(uint8_t mood) { Face f; f.mood = mood; return f; }

bool idleGaze = false;
uint8_t idleGazeInterval = 0;
uint32_t gazeHoldUntil = 0;

void setIdleGaze(bool on, uint8_t interval = 2, uint8_t variation = 3) {
  if (on == idleGaze && (!on || interval == idleGazeInterval)) return;
  BusLock lock;
  eyes.setIdleMode(on, interval, variation);
  idleGaze = on;
  idleGazeInterval = interval;
}

// x: -1 (left) .. 1 (right), y: -1 (up) .. 1 (down)
void lookAt(float x, float y) {
  setIdleGaze(false);
  BusLock lock;
  eyes.eyeLxNext = (int)((clampf(x, -1, 1) + 1) * 0.5f * eyes.getScreenConstraint_X());
  eyes.eyeLyNext = (int)((clampf(y, -1, 1) + 1) * 0.5f * eyes.getScreenConstraint_Y());
}
void glance(float x, float y, uint32_t ms) {
  lookAt(x, y);
  gazeHoldUntil = millis() + ms;
}
void blinkEyes(bool left = true, bool right = true) { BusLock lock; eyes.blink(left, right); }
void laughEyes() { BusLock lock; eyes.anim_laugh(); }
void confusedEyes() { BusLock lock; eyes.anim_confused(); }

// =====================================================================
//  MOTION (MPU6050)
// =====================================================================

// =====================================================================
//  MICROPHONE: hears the sharp click of a finger on the case
// =====================================================================
#if ENABLE_MIC
I2SClass i2s;
#endif
volatile bool micOk = false;
volatile bool micPauseReq = false, micPaused = false;  // face-down power save switches the mic off
Ears ears;  // music + whistle / hum (ears.h), fed by the mic task
volatile uint32_t micClickCount = 0;
volatile uint32_t micClickTimes[8];  volatile float micClickPeaks[8];  // last 8 clicks, exact time
volatile float micBg = 0, micPeakMax = 0;  // for the "mic" serial stream
volatile float micPeakDbg = 0;              // loudest chunk since the last tapdebug line
volatile float micPeakWin = 0;              // loudest chunk in the current 100 ms scratch window
bool scratching = false;  uint32_t scratchStartMs = 0, scratchEndMs = 0;
uint16_t scratchBitsNow = 0;
bool scratchLooking() { return __builtin_popcount(scratchBitsNow & 0x1F) >= 3; }  // 3 of the last 5 windows scratchy
bool micStream = false;
bool earsStream = false;  // "ears": music / pitch analysis twice a second
bool tapDebug = false;  // "tapdebug": raw tap data at 100 Hz for tools/tap_test.py
volatile uint32_t audioDumpUntil = 0;  // "audiodump <s>": raw mic audio over USB for tools/audio_dump.py

#if ENABLE_MIC
void micTask(void *) {
  static int32_t buf[128];  // 8 ms at 16 kHz
  float prevX = 0, hp = 0, prevHp = 0, hf = 0, bg = 0;
  // Second high-pass at ~1.5 kHz: taps and fingernail scratches are sharp and high,
  // voices, footsteps and desk thuds are mostly low.
  static const float HF_A = 1.0f / (1.0f + 2.0f * PI * 1500.0f / 16000.0f);
  bool armed = true, checked = false;
  uint32_t calmSince = 0, lastSound = 0, good = 0, total = 0, start = millis();
  for (;;) {
    if (micPauseReq) {  // power save: stop the clock, the INMP441 then sleeps by itself
      if (!micPaused) { i2s.end(); micPaused = true; }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (micPaused) {
      micStartI2S();
      ears.reset();
      micPaused = false;
      armed = true;
      calmSince = 0;
    }
    size_t n = i2s.readBytes((char *)buf, sizeof buf) / 4;
    if (!n) { vTaskDelay(1); continue; }
    uint32_t now = millis();
    if (!checked) {  // the first second: is a mic actually talking to us?
      for (size_t i = 0; i < n; i++) { total++; if (buf[i] != 0 && buf[i] != -1) good++; }
      if (now - start > 1000) {
        checked = true;
        micOk = good > total / 2;
        Serial.println(micOk ? F("Microphone OK - taps are counted by ear") : F("Microphone not answering - taps use the IMU only"));
        if (!micOk) { i2s.end(); vTaskDelete(NULL); }
      }
      continue;
    }
    if (audioDumpUntil) {  // frames: "AUD!" + uint16 count + count x 24-bit little-endian samples
      if ((int32_t)(now - audioDumpUntil) >= 0) {
        audioDumpUntil = 0;
      } else {
        static uint8_t frame[6 + 128 * 3];
        memcpy(frame, "AUD!", 4);
        frame[4] = n & 0xFF;
        frame[5] = n >> 8;
        for (size_t i = 0; i < n; i++) {
          int32_t v = buf[i] >> 8;
          frame[6 + i * 3] = v;
          frame[7 + i * 3] = v >> 8;
          frame[8 + i * 3] = v >> 16;
        }
        Serial.write(frame, 6 + n * 3);  // one write: other tasks' prints can't land inside a frame
      }
    }
    if (soundPlaying) lastSound = now;
    bool speakerQuiet = now - lastSound > 150;  // never hear our own buzzer as a tap
    for (size_t j = 0; j < n; j += 32) {        // 2 ms chunks
      float peak = 0;
      for (size_t k = j; k < j + 32 && k < n; k++) {
#if ENABLE_EARS
        ears.feed(buf[k] >> 8, !speakerQuiet, now);
#endif
        float x = (float)(buf[k] >> 8);         // 24-bit sample
        hp = x - prevX + 0.95f * hp;            // high-pass: drop DC and rumble
        prevX = x;
        hf = HF_A * (hf + hp - prevHp);         // keep only the high, sharp part
        prevHp = hp;
        if (fabsf(hf) > peak) peak = fabsf(hf);
      }
      if (bg <= 0) bg = peak;
      if (peak > micPeakMax) micPeakMax = peak;
      if (peak > micPeakDbg) micPeakDbg = peak;
      if (peak > micPeakWin) micPeakWin = peak;
      float thresh = max((float)MIC_CLICK_MIN, bg * MIC_CLICK_RATIO);
      if (armed && speakerQuiet && peak > thresh) {
        armed = false;
        uint32_t i = micClickCount % 8;
        micClickTimes[i] = now - (uint32_t)((n - j) / 16);  // 16 samples per ms
        micClickPeaks[i] = peak;
        micClickCount = micClickCount + 1;
      } else if (!armed) {  // one click = one count: wait until it's calm again for 40 ms
        if (peak < bg * 2.5f + MIC_CLICK_MIN * 0.2f) {
          if (!calmSince) calmSince = now;
          if (now - calmSince > 40) { armed = true; calmSince = 0; }
        } else {
          calmSince = 0;
        }
      }
      bg += (peak - bg) * (peak < bg * 3 ? 0.005f : 0.0002f);  // background follows the room, not the clicks
    }
    micBg = bg;
  }
}
#endif

bool micStartI2S() {
#if ENABLE_MIC
  i2s.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD);
  return i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
#else
  return false;
#endif
}

void micBegin() {
#if ENABLE_MIC && TAP_SOURCE > 0
  if (!micStartI2S()) {
    Serial.println(F("Microphone: I2S failed to start - taps use the IMU only"));
    return;
  }
  // Above the eyes task: tiny work per block, and its click times must be on time.
  ears.begin();
  xTaskCreatePinnedToCore(micTask, "mic", 8192, NULL, 3, NULL, 0);
#endif
}

const char *ORIENT_NAMES[] = {"upright", "tilted", "upside-down", "lying-left", "lying-right", "face-up", "face-down"};

bool imuOk = false;
Vec3 acc, grav, gyr, dyn;
float accMag = 1, jerk = 0, gyroMag = 0;
float vib = 0, vibFloor = 0.01f;
uint32_t lastImuMs = 0;
bool imuRestart = true;          // re-seed the filters (boot, after calibration)

// Mounting: rows are the pet's right / up / out-of-screen axes expressed in the
// sensor's frame. Comes from "calibrate" (saved in flash) or the config.h mapping.
float imuRot[3][3];
float gyroBias[3] = {0, 0, 0};   // deg/s, sensor frame (clones often have a big one)
bool imuCalibrated = false;
float rawAcc[3], rawGyro[3];     // last sample in the sensor frame (gyro without bias correction)

bool handling = false;  float handleScore = 0;
// "On a surface" vs "in a hand": on something solid the pet doesn't rotate at all,
// in a hand there is always a little tremor. Decides whether a ringing jolt can be a slam.
bool onSurface = false;  float gyroAvg = 0;  uint32_t pickupMs = 0;
bool hfReset = true;  // restart the IMU high-pass filter (after a FIFO reset)
bool shaking = false;   float shakeMeter = 0;  uint32_t shakeStartMs = 0;
float dizzy = 0;
bool falling = false;   uint8_t lowGCount = 0, normalCount = 0;  uint32_t fallStartMs = 0;
uint8_t tapCount = 0;   float tapSide = 0;  uint32_t lastTapMs = 0, lastTapCandMs = 0;
// The sensor fills its FIFO at 250 Hz; every step we look at all new samples for the biggest kick.
bool fifoOk = true;  uint16_t fifoEmptySteps = 0;
float tapPeak = 0;  // biggest kick since the last "imu" line
uint32_t deskActiveUntil = 0;
Orient orient = OR_UPRIGHT, prevOrient = OR_UPRIGHT, orientCand = OR_UPRIGHT;
uint32_t orientCandSince = 0;
MotionEvents ev;


static Vec3 toBody(const float s[3]) {
  Vec3 v;
  v.x = imuRot[0][0] * s[0] + imuRot[0][1] * s[1] + imuRot[0][2] * s[2];
  v.y = imuRot[1][0] * s[0] + imuRot[1][1] * s[1] + imuRot[1][2] * s[2];
  v.z = imuRot[2][0] * s[0] + imuRot[2][1] * s[1] + imuRot[2][2] * s[2];
  return v;
}

void imuDefaultMounting() {
  memset(imuRot, 0, sizeof(imuRot));
  imuRot[0][IMU_RIGHT_AXIS] = IMU_RIGHT_SIGN;
  imuRot[1][IMU_UP_AXIS] = IMU_UP_SIGN;
  imuRot[2][IMU_OUT_AXIS] = IMU_OUT_SIGN;
}

void loadMounting() {
  imuDefaultMounting();
  prefs.begin("kitbit-imu", true);
  if (prefs.getBool("ok", false) &&
      prefs.getBytes("rot", imuRot, sizeof(imuRot)) == sizeof(imuRot)) {
    prefs.getBytes("gb", gyroBias, sizeof(gyroBias));
    imuCalibrated = true;
  } else {
    imuDefaultMounting();
  }
  prefs.end();
  Serial.println(imuCalibrated ? F("IMU mounting: calibrated (from flash)")
                               : F("IMU mounting: config.h defaults - type 'calibrate' or tap 6 times quickly"));
}

void saveMounting() {
  prefs.begin("kitbit-imu", false);
  prefs.putBytes("rot", imuRot, sizeof(imuRot));
  prefs.putBytes("gb", gyroBias, sizeof(gyroBias));
  prefs.putBool("ok", true);
  prefs.end();
}

void clearMounting() {
  prefs.begin("kitbit-imu", false);
  prefs.clear();
  prefs.end();
  imuCalibrated = false;
  imuDefaultMounting();
  imuRestart = true;
}

static Orient classify(const Vec3 &g) {
  float m = g.len();
  if (m < 0.5f) return OR_TILTED;
  float x = g.x / m, y = g.y / m, z = g.z / m;
  if (y > 0.80f) return OR_UPRIGHT;
  if (y < -0.80f) return OR_UPSIDE_DOWN;
  if (x > 0.80f) return OR_LYING_LEFT;    // right side points up -> lying on its left side
  if (x < -0.80f) return OR_LYING_RIGHT;
  if (z > 0.80f) return OR_FACE_UP;
  if (z < -0.80f) return OR_FACE_DOWN;
  return OR_TILTED;
}

// ---- Minimal register-level driver -----------------------------------
// Many "MPU6050" boards carry a clone or an MPU6500/MPU9250 that reports a
// different WHO_AM_I, which makes most libraries refuse it. The registers used
// here are identical on all of them, so any chip that answers is accepted.
uint8_t imuAddr = 0x68;
uint8_t imuWho = 0;

static bool imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool imuRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(imuAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool imuIs6500Family() {  // MPU6500 / 9250 / 9255 / ICM-20608 style
  return imuWho == 0x70 || imuWho == 0x71 || imuWho == 0x73 || imuWho == 0x74 || imuWho == 0x75 || imuWho == 0xAF;
}

void i2cScan() {
  Serial.print(F("I2C devices:"));
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();
}

bool imuBegin() {
  const uint8_t addrs[] = {0x68, 0x69};
  bool found = false;
  for (uint8_t a : addrs) {
    imuAddr = a;
    if (imuRead(0x75, &imuWho, 1)) { found = true; break; }  // WHO_AM_I
  }
  if (!found) return false;
  Serial.printf("Motion sensor at 0x%02X, WHO_AM_I=0x%02X (%s)\n", imuAddr, imuWho,
                imuWho == 0x68 ? "MPU6050" : imuIs6500Family() ? "MPU6500 family" : "unknown clone - trying anyway");

  imuWrite(0x6B, 0x80);  // PWR_MGMT_1: reset
  delay(100);
  imuWrite(0x6B, 0x01);  // wake up, clock from gyro PLL
  delay(50);
  imuWrite(0x19, 0x03);  // SMPLRT_DIV: 1 kHz / (1+3) = 250 Hz (into the FIFO: 1.5 kB/s, 512 B lasts 340 ms)
  // CONFIG: DLPF ~184 Hz keeps the sharp spike of a tap. 6500 family: FIFO_MODE = stop when
  // full instead of overwriting, which scrambles the byte order on these chips.
  imuWrite(0x1A, imuWho != 0x68 ? 0x41 : 0x01);
  imuWrite(0x1B, 0x08);  // GYRO_CONFIG: +-500 dps
  imuWrite(0x1C, 0x08);  // ACCEL_CONFIG: +-4 g
  if (imuWho != 0x68) imuWrite(0x1D, 0x01);  // ACCEL_CONFIG2 (6500 family): accel DLPF ~218 Hz
  imuFifoReset();
  delay(20);

  // Sanity check: a living sensor sees ~1 g of gravity.
  uint8_t b[6];
  if (!imuRead(0x3B, b, 6)) return false;
  float x = (int16_t)(b[0] << 8 | b[1]) / 8192.0f;
  float y = (int16_t)(b[2] << 8 | b[3]) / 8192.0f;
  float z = (int16_t)(b[4] << 8 | b[5]) / 8192.0f;
  float g = sqrtf(x * x + y * y + z * z);
  Serial.printf("Gravity check: %.2f g %s\n", g, (g > 0.6f && g < 1.4f) ? "(ok)" : "(odd - sensor may be dead or very fake)");
  return g > 0.2f;
}

// accel in g, gyro in deg/s (sensor frame)
static bool imuSample(float a[3], float g[3]) {
  uint8_t b[14];
  if (!imuRead(0x3B, b, 14)) return false;  // ACCEL_XOUT_H .. GYRO_ZOUT_L
  for (int i = 0; i < 3; i++) {
    a[i] = (int16_t)(b[i * 2] << 8 | b[i * 2 + 1]) / 8192.0f;      // +-4 g
    g[i] = (int16_t)(b[8 + i * 2] << 8 | b[9 + i * 2]) / 65.5f;    // +-500 dps
  }
  return true;
}

// At boot: if the pet is sitting still, measure the gyro zero-offset (~1 s).
void imuMeasureGyroBias() {
  float sum[3] = {0, 0, 0}, aMin = 10, aMax = 0;
  int n = 0;
  for (int i = 0; i < 100; i++) {
    float a[3], g[3];
    if (imuSample(a, g)) {
      float m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
      aMin = min(aMin, m);
      aMax = max(aMax, m);
      for (int k = 0; k < 3; k++) sum[k] += g[k];
      n++;
    }
    delay(10);
  }
  if (n > 50 && aMax - aMin < 0.05f) {
    for (int k = 0; k < 3; k++) gyroBias[k] = sum[k] / n;
    Serial.printf("Gyro offset measured: %.1f %.1f %.1f dps\n", gyroBias[0], gyroBias[1], gyroBias[2]);
  } else {
    Serial.println(F("Moved during boot - using saved gyro offset"));
  }
}

void imuFifoReset() {
  hfReset = true;
  imuWrite(0x6A, 0x00);  // USER_CTRL: FIFO off
  imuWrite(0x23, 0x08);  // FIFO_EN: accelerometer only
  imuWrite(0x6A, 0x04);  // reset FIFO
  delay(1);
  imuWrite(0x6A, 0x40);  // FIFO on
}

// Recent jolts of the pet with their exact time (from the sample's place in the FIFO),
// so they can be matched with mic clicks even when the bus was busy with the screen.
// Each jolt is a "hit": after SLAM_RING_TO_MS it is judged a tap or a desk slam.
const uint8_t HIT_PENDING = 0, HIT_TAP = 1, HIT_SLAM = 2;
uint32_t kickMs[8] = {0};  float kickSide[8] = {0};  uint8_t kickVerdict[8] = {0};  uint8_t kickIdx = 0;
float kickPeak[8] = {0};
// Everything that happened (mic clicks, jolts), for telling typing from tapping.
uint32_t actTimes[32];  uint8_t actIdx = 0;
float bgJolt = 0.02f;  // typical jolt of whatever is going on around it (keystrokes on a laptop...)
uint32_t typingUntil = 0;
uint32_t busyUntil = 0;  // typing was seen recently: stricter tap checks until then
// One touch makes a mic click AND a jolt: anything within 120 ms of the last event is the same touch.
void logActivity(uint32_t t) {
  uint32_t last = actTimes[(actIdx + 31) % 32];
  if (last && (uint32_t)abs((int32_t)(t - last)) < 120) return;
  actTimes[actIdx] = t;
  actIdx = (actIdx + 1) % 32;
}
int activityBetween(uint32_t from, uint32_t to, uint32_t *first = nullptr, uint32_t *lastT = nullptr) {
  int n = 0;
  for (int i = 0; i < 32; i++) {
    uint32_t t = actTimes[i];
    if (t && (int32_t)(t - from) >= 0 && (int32_t)(t - to) < 0) {
      if (first && (!n || (int32_t)(t - *first) < 0)) *first = t;
      if (lastT && (!n || (int32_t)(t - *lastT) > 0)) *lastT = t;
      n++;
    }
  }
  return n;
}
bool scratchLooking();  // (scratch detection, further down)
bool slamPending = false;
uint32_t slamAt = 0;
// Plain jolt log for the face-down "switch off" double tap: face down on the desk every
// tap rings like a slam, so the normal tap logic (mic click + non-ringing jolt) can't be used.
uint32_t joltLog[4];  volatile uint32_t joltLogCount = 0;
bool offTest = false;  // "offtest": log the face-down double tap instead of switching off

// Drains the whole FIFO (250 Hz) and returns the biggest kick away from gravity (g),
// with its direction in the sensor frame. Returns -1 if nothing was read.
float imuFifoPeak(float dir[3]) {
  uint8_t st = 0, c[2];
  if (imuRead(0x3A, &st, 1) && (st & 0x10)) { imuFifoReset(); return -1; }  // overflowed: samples lost
  if (!imuRead(0x72, c, 2)) return -1;
  uint16_t count = (uint16_t)(c[0] << 8 | c[1]);
  if (count < 6) return -1;
  if (count % 6) { imuFifoReset(); return -1; }  // out of step
  uint16_t total = count / 6, done = 0;
  uint32_t now = millis();
  static uint32_t lastKickAt = 0;
  float peak = 0;
  uint8_t b[120];
  while (done < total) {
    uint8_t n = min<uint16_t>(total - done, 20);  // 20 samples = 120 bytes, fits the I2C buffer
    if (!imuRead(0x74, b, n * 6)) { imuFifoReset(); return peak > 0 ? peak : -1; }
    uint8_t bad = 0;
    for (uint8_t i = 0; i < n; i++) {
      float s[3];
      for (int k = 0; k < 3; k++) s[k] = (int16_t)(b[i * 6 + k * 2] << 8 | b[i * 6 + k * 2 + 1]) / 8192.0f;
      float mag = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
      if (mag < 0.3f || mag > 3.9f) {  // empty / scrambled sample: a tap on a desk pet is never like that
        if (++bad > 2) { imuFifoReset(); return peak > 0 ? peak : -1; }
        continue;
      }
      // High-pass (~HF_CUTOFF_HZ): keep the sharp spike of a tap, drop gravity, tilting
      // and the slow movements of a hand.
      static float hfPrev[3], hfOut[3];
      static const float A = 1.0f / (1.0f + 2.0f * PI * HF_CUTOFF_HZ * 0.004f);
      if (hfReset) { memcpy(hfPrev, s, sizeof s); memset(hfOut, 0, sizeof hfOut); hfReset = false; }
      float d[3], m = 0;
      for (int k = 0; k < 3; k++) {
        hfOut[k] = A * (hfOut[k] + s[k] - hfPrev[k]);
        hfPrev[k] = s[k];
        d[k] = hfOut[k];
        m += d[k] * d[k];
      }
      m = sqrtf(m);
      if (m > peak) { peak = m; memcpy(dir, d, sizeof d); }
      uint32_t t = now - (uint32_t)(total - 1 - (done + i)) * 4;  // 4 ms per sample
      // Tap or slam: a finger tap is one short jolt, a slam on the desk makes the
      // pet keep shaking for ~80 ms while the desk rings.
      static bool hitOpen = false, hitRings = false;
      static uint32_t hitStart = 0, lastAbove = 0;
      static float hitPeak = 0;
      static uint8_t hitSlot = 0;
      if (m > TAP_CONFIRM_G) {
        if (!hitOpen && t - lastAbove > 60) {
          hitOpen = true;
          hitRings = false;
          hitStart = t;
          hitPeak = m;
          hitSlot = kickIdx;
          kickMs[kickIdx] = t;
          kickSide[kickIdx] = toBody(d).x;
          kickPeak[kickIdx] = m;
          logActivity(t);
          kickVerdict[kickIdx] = HIT_PENDING;
          kickIdx = (kickIdx + 1) % 8;
        } else if (hitOpen) {
          if (m > hitPeak) { hitPeak = m; kickSide[hitSlot] = toBody(d).x; }
          if (m > SLAM_RING_G && t - hitStart >= SLAM_RING_FROM_MS) hitRings = true;
        }
        lastAbove = t;
      }
      static bool joltArmed = true;
      static uint32_t joltCalm = 0, joltSound = 0;
      if (soundPlaying) joltSound = t;  // our own buzzer shakes the case: not a tap
      if (joltArmed && m > HIDE_TAP_G && t - joltSound > 150) {
        joltArmed = false;
        joltCalm = 0;
        joltLog[joltLogCount % 4] = t;
        joltLogCount = joltLogCount + 1;
        if (tapDebug) Serial.printf("E,%lu,jolt,%.2f\n", (unsigned long)t, m);
      } else if (!joltArmed) {  // re-arm once it has been calm for 60 ms (the desk may ring)
        if (m < HIDE_TAP_G * 0.5f) {
          if (!joltCalm) joltCalm = t;
          if (t - joltCalm >= 60) joltArmed = true;
        } else {
          joltCalm = 0;
        }
      }
      if (hitOpen && t - hitStart > SLAM_RING_TO_MS) {
        hitOpen = false;
        bool slam = hitRings && hitPeak > SLAM_MIN_G && onSurface;  // in a hand there's no desk to slam
        kickVerdict[hitSlot] = slam ? HIT_SLAM : HIT_TAP;
        kickPeak[hitSlot] = hitPeak;
        bgJolt = clampf(bgJolt + (hitPeak - bgJolt) * 0.1f, 0.02f, 2.0f);  // background level of this surface
        if (slam && !soundPlaying) { slamPending = true; slamAt = hitStart; }
        if (tapDebug) Serial.printf("H,%lu,%s,%.2f\n", (unsigned long)hitStart, slam ? "slam" : "tap", hitPeak);
      }
      if (tapDebug && m > TAP_CONFIRM_G && t - lastKickAt > 30) {
        lastKickAt = t;
        {  // K,ms,size g,body right,up,out (direction of the jolt)
          Vec3 bd = toBody(d);
          Serial.printf("K,%lu,%.3f,%.3f,%.3f,%.3f\n", (unsigned long)t, m, bd.x, bd.y, bd.z);
        }
      }
    }
    done += n;
  }
  return peak;
}

// Was there a hit close to time t? Returns HIT_TAP / HIT_SLAM (and uses it up, so one
// hit confirms one click), -1 if that hit isn't judged yet, 0 if there was none.
int takeKickNear(uint32_t t, float *side, float *peak = nullptr) {
  for (int i = 0; i < 8; i++) {
    if (kickMs[i] && (uint32_t)abs((int32_t)(kickMs[i] - t)) <= TAP_MATCH_MS) {
      if (kickVerdict[i] == HIT_PENDING) return -1;
      *side = kickSide[i];
      if (peak) *peak = kickPeak[i];
      kickMs[i] = 0;
      return kickVerdict[i];
    }
  }
  return 0;
}

void imuStep() {
  float sa[3], sg[3];
  {
    BusLock lock;
    if (!imuSample(sa, sg)) return;
  }
  float kickDir[3] = {0, 0, 0};
  float kick = -1;
  if (fifoOk) {
    BusLock lock;
    kick = imuFifoPeak(kickDir);
    if (kick < 0 && ++fifoEmptySteps > 200) {  // FIFO never delivers on this chip: fall back
      fifoOk = false;
      Serial.println(F("[imu] FIFO not working - taps use plain samples (less sensitive)"));
    }
    if (kick >= 0) fifoEmptySteps = 0;
  }
  uint32_t now = millis();
  float dt = clampf((now - lastImuMs) / 1000.0f, 0.001f, 0.1f);
  lastImuMs = now;

  memcpy(rawAcc, sa, sizeof(rawAcc));
  memcpy(rawGyro, sg, sizeof(rawGyro));
  for (int i = 0; i < 3; i++) sg[i] -= gyroBias[i];

  Vec3 prev = acc;
  acc = toBody(sa);
  gyr = toBody(sg);

  if (imuRestart) {
    imuRestart = false;
    prev = grav = acc;
  }

  // Gravity = low-passed acceleration; whatever is left is the pet being moved.
  grav.x += (acc.x - grav.x) * 0.04f;
  grav.y += (acc.y - grav.y) * 0.04f;
  grav.z += (acc.z - grav.z) * 0.04f;
  dyn = acc - grav;
  float dynMag = dyn.len();
  accMag = acc.len();
  jerk = (acc - prev).len();
  gyroMag = gyr.len();

  // Gyro zero-offset drifts with temperature: keep trimming it while sitting still.
  if (jerk < 0.04f && dynMag < 0.04f && fabsf(accMag - 1.0f) < 0.08f && gyroMag < 8.0f) {
    for (int i = 0; i < 3; i++) gyroBias[i] += (sg[i]) * 0.002f;
  }

  // --- being held / carried ---
  bool moving = gyroMag > HANDLE_GYRO_DPS || dynMag > 0.12f;
  handleScore += ((moving ? 1.0f : 0.0f) - handleScore) * 0.05f;
  if (!handling && handleScore > 0.45f) { handling = true; ev.pickup = true; tapCount = 0; pickupMs = now; }
  else if (handling && handleScore < 0.08f) { handling = false; ev.putdown = true; }

  // --- on a surface or in a hand? ---
  gyroAvg += (gyroMag - gyroAvg) * 0.05f;
  static uint32_t calmSince = 0, restlessSince = 0;
  static const float uprightCos = cosf(SURFACE_TILT_DEG * DEG_TO_RAD);
  float gl = grav.len();
  bool standing = gl > 0.5f && grav.y / gl > uprightCos;  // exactly its desk pose
  if (!standing) { onSurface = false; calmSince = 0; }
  else if (gyroAvg < SURFACE_GYRO_DPS) { restlessSince = 0; if (!calmSince) calmSince = now; }
  else { calmSince = 0; if (!restlessSince) restlessSince = now; }
  if (!onSurface && calmSince && now - calmSince > 1000) onSurface = true;
  // A tap on the desk rotates the pet for a moment: only leave "surface" if it lasts.
  if (onSurface && gyroAvg > SURFACE_GYRO_DPS * 2 && restlessSince && now - restlessSince > 300) onSurface = false;

  // --- shaking ---
  shakeMeter = shakeMeter * 0.97f + (dynMag > SHAKE_G ? dynMag * 0.05f : 0.0f);
  if (!shaking && shakeMeter > SHAKE_START) { shaking = true; shakeStartMs = now; ev.shakeStart = true; }
  else if (shaking && shakeMeter < 0.4f) { shaking = false; ev.shakeEnd = true; ev.shakeMs = now - shakeStartMs; }

  // --- dizziness builds up from shaking and spinning ---
  float spin = fabsf(gyr.y);
  if (shaking) dizzy += dt * shakeMeter * 0.3f;
  if (spin > SPIN_DPS) dizzy += dt * spin / SPIN_DPS * 0.5f;
  dizzy = clampf(dizzy - dt * 0.15f, 0, 10);

  // --- free fall / landing ---
  if (accMag < FREEFALL_G) {
    normalCount = 0;
    if (++lowGCount >= 5 && !falling) { falling = true; fallStartMs = now; ev.freefall = true; }
  } else {
    lowGCount = 0;
  }
  if (falling) {
    if (accMag > LAND_G) { falling = false; ev.landed = true; }
    else if (accMag > 0.7f && accMag < 1.4f) {
      if (++normalCount > 15) { falling = false; ev.caught = true; }
    }
    if (since(fallStartMs) > 2500) { falling = false; ev.caught = true; }
  }

  // --- taps and knocks (only while resting) ---
  float tapG = kick >= 0 ? kick : (fifoOk ? 0 : jerk);
  if (tapG > tapPeak) tapPeak = tapG;
  // The buzzer shakes the case: its own sounds must not count as taps.
  static uint32_t lastSoundMs = 0;
  if (soundPlaying) lastSoundMs = now;
  bool quietSpeaker = now - lastSoundMs > 150;
  // One tap = one count: after a tap the case rings for a bit, so the next tap can
  // only count once the sensor has calmed down again (below half the threshold).
  static bool tapArmed = true;
  if (!tapArmed && tapG < TAP_G * 0.5f && now - lastTapCandMs > TAP_DEBOUNCE_MS) tapArmed = true;
  // Taps count in any position, desk or hand; only being swung / shaken / dropped /
  // just grabbed blocks them.
  bool tapBlocked = falling || shaking || gyroMag > TAP_MAX_GYRO_DPS || now - pickupMs < PICKUP_IGNORE_MS;
  if (tapArmed && quietSpeaker && !tapBlocked) {
    if (tapG > BUMP_G) {
      ev.bump = true;
      if (tapDebug) Serial.printf("E,%lu,bump\n", (unsigned long)now);
      lastTapCandMs = now;
      tapArmed = false;
      tapCount = 0;
    } else if (!micOk && tapG > TAP_G && (int32_t)(typingUntil - now) <= 0) {  // no mic: the IMU alone decides
      tapCount++;
      tapSide += kick >= 0 ? toBody(kickDir).x : dyn.x;
      lastTapMs = lastTapCandMs = now;
      tapArmed = false;
    }
  }

  // With the mic: a tap = a sharp click it heard + a (tiny) jolt of the pet itself,
  // at the same moment. Clicks without a jolt (typing, claps) are ignored.
  if (tapDebug) {  // S,ms,imu kick (g, fast part),mic peak,mic background,handling,shaking,falling,sound,gyro avg,surface
    Serial.printf("S,%lu,%.3f,%.0f,%.0f,%d,%d,%d,%d,%.1f,%d\n", (unsigned long)now, tapG, micPeakDbg, micBg,
                  handling, shaking, falling, soundPlaying, gyroAvg, onSurface);
    micPeakDbg = 0;
  }
  // Every mic click waits (up to ~200 ms, the sensor may be read late) for a jolt
  // with a matching time stamp.
  static uint32_t seenClicks = 0;
  static uint32_t pending[8];
  static uint8_t pendingN = 0;
  if (!fifoOk && tapG > TAP_CONFIRM_G) {  // no FIFO: best effort with the step time
    kickMs[kickIdx] = now; kickSide[kickIdx] = dyn.x; kickVerdict[kickIdx] = HIT_TAP; kickIdx = (kickIdx + 1) % 8;
  }
  if (slamPending) {  // the desk rang: someone slammed it?
    slamPending = false;
    // A real slam is loud: the mic must have heard a bang right then (nudging the desk,
    // putting a mug down or heavy typing mostly isn't). And no startle spam.
    bool bang = !(SLAM_NEEDS_BANG && micOk);
    for (int i = 0; i < 8 && !bang; i++)
      if (micClickCount > (uint32_t)i && (uint32_t)abs((int32_t)(micClickTimes[(micClickCount - 1 - i) % 8] - slamAt)) <= 200) bang = true;
    static uint32_t lastSlam = 0;
    bool cooled = !lastSlam || now - lastSlam > SLAM_COOLDOWN_MS;
    if (tapDebug && (!bang || !cooled)) Serial.printf("E,%lu,bump_ignored_%s\n", (unsigned long)now, bang ? "cooldown" : "quiet");
    bool typingNow = (int32_t)(typingUntil - now) > 0;
    if (bang && cooled && !falling && !shaking && !typingNow) {
      lastSlam = now;
      ev.bump = true;
      tapCount = 0;
      if (tapDebug) Serial.printf("E,%lu,bump\n", (unsigned long)now);
    }
  }
  if (micOk) {
    while (seenClicks != micClickCount) {
      uint32_t i = seenClicks % 8;
      if (tapDebug) Serial.printf("C,%lu,%.0f\n", (unsigned long)micClickTimes[i], micClickPeaks[i]);
      if (pendingN < 8) pending[pendingN++] = micClickTimes[i];
      logActivity(micClickTimes[i]);
      seenClicks++;
    }
    bgJolt += (0.02f - bgJolt) * 0.003f;  // quiet again: back to the floor within ~5 s
    // Typing: a sustained stream of separate touches (not one burst of taps, not the
    // continuous noise of a head scratch). No taps until it has calmed down.
    uint32_t first = 0, lastT = 0;
    int recentN = activityBetween(now - TYPING_WINDOW_MS, now + 1, &first, &lastT);
    if (recentN >= TYPING_EVENTS && lastT - first >= TYPING_MIN_SPAN_MS && !scratching && !scratchLooking()) {
      if ((int32_t)(typingUntil - now) <= 0 && tapDebug) Serial.printf("E,%lu,typing\n", (unsigned long)now);
      typingUntil = now + TYPING_QUIET_MS;
      busyUntil = now + TYPING_BUSY_MS;
      tapCount = 0;
    }
    bool typing = (int32_t)(typingUntil - now) > 0;
    bool busy = (int32_t)(busyUntil - now) > 0;  // only then the stricter checks below
    for (uint8_t p = 0; p < pendingN;) {
      float side = 0, peak = 0;
      int hit = TAP_SOURCE == 2 ? HIT_TAP : takeKickNear(pending[p], &side, &peak);
      bool expired = now - pending[p] > TAP_MATCH_MS + SLAM_RING_TO_MS + 150;  // hit judged + sensor read late
      bool jolted = hit == HIT_TAP;
      if (hit == HIT_TAP || hit == HIT_SLAM || expired) {
        if (hit == HIT_SLAM) {
          if (tapDebug) Serial.printf("E,%lu,click_slam\n", (unsigned long)now);  // part of the slam, not a tap
        } else if (jolted && !tapBlocked) {
          const char *why = nullptr;
          if (typing) why = "typing";
          else if (busy && TAP_SOURCE != 2 && peak < bgJolt * TAP_REL) why = "weak";  // no stronger than the background
          else if (busy && !tapCount && activityBetween(pending[p] - TAP_QUIET_BEFORE_MS, pending[p] - TAP_MATCH_MS) > 0) why = "noquiet";
          if (why) {
            if (tapDebug) Serial.printf("E,%lu,click_%s\n", (unsigned long)now, why);
          } else if (++tapCount > TAP_MAX_GROUP && busy) {  // too many in a row on a busy surface: typing
            tapCount = 0;
            typingUntil = now + TYPING_QUIET_MS;
            if (tapDebug) Serial.printf("E,%lu,click_toomany\n", (unsigned long)now);
          } else {
            tapSide += side;
            lastTapMs = now;
            if (tapDebug) Serial.printf("E,%lu,click_tap\n", (unsigned long)now);
          }
        } else if (tapDebug) {
          Serial.printf("E,%lu,%s\n", (unsigned long)now, jolted ? "click_blocked" : "click_ignored");
        }
        pending[p] = pending[--pendingN];
      } else {
        p++;
      }
    }
  }
  // --- head scratches: rough noise + small vibration of the pet, for most of a second ---
  static uint32_t winStart = 0;
  static float winImu = 0;
  static bool winSound = false;
  static uint16_t scratchBits = 0;
  if (tapG > winImu) winImu = tapG;
  if (soundPlaying) winSound = true;
  if (micOk && now - winStart >= 100) {
    if (!winSound) {  // windows with our own buzzer in them prove nothing either way
      bool scratchy = micPeakWin > micBg * SCRATCH_MIC_RATIO && winImu > SCRATCH_IMU_G;
      scratchBits = ((scratchBits << 1) | scratchy) & 0x3FF;
      int n = __builtin_popcount(scratchBits);
      if (!scratching && n >= SCRATCH_START_OF_10 && !shaking && !falling && (int32_t)(typingUntil - now) <= 0) {
        scratching = true;
        scratchStartMs = now;
        ev.scratchStart = true;
        tapCount = 0;  // those "taps" were the scratching
        if (tapDebug) Serial.printf("E,%lu,scratch_start\n", (unsigned long)now);
      } else if (scratching && (n <= SCRATCH_STOP_OF_10 || shaking || falling)) {
        scratching = false;
        scratchEndMs = now;
        ev.scratchEnd = true;
        ev.scratchMs = now - scratchStartMs;
        if (tapDebug) Serial.printf("E,%lu,scratch_end\n", (unsigned long)now);
      }
    }
    winStart = now;
    winImu = 0;
    micPeakWin = 0;
    winSound = false;
  }
  scratchBitsNow = scratchBits;
  // No taps while (or right after) being scratched: every stroke sounds like a click.
  // A tap group that looked scratchy at any point is thrown away.
  static bool tapGroupScratchy = false;
  // Taps leave quiet windows between them (1-0-1-0-1); scratching is continuous (1-1-1).
  bool scratchSuspect = scratching || now - scratchEndMs < 600 || (scratchBits & 0x7) == 0x7;
  if (scratchSuspect && tapCount) tapGroupScratchy = true;
  if (scratching) tapCount = 0;
  if (!tapCount) tapGroupScratchy = false;

  if (tapCount && now - lastTapMs > TAP_WINDOW_MS && !scratchSuspect) {
    if (tapGroupScratchy) {
      if (tapDebug) Serial.printf("E,%lu,taps_dropped_scratchy,%d\n", (unsigned long)now, tapCount);
      tapCount = 0;
      tapSide = 0;
      tapGroupScratchy = false;
    }
  }
  if (tapCount && now - lastTapMs > TAP_WINDOW_MS && !scratchSuspect) {
    ev.taps = tapCount;
    ev.tapSide = tapSide;
    if (tapDebug) Serial.printf("E,%lu,taps,%d\n", (unsigned long)now, tapCount);
    tapCount = 0;
    tapSide = 0;
  }

  // --- desk activity: tiny vibrations from typing / mouse / mug ---
  vib += (jerk - vib) * 0.02f;
  if (!handling && !shaking && !soundPlaying) {
    if (vib < vibFloor) vibFloor = vib;
    else if (vib < vibFloor * 1.5f) vibFloor += (vib - vibFloor) * 0.001f;
    else vibFloor += (vib - vibFloor) * 0.000002f;  // very slow creep if the room got noisier
    if (vib > vibFloor * DESK_RATIO + DESK_MARGIN_G) deskActiveUntil = now + 4000;
  }

  // --- orientation (only when resting) ---
  if (!handling && !falling && fabsf(accMag - 1.0f) < 0.2f) {
    Orient o = classify(grav);
    if (o != orientCand) { orientCand = o; orientCandSince = now; }
    else if (o != orient && now - orientCandSince > ORIENT_SETTLE_MS) {
      prevOrient = orient;
      orient = o;
      ev.orientChanged = true;
    }
  }
}

// =====================================================================
//  THE PET
// =====================================================================
const char *ACTIVITY_NAMES[] = {"awake", "drowsy", "asleep", "hiding"};

Activity activity = ACT_AWAKE;
uint32_t activitySince = 0;

float happiness = 70;   // 0..100
float energy = 90;      // 0..100
uint32_t ageMinutes = 0;

uint32_t lastInteraction = 0;  // someone touched / moved the pet
uint32_t lastPresence = 0;     // someone is working at the desk
bool everPresent = false;

bool bootPending = true;
uint32_t bootAt = 0;

bool batMode = false;
uint32_t upsideDownSince = 0, nextHelpAt = 0;
uint32_t lieDownSince = 0;
uint32_t lastPickupJoy = 0;
uint32_t battWokeAt = 0;  // last time it came back from sleep (0 = boot, also counts)

uint32_t nextQuirkAt = 0;
uint32_t nextSnoreAt = 0, nextDreamAt = 0, peekCloseAt = 0;
uint32_t hopRestoreAt = 0;  int hopRestoreY = 0;
uint8_t hiccupsLeft = 0;    uint32_t nextHiccupAt = 0;
uint8_t sneezeStage = 0;    uint32_t sneezeAt = 0;
bool dizzyShown = false, grumpyPending = false;
uint8_t recentTaps = 0;     uint32_t recentTapsSince = 0;
float workDriftX = 0;       uint32_t nextWorkDriftAt = 0;

bool workActive = false, breakDue = false;
uint32_t workStart = 0, nextBreakNagAt = 0;

bool birthdayPending = false;
bool imuStream = false;

bool present() { return imuOk ? !reached(deskActiveUntil) : true; }
uint8_t chatterLevel() { return present() ? 3 : 2; }   // keep quiet while you work
void chatter(int id) { say(id, chatterLevel()); }
void addHappiness(float d) { happiness = clampf(happiness + d, 0, 100); }
void addEnergy(float d) { energy = clampf(energy + d, 0, 100); }

void scheduleQuirk(float factor = 1.0f) {
  nextQuirkAt = millis() + (uint32_t)(random(QUIRK_MIN_S, QUIRK_MAX_S + 1) * 1000UL * factor);
}

void setActivity(Activity a) {
  if (a == activity) return;
  activity = a;
  activitySince = millis();
  faceDirty = true;
  reactionUntil = millis();
  wantDim = (a == ACT_ASLEEP);
  wantDisplayOn = (a != ACT_HIDING);
  hidingQuiet = (a == ACT_HIDING);
  if (a == ACT_HIDING) hush();

  if (a == ACT_ASLEEP || a == ACT_HIDING) {
    hiccupsLeft = 0;
    sneezeStage = 0;
    peekCloseAt = 0;
    lookAt(0, 0.3f);
  }
  if (a == ACT_ASLEEP) {
    showOverlay(OV_ZZZ, 0);
    nextSnoreAt = millis() + random(8000, 20000);
    nextDreamAt = millis() + random(120000, 300000);
  } else {
    clearOverlay(OV_ZZZ);
  }
  if (a == ACT_AWAKE) scheduleQuirk();
  if (a == ACT_AWAKE || a == ACT_DROWSY) battWokeAt = millis();  // battery voltage settles for a while
  Serial.printf("[pet] %s\n", ACTIVITY_NAMES[a]);
}

void fallAsleep() {
  if (activity == ACT_ASLEEP) return;
  say(S_SLEEPING, 2);
  setActivity(ACT_ASLEEP);
}

// Someone did something with the pet: it wakes up from drowsing and isn't bored anymore.
void interact() {
  lastInteraction = millis();
  if (activity == ACT_DROWSY) setActivity(ACT_AWAKE);
}

void wakeUp(bool startled) {
  setActivity(ACT_AWAKE);
  lieDownSince = millis();
  if (startled) {
    Face f; f.wide = true;
    react(f, 1300);
    showOverlay(OV_ALERT, 1300);
    say(S_SURPRISE, 1, true);
  } else {
    react(faceWith(TIRED), 2000);
    say(SND_YAWN);
  }
}

// ---------------- reactions ----------------
void petMe() {  // double tap or touch pad
  interact();
  react(faceWith(HAPPY), 2500);
  showOverlay(OV_HEARTS, 2500);
  laughEyes();
  say(random(2) ? S_CUDDLY : SND_PURR);
  addHappiness(PET_JOY);
  addEnergy(3);  // a cuddle perks it up a bit
}

void boop(float side) {
  interact();
  blinkEyes();
  // The tap pushes the pet away from where it came from: look towards the finger.
  glance(side > 0 ? -1.0f : 1.0f, frand(-0.3f, 0.2f), 1200);
  say(SND_CHIRP);
  addHappiness(PET_JOY / 5);
}

void sing() {
  interact();
  if (energy < 15) {  // too tired to sing
    react(faceWith(TIRED), 2000);
    say(SND_YAWN);
    return;
  }
  react(faceWith(HAPPY), 4000);
  showOverlay(OV_NOTES, 4000);
  laughEyes();
  say(random(2) ? SND_SONG_A : SND_SONG_B);
  addHappiness(PET_JOY / 2);
  addEnergy(-3);
}

void startle() {
  interact();
  Face f; f.wide = true;
  react(f, 1300);
  showOverlay(OV_ALERT, 1300);
  glance(0, -0.2f, 1300);
  say(S_SURPRISE, 1, true);
  if (hiccupsLeft) hiccupsLeft = 0;  // scared the hiccups away
  addHappiness(-2);
}

void annoyed() {
  react(faceWith(ANGRY), 2500);
  glance(0, -0.4f, 2500);
  say(S_MODE3);
  addHappiness(-6);
}

void greetUser() {
  if (activity == ACT_HIDING) return;
  if (activity != ACT_AWAKE) setActivity(ACT_AWAKE);
  react(faceWith(HAPPY), 2500);
  showOverlay(OV_HEARTS, 2500);
  glance(0, -0.3f, 2500);
  laughEyes();
  say(S_SUPER_HAPPY);
  addHappiness(5);
  Serial.println(F("[pet] welcome back!"));
}

void nagBreak() {
  if (activity == ACT_DROWSY) setActivity(ACT_AWAKE);
  Face f; f.mood = TIRED; f.sweat = true;
  react(f, 4000);
  glance(0, -0.4f, 4000);  // look up at you
  showOverlay(OV_BREAK, 0);
  say(SND_BREAK);
  Serial.println(F("[pet] time for a break!"));
}

void ackBreak() {
  breakDue = false;
  workStart = millis();
  clearOverlay(OV_BREAK);
  react(faceWith(HAPPY), 2500);
  showOverlay(OV_HEARTS, 2500);
  say(S_HAPPY);
  addHappiness(5);
}

void celebrateBirthday() {
  birthdayPending = false;
  react(faceWith(HAPPY), 7000);
  showOverlay(OV_NOTES, 7000);
  laughEyes();
  say(SND_BIRTHDAY);
  addHappiness(15);
  Serial.printf("[pet] happy birthday! %lu day(s) old\n", (unsigned long)(ageMinutes / 1440));
}

// ---- head scratches ----
uint32_t nextPurrAt = 0;

void onScratchStart() {
  Serial.println(F("[pet] scritches!"));
  if (activity == ACT_ASLEEP) { say(SND_PURR, 2); return; }  // purrs in its sleep, stays asleep
  interact();
  if (activity == ACT_DROWSY) setActivity(ACT_AWAKE);
  nextPurrAt = 0;
}

void scratchStep() {  // 50 Hz while being scratched: happy squint, hearts, purring
  if (!scratching || activity != ACT_AWAKE) return;
  lastInteraction = millis();
  react(faceWith(HAPPY), 700);
  if (overlay != OV_HEARTS) showOverlay(OV_HEARTS, 1500);
  glance(0, -0.6f, 700);  // leans into it, eyes up
  if (reached(nextPurrAt) && !soundPlaying) {
    say(SND_PURR);
    nextPurrAt = millis() + 1800;
  }
  addHappiness(SCRATCH_JOY_PER_S / 50.0f);  // brain runs at 50 Hz
}

void onScratchEnd(uint32_t ms) {
  if (ms < 1500) return;
  react(faceWith(HAPPY), 2000);
  laughEyes();
  say(S_HAPPY);
}

// Face down + double tap = switch off. It can't be seen, so it sings goodnight.
void switchOffFromHiding() {
  if (offTest) { Serial.printf("E,%lu,would_switch_off\n", (unsigned long)millis()); return; }
  Serial.println(F("[power] face down + double tap: goodnight"));
  hidingQuiet = false;
  say(SND_GOODNIGHT, 1, true);
  delay(2200);
  powerOff(OFF_USER, false);
}

// While face down: two jolts 0.1-0.7 s apart (after it has lain still for 1.5 s).
void hidingDoubleTapStep() {
  static uint32_t seen = 0, prev = 0;
  while (seen != joltLogCount) {
    uint32_t t = joltLog[seen % 4];
    seen++;
    if (activity != ACT_HIDING || (int32_t)(t - activitySince) < 1500) { prev = 0; continue; }
    if (prev && t - prev >= 100 && t - prev <= 700) { prev = 0; switchOffFromHiding(); return; }
    prev = t;
  }
}

// ---- face down = power save ----
bool powerSave = false;
uint32_t psHoldoffUntil = 0;

void enterPowerSave() {
  powerSave = true;
  micPauseReq = true;
  vTaskDelay(pdMS_TO_TICKS(150));  // let the eyes task switch the screen off, the mic task stop
  Serial.println(F("[power] face down: power save (screen + mic off, light sleep)"));
  Serial.flush();
  setCpuFrequencyMhz(80);
}

void exitPowerSave() {
  setCpuFrequencyMhz(160);
  powerSave = false;
  micPauseReq = false;
  imuRestart = true;  // gravity from the fresh samples: the face-up turn is seen at once
  psHoldoffUntil = millis() + 3000;  // full speed while it notices it was turned over
  Serial.println(F("[power] turned over: waking up"));
}

// One power-save step: read the sensor, check for "face up" and the switch-off double
// tap, then light-sleep until the next one.
void powerSaveStep() {
  if (!powerSave) enterPowerSave();
  imuStep();               // drains the FIFO: jolts for the face-down double tap
  ev = MotionEvents();     // nothing else reacts while it hides
  static uint8_t upCount = 0;
  if (acc.z > -0.5f) upCount++;  // screen no longer facing down (body z = out of the screen)
  else upCount = 0;
  if (upCount >= 2) { upCount = 0; exitPowerSave(); return; }
  hidingDoubleTapStep();   // may switch it off for real
  static uint32_t lastSec = 0;
  if (millis() - lastSec >= 1000) { lastSec = millis(); needsTick(); battStep(); }
  serialStep();
  esp_sleep_enable_timer_wakeup((uint64_t)PS_WAKE_MS * 1000ULL);
  esp_light_sleep_start();
}

// ---- music: dance on the beat, hum chord notes along; repeat whistled / hummed tunes ----
void sayNote(int midi, uint32_t ms, uint8_t level) {
  say(SND_NOTE_FLAG | (constrain(midi, 0, 127) << 16) | (int)min<uint32_t>(ms, 0xFFFF), level);
}

extern uint8_t calStage;  // (calibration section, further down)
bool dancing = false;
#if REPEAT_AFTER_ME
enum { RP_IDLE, RP_LISTEN, RP_PLAY };
uint8_t rpState = RP_IDLE;
#endif
bool repeatBusy() {
#if REPEAT_AFTER_ME
  return rpState != RP_IDLE;
#else
  return false;
#endif
}

bool beatClear() { return ears.st.beatAvg >= SING_MIN_BEAT; }

// Hold the root of the current (stable) chord, in the buzzer's clear range. Not for the
// whole chord: the mic can't listen while the buzzer plays, it needs a gap to hear the
// next chord change.
void playBass(uint32_t span) {
#if BASS_ALONG
  if (ears.st.bass < 0 || chattiness < 2 || muted) return;
  int midi = SING_BASE_MIDI + ((ears.st.bass - SING_BASE_MIDI) % 12 + 12) % 12;
  sayNote(midi, constrain((uint32_t)(span * BASS_SUSTAIN), 150UL, (uint32_t)BASS_MAX_MS), 2);
#endif
}

// One note from the song's own 3 strongest notes: step = position in a low-mid-high-mid walk.
void singSongNote(int step, uint32_t ms) {
#if SING_ALONG
  if (ears.st.songPc[2] < 0 || chattiness < 2 || muted) return;
  int pcs[3];
  for (int i = 0; i < 3; i++) pcs[i] = ((ears.st.songPc[i] - SING_BASE_MIDI) % 12 + 12) % 12;
  std::sort(pcs, pcs + 3);  // low to high inside the octave above SING_BASE_MIDI
  static const uint8_t walk[4] = {0, 1, 2, 1};
  sayNote(SING_BASE_MIDI + pcs[walk[step % 4]], ms, 2);
#endif
}

void onBeat(uint32_t n, uint32_t period) {
  glance((n % 2) ? 0.7f : -0.7f, -0.1f, period);  // sway left / right on the beat
  react(faceWith(HAPPY), period + 200);
  if (n % 4 == 0 && !hopRestoreAt) {  // a little hop on the first beat of the bar
    BusLock lock;
    hopRestoreY = eyes.eyeLyNext;
    eyes.eyeLyNext = 0;
    hopRestoreAt = millis() + 120;
  }
  if (n % 8 == 0) showOverlay(OV_NOTES, period * 4);
#if SING_ALONG
  // Sing along: SING_BARS_ON bars singing, SING_BARS_OFF listening. Only the notes the song
  // itself played most (always in key), up in the buzzer's sweet spot, walking up and down.
  int bar = (n / 4) % (SING_BARS_ON + SING_BARS_OFF), beat = n % 4;
  if (bar < SING_BARS_ON) singSongNote(beat, min<uint32_t>(period * 55 / 100, 260));
#endif
#if BASS_ALONG
  if (n % 2 == 0) playBass(period * 2);  // beats 1 and 3 of the bar, held ~1.4 beats
#endif
}

void musicStep() {
#if ENABLE_EARS
  static const char *NOTE_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  uint32_t now = millis();
  bool heard = micOk && (ears.st.music || ears.st.singing);
  // Songs have stretches the detector doesn't like: while it's still hearing steady,
  // continuous sound, keep dancing a while instead of stopping and restarting.
  static uint32_t lastHeard = 0;
  if (heard) lastHeard = now;
  bool stillGoing = dancing && now - lastHeard < MUSIC_GRACE_MS &&
                    ears.st.loudAvg > MUSIC_MIN_DB && ears.st.continuity > MUSIC_MIN_CONT;
  bool on = (heard || stillGoing) && activity == ACT_AWAKE && !calStage && !repeatBusy();
  if (on && !dancing) {
    dancing = true;
    interact();
    react(faceWith(HAPPY), 1500);
    laughEyes();
    showOverlay(OV_NOTES, 2500);
    if (ears.st.music)
      Serial.printf("[pet] music! %.0f BPM, key %s %s\n", ears.st.bpm, NOTE_NAMES[max<int>(0, ears.st.key)],
                    ears.st.keyMinor ? "minor" : "major");
    else
      Serial.println(F("[pet] someone is singing!"));
  } else if (!on && dancing) {
    dancing = false;
    if (activity == ACT_AWAKE) { react(faceWith(HAPPY), 1500); say(SND_GIGGLE, 2); }
    Serial.println(F("[pet] music stopped"));
    return;
  }
  if (!dancing) return;
  lastInteraction = now;  // music is company
  if (!beatClear()) {  // no clear beat: sway smoothly, don't pretend to know the beat
    // ...and hum along freely: a short phrase of the song's own notes, then a pause.
    static uint32_t nextSway = 0, humStep = 0;
    static bool swayLeft = false;
    if (reached(nextSway)) {
      swayLeft = !swayLeft;
      glance(swayLeft ? -0.6f : 0.6f, -0.1f, HUM_NOTE_MS * 2);
      react(faceWith(HAPPY), HUM_NOTE_MS * 2 + 200);
      if (overlay != OV_NOTES) showOverlay(OV_NOTES, 3000);
      if (humStep % (HUM_PHRASE + 1) < HUM_PHRASE) singSongNote(humStep % (HUM_PHRASE + 1), HUM_NOTE_MS * 6 / 10);
      humStep++;
      nextSway = now + HUM_NOTE_MS;
    }
#if BASS_ALONG
    // no clear beat: pluck the root whenever the chord changes
    static uint32_t seenChanges = 0, lastPluck = 0;
    if (ears.st.bassChanges != seenChanges) {
      seenChanges = ears.st.bassChanges;
      if (now - lastPluck > 400) { playBass(ears.st.chordMs ? ears.st.chordMs : 1500); lastPluck = now; }
    }
#endif
    return;
  }
  uint32_t period = ears.st.beatMs, last = ears.st.lastBeat;
  if (period < 300 || period > 1100 || (int32_t)(now - last) < 0) return;
  uint32_t beatTime = last + (now - last) / period * period;
  static uint32_t prevBeat = 0, beatN = 0;
  if (now - beatTime < 80 && beatTime - prevBeat > period / 2) {
    prevBeat = beatTime;
    onBeat(++beatN, period);
  }
#endif
}

#if REPEAT_AFTER_ME
float rpFrames[420];  // one pitch (midi, 0 = none) per 20 ms brain tick
uint16_t rpN = 0;
uint32_t rpStart = 0, rpLastPitch = 0, rpSteadySince = 0, rpPlayUntil = 0;
float rpSteadyMidi = 0;

void repeatPlayBack() {
  while (rpN && rpFrames[rpN - 1] == 0) rpN--;  // trailing silence
  // median pitch -> octave shift into the buzzer's sweet spot
  float sorted[420];
  int np = 0;
  for (int i = 0; i < rpN; i++) if (rpFrames[i] > 0) sorted[np++] = rpFrames[i];
  if (np < 10) { rpState = RP_IDLE; return; }
  std::sort(sorted, sorted + np);
  float med = sorted[np / 2];
  int shift = 12 * (int)roundf((SING_BASE_MIDI + 4 - med) / 12.0f);
  // frames -> notes: a note continues while the pitch stays within ~0.8 semitone
  int notes = 0;
  uint32_t total = 0;
  int i = 0;
  while (i < rpN && notes < 60) {
    int j = i;
    float sum = 0;
    if (rpFrames[i] == 0) {
      while (j < rpN && rpFrames[j] == 0) j++;
      if (j - i >= 3) { sayNote(0, (j - i) * 20, 1); total += (j - i) * 20; notes++; }
    } else {
      float ref = rpFrames[i];
      while (j < rpN && rpFrames[j] > 0 && fabsf(rpFrames[j] - ref) < 0.8f) { sum += rpFrames[j]; j++; }
      int len = j - i;
      if (len >= 3) {
        int midi = (int)roundf(sum / len) + shift;
        uint32_t ms = len * 20;
        sayNote(midi, ms > 80 ? ms - 60 : 30, 1);  // the sound task adds ~60 ms per note
        total += ms;
        notes++;
      }
    }
    i = j;
  }
  rpState = RP_PLAY;
  rpPlayUntil = millis() + total + 600;
  react(faceWith(HAPPY), total + 500);
  showOverlay(OV_NOTES, total + 500);
  Serial.printf("[pet] my turn! %d notes, %+d semitones\n", notes, shift);
}
#endif

void repeatStep() {
#if REPEAT_AFTER_ME && ENABLE_EARS
  uint32_t now = millis();
  bool fresh = micOk && ears.st.pitch && now - ears.st.pitchMs < 60 && !soundPlaying;
  float m = ears.st.pitchMidi;
  switch (rpState) {
    case RP_IDLE:
      if (activity != ACT_AWAKE || dancing || scratching || calStage || hiddenOrBusy()) { rpSteadySince = 0; return; }
      if (fresh) {
        if (!rpSteadySince || fabsf(m - rpSteadyMidi) > 0.7f) { rpSteadySince = now; rpSteadyMidi = m; }
        else if (now - rpSteadySince >= REPEAT_START_MS) {  // a steady note: someone whistles / hums at it
          rpState = RP_LISTEN;
          rpStart = rpSteadySince;
          rpLastPitch = now;
          rpN = 0;
          for (uint32_t t = rpSteadySince; t < now && rpN < 420; t += 20) rpFrames[rpN++] = rpSteadyMidi;
          interact();
          react(faceWith(DEFAULT), REPEAT_MAX_MS);
          showOverlay(OV_QUESTION, 0);
          Serial.printf("[pet] listening (%s)...\n", ears.st.pitchKind == 1 ? "whistle" : "hum");
        }
      } else if (now - ears.st.pitchMs > 150) {
        rpSteadySince = 0;
      }
      break;
    case RP_LISTEN:
      if (rpN < 420) rpFrames[rpN++] = fresh ? m : 0;
      if (fresh) rpLastPitch = now;
      glance(0, -0.5f, 100);  // looks up at you, all ears
      if (now - rpLastPitch > REPEAT_END_MS || now - rpStart > REPEAT_MAX_MS) {
        clearOverlay(OV_QUESTION);
        repeatPlayBack();
      }
      break;
    case RP_PLAY:
      if (reached(rpPlayUntil)) {
        rpState = RP_IDLE;
        rpSteadySince = 0;
        react(faceWith(HAPPY), 1500);
        showOverlay(OV_HEARTS, 1500);
      }
      break;
  }
#endif
}

bool hiddenOrBusy() { return activity == ACT_HIDING || reactingToSomethingBig(); }
bool reactingToSomethingBig() { return shaking || falling || handling; }

void onTaps(uint8_t n, float side) {
  if (activity == ACT_HIDING) return;  // face down: see hidingDoubleTapStep()

  if (since(recentTapsSince) > 10000) { recentTapsSince = millis(); recentTaps = 0; }
  recentTaps += n;
  if (recentTaps > 10) {  // tap spam
    recentTaps = 0;
    interact();
    if (activity == ACT_ASLEEP) setActivity(ACT_AWAKE);
    annoyed();
    return;
  }

  if (activity == ACT_ASLEEP) {
    if (n == 1 && since(activitySince) < minutes(2)) {
      // Just fell asleep: one eye opens, grumbles, goes back to sleep.
      { BusLock lock; eyes.open(true, false); }
      peekCloseAt = millis() + 900;
      say(SND_GRUMBLE);
      return;
    }
    interact();
    wakeUp(false);
    return;
  }

  if (hiccupsLeft) {  // any tap cures hiccups
    hiccupsLeft = 0;
    interact();
    react(faceWith(HAPPY), 1200);
    say(SND_RELIEF);
    return;
  }
  if (breakDue && n >= 2) { interact(); ackBreak(); return; }

  if (n == 1) boop(side);
  else if (n == 2) petMe();
  else sing();
}

void onOrientChanged(Orient from, Orient to) {
  Serial.printf("[imu] %s -> %s\n", ORIENT_NAMES[from], ORIENT_NAMES[to]);
  uint32_t now = millis();

  // Leaving special poses
  if (from == OR_FACE_DOWN) {  // peekaboo!
    setActivity(ACT_AWAKE);
    interact();
    react(faceWith(HAPPY), 2000);
    showOverlay(OV_HEARTS, 1500);
    laughEyes();
    say(SND_PEEKABOO, 1, true);
    addHappiness(5);
  }
  if (from == OR_UPSIDE_DOWN) {
    wantRotation = DISPLAY_ROTATION;
    if (activity == ACT_ASLEEP) {  // woke from a bat nap
      wakeUp(false);
    } else {
      react(faceWith(HAPPY), 1500);
      say(SND_RELIEF);
    }
    batMode = false;
  }
  if ((from == OR_LYING_LEFT || from == OR_LYING_RIGHT) &&
      (to == OR_UPRIGHT || to == OR_TILTED) && activity == ACT_ASLEEP) {
    interact();
    wakeUp(false);
  }

  // Entering poses
  switch (to) {
    case OR_UPSIDE_DOWN:
      if (activity == ACT_ASLEEP) setActivity(ACT_AWAKE);
      interact();
      wantRotation = DISPLAY_ROTATION ^ 2;  // flip the screen so the face stays readable
      batMode = false;
      upsideDownSince = now;
      nextHelpAt = now + 5000;
      {
        Face f; f.mood = ANGRY; f.vFlicker = 2;
        react(f, 2000);
      }
      say(SND_HELP, 1, true);
      addHappiness(-3);
      break;
    case OR_LYING_LEFT:
    case OR_LYING_RIGHT:
      lieDownSince = now;  // bedtime gesture: falls asleep if left lying down
      if (activity == ACT_AWAKE || activity == ACT_DROWSY) {
        react(faceWith(DEFAULT), 1500);  // sleepy, not sad: the eyes squint shut in activityStep
        say(SND_YAWN, 2);
      }
      break;
    case OR_FACE_DOWN:
      setActivity(ACT_HIDING);  // do not disturb: screen off, silent
      break;
    case OR_FACE_UP:
      if (activity != ACT_ASLEEP) {
        react(faceWith(DEFAULT), 1500);
        chatter(SND_WHISTLE);  // ooh, the ceiling
      }
      break;
    default:
      break;
  }
}

void handleMotionEvents() {
  MotionEvents e = ev;
  ev = MotionEvents();
  uint32_t now = millis();

  if (e.orientChanged) onOrientChanged(prevOrient, orient);

  if (e.freefall && activity != ACT_HIDING) {
    setActivity(ACT_AWAKE);
    Face f; f.wide = true; f.vFlicker = 3;
    react(f, 3000);
    showOverlay(OV_ALERT, 3000);
    say(S_OHOOH2, 1, true);  // aaaaah
  }
  if (e.landed) {
    interact();
    Face f; f.mood = ANGRY; f.hFlicker = 2;
    react(f, 2500);
    showOverlay(OV_DIZZY, 2500);
    say(SND_OOF, 1, true);
    addHappiness(-10);
    dizzy += 1.0f;
  }
  if (e.caught) {
    react(faceWith(HAPPY), 1500);
    say(SND_RELIEF, 1, true);
  }

  if (e.shakeStart && activity != ACT_HIDING) {
    if (activity != ACT_AWAKE) setActivity(ACT_AWAKE);
    interact();
    say(S_OHOOH, 1, true);
  }
  if (e.shakeEnd) {
    if (e.shakeMs > SHAKE_GRUMPY_MS) { grumpyPending = true; addHappiness(-12); }
    else if (dizzy < DIZZY_SHOW) {  // a little shake is fun
      react(faceWith(HAPPY), 1500);
      say(SND_GIGGLE);
      addHappiness(2);
    }
  }

  if (e.pickup && activity != ACT_HIDING) {
    if (activity == ACT_ASLEEP) {
      interact();
      wakeUp(true);
    } else {
      interact();
      if (since(lastPickupJoy) > 30000) {
        lastPickupJoy = now;
        react(faceWith(happiness > 40 ? HAPPY : DEFAULT), 1500);
        say(SND_WHEE, 2);
        addHappiness(2);
      }
      if (breakDue) ackBreak();  // you noticed me - counts as acknowledging the break
    }
  }
  if (e.putdown && activity == ACT_AWAKE) blinkEyes();

  if (e.bump && activity != ACT_HIDING) {
    if (activity == ACT_ASLEEP) { interact(); wakeUp(true); }
    else startle();
  }

  if (e.scratchStart && activity != ACT_HIDING) onScratchStart();
  if (e.scratchEnd && activity == ACT_AWAKE) onScratchEnd(e.scratchMs);
  if (e.taps) onTaps(e.taps, e.tapSide);
}

// ---------------- needs, sleep and the rest of daily life ----------------
void needsTick() {  // once per second
  bool resting = (activity == ACT_ASLEEP || activity == ACT_HIDING);
  bool eating = (battState == BATT_CHARGING || battState == BATT_FULL);  // plugged in = being fed
  if (resting) addEnergy(100.0f / (ENERGY_RECHARGE_MIN * 60.0f));
  else if (!eating) addEnergy(-100.0f / (ENERGY_AWAKE_HOURS * 3600.0f));
  if (eating && happiness < 80) addHappiness(1.0f / 60.0f);  // a full tummy is a happy tummy
  if (eating && !resting) addEnergy(100.0f / (ENERGY_CHARGE_MIN * 60.0f));  // being fed restores energy

  if (!resting) {
    bool bored = since(lastInteraction) > minutes(BORED_AFTER_MIN);
    if (bored && !present()) addHappiness(-1.0f / 60.0f);          // lonely: -1 per minute
    else if (present() && happiness < 60) addHappiness(1.0f / 120.0f); // company is nice
  }
}

void ageTick() {  // once per minute
  ageMinutes++;
  if (ageMinutes % 1440 == 0) birthdayPending = true;
}

void loadMemory() {
#if REMEMBER_MOOD
  prefs.begin("kitbit", true);
  happiness = prefs.getFloat("happy", 70);
  energy = prefs.getFloat("energy", 90);
  ageMinutes = prefs.getUInt("age", 0);
  prefs.end();
#endif
}

void saveMemory() {
#if REMEMBER_MOOD
  prefs.begin("kitbit", false);
  prefs.putFloat("happy", happiness);
  prefs.putFloat("energy", energy);
  prefs.putUInt("age", ageMinutes);
  prefs.end();
#endif
}

void presenceStep() {
  if (!imuOk || !present()) return;
  uint32_t now = millis();
  if (everPresent && since(lastPresence) > minutes(AWAY_MIN)) greetUser();
  everPresent = true;
  lastPresence = now;
  if (activity == ACT_DROWSY && energy >= 15) setActivity(ACT_AWAKE);
}

void breakBuddyStep() {
#if BREAK_REMINDER_MIN > 0
  if (!imuOk) return;
  uint32_t now = millis();
  if (present() && !workActive) { workActive = true; workStart = now; }
  if (workActive && since(lastPresence) > minutes(BREAK_RESET_MIN)) {  // you took a break
    workActive = false;
    if (breakDue) { breakDue = false; clearOverlay(OV_BREAK); }
  }
  if (workActive && !breakDue && since(workStart) > minutes(BREAK_REMINDER_MIN)) {
    breakDue = true;
    nextBreakNagAt = now;
  }
  if (breakDue && reached(nextBreakNagAt) && (activity == ACT_AWAKE || activity == ACT_DROWSY)) {
    nagBreak();
    nextBreakNagAt = now + minutes(BREAK_NAG_EVERY_MIN);
  }
#endif
}

void activityStep() {
  uint32_t now = millis();
  uint32_t quietFor = min(since(lastInteraction), imuOk ? since(lastPresence) : 0UL);
  bool lying = (orient == OR_LYING_LEFT || orient == OR_LYING_RIGHT);

  // Bedtime: lying on its side, the eyes slowly squint shut until it falls asleep.
  static bool squinting = false;
  if (lying && activity == ACT_AWAKE) {
    float p = clampf(since(lieDownSince) / 6000.0f, 0, 1);
    int h = EYE_H - (int)((EYE_H - 8) * p);
    static int lastH = 0;
    if (!squinting || abs(h - lastH) >= 2) {
      BusLock lock;
      eyes.setHeight(h, h);
      lastH = h;
    }
    squinting = true;
  } else if (squinting) {  // picked up again (or fell asleep): normal eyes
    squinting = false;
    faceDirty = true;
  }

  switch (activity) {
    case ACT_AWAKE:
      if (reacting()) break;
      if (lying && since(lieDownSince) > 6000) fallAsleep();
      else if (orient == OR_UPSIDE_DOWN && batMode && since(upsideDownSince) > 60000) fallAsleep();  // bat nap
      else if (quietFor > minutes(DROWSY_AFTER_MIN) || (energy < 8 && since(activitySince) > 60000)) {
        setActivity(ACT_DROWSY);
        say(SND_YAWN, 2);
      }
      break;

    case ACT_DROWSY:
      if (lying || quietFor > minutes(SLEEP_AFTER_MIN) || since(activitySince) > minutes(5)) fallAsleep();
      break;

    case ACT_ASLEEP:
      if (peekCloseAt && reached(peekCloseAt)) {
        BusLock lock;
        eyes.close();
        peekCloseAt = 0;
      }
      // Rested and you're around: wake up by itself.
      if (energy >= 99 && present() && !lying && orient != OR_UPSIDE_DOWN) { wakeUp(false); break; }
      if (since(activitySince) < minutes(SNORE_MINUTES) && reached(nextSnoreAt)) {
        say(SND_SNORE, 2);
        nextSnoreAt = now + random(15000, 35000);
      }
      if (reached(nextDreamAt)) {  // dreaming: peeks with one eye
        bool left = random(2);
        { BusLock lock; eyes.open(left, !left); }
        peekCloseAt = now + 700;
        nextDreamAt = now + random(120000, 360000);
      }
      break;

    case ACT_HIDING:
#if AUTO_OFF_FACEDOWN_MIN > 0
      // Hidden face down while on battery: switch off for real (flip it back to wake it).
      if (battOnBattery() && since(activitySince) > minutes(AUTO_OFF_FACEDOWN_MIN)) powerOff(OFF_USER, false);
#endif
      break;
  }

  // Upside down: complain for a while, then give up and become a bat.
  if (orient == OR_UPSIDE_DOWN && activity == ACT_AWAKE && !batMode) {
    if (since(upsideDownSince) > 20000) {
      batMode = true;
      react(faceWith(HAPPY), 1500);
      say(SND_CHIRP);
      Serial.println(F("[pet] I'm a bat now"));
    } else if (reached(nextHelpAt)) {
      Face f; f.mood = ANGRY; f.vFlicker = 2;
      react(f, 1500);
      say(SND_HELP);
      nextHelpAt = now + 6000;
    }
  }
}

void baselineFace() {
  if (reacting()) return;
  Face f;
  switch (activity) {
    case ACT_ASLEEP:
    case ACT_HIDING:
      f.open = false; f.mood = TIRED; f.curious = false;
      break;
    case ACT_DROWSY:
      f.mood = TIRED;
      break;
    case ACT_AWAKE:
      if (orient == OR_UPSIDE_DOWN) f.mood = batMode ? HAPPY : ANGRY;
      else if (handling) f.mood = happiness > 40 ? HAPPY : DEFAULT;
      // Droopy eyes = sad, or REALLY tired. Normal tiredness shows as yawns / dozing off sooner.
      else if (energy < 10 || happiness < 30 || battState == BATT_LOW || battState == BATT_CRITICAL) f.mood = TIRED;
      else if (happiness > 85) f.mood = HAPPY;
      if (breakDue || battState == BATT_CRITICAL) f.sweat = true;
      break;
  }
  showFace(f);
}

void effectsStep() {
  uint32_t now = millis();

  // Being shaken: wobbly eyes the whole time.
  if (shaking && activity == ACT_AWAKE) {
    Face f; f.hFlicker = 3; f.vFlicker = 2;
    react(f, 300);
    if (dizzy > DIZZY_SHOW * 0.5f && overlay != OV_DIZZY) showOverlay(OV_DIZZY, 600);  // stars once it's getting dizzy
  }

  // Dizzy after shaking / spinning.
  if (!shaking && !dizzyShown && dizzy > DIZZY_SHOW && fabsf(gyr.y) < 60 && activity == ACT_AWAKE) {
    dizzyShown = true;
    uint32_t ms = (uint32_t)clampf(dizzy * 1200, 2000, 8000);
    Face f; f.mood = TIRED; f.hFlicker = 2;
    react(f, ms);
    confusedEyes();
    showOverlay(OV_DIZZY, ms);
    say(S_CONFUSED);
  }
  if (dizzy < 0.3f) dizzyShown = false;

  if (grumpyPending && !reacting() && !shaking) {
    grumpyPending = false;
    react(faceWith(ANGRY), 2500);
    say(SND_GRUMBLE);
  }

  // Hiccup: the eyes hop up for a moment.
  if (hopRestoreAt && reached(hopRestoreAt)) {
    BusLock lock;
    eyes.eyeLyNext = hopRestoreY;
    hopRestoreAt = 0;
  }
  if (hiccupsLeft && reached(nextHiccupAt) && !reacting() && activity == ACT_AWAKE) {
    {
      BusLock lock;
      hopRestoreY = eyes.eyeLyNext;
      eyes.eyeLyNext = 0;
    }
    hopRestoreAt = now + 140;
    gazeHoldUntil = now + 160;
    say(SND_HICCUP, 2);
    hiccupsLeft--;
    nextHiccupAt = now + random(1200, 2600);
  }

  // Sneeze: build up -> achoo -> blink.
  if (sneezeStage && reached(sneezeAt) && activity == ACT_AWAKE) {
    if (sneezeStage == 1) {
      react(faceWith(TIRED), 1600);
      glance(0, -0.7f, 1600);
      chatter(SND_SNEEZE_BUILD);
      sneezeStage = 2; sneezeAt = now + 1500;
    } else if (sneezeStage == 2) {
      Face f; f.mood = TIRED; f.vFlicker = 4;
      react(f, 350);
      blinkEyes();
      chatter(SND_SNEEZE);
      sneezeStage = 3; sneezeAt = now + 400;
    } else {
      react(faceWith(DEFAULT), 800);
      blinkEyes();
      if (random(5) == 0) { sneezeStage = 1; sneezeAt = now + 900; }  // and another one
      else sneezeStage = 0;
    }
  }
}

void quirkStep() {
  if (activity != ACT_AWAKE || reacting() || handling || shaking || hiccupsLeft || sneezeStage) return;
  if (orient != OR_UPRIGHT && orient != OR_TILTED) return;
  if (!reached(nextQuirkAt)) return;

  if (birthdayPending) { celebrateBirthday(); scheduleQuirk(); return; }

  bool bored = since(lastInteraction) > minutes(BORED_AFTER_MIN);
  bool working = present();
  enum { Q_WINK, Q_LOOK, Q_WHISTLE, Q_YAWN, Q_SIGH, Q_ATTENTION, Q_HICCUPS, Q_SNEEZE, Q_GIGGLE, Q_COUNT };
  float w[Q_COUNT] = {
    3,                                   // wink
    4,                                   // look around
    happiness > 50 ? 2.0f : 0.3f,        // whistle
    energy < 40 ? 3.0f : 0.5f,           // yawn
    bored ? 3.0f : 0.0f,                 // sigh
    bored && !working ? 3.0f : 0.3f,     // ask for attention
    0.5f,                                // hiccups
    0.5f,                                // sneeze
    happiness > 70 ? 1.5f : 0.3f         // giggle to itself
  };
  float total = 0;
  for (float x : w) total += x;
  float r = frand(0, total);
  int q = 0;
  for (; q < Q_COUNT - 1; q++) { if (r < w[q]) break; r -= w[q]; }

  switch (q) {
    case Q_WINK: {
      bool left = random(2);
      blinkEyes(left, !left);
      chatter(SND_CHIRP);
      break;
    }
    case Q_LOOK: {
      Face f; f.curious = true;
      react(f, 1800);
      glance(frand(-1, 1), frand(-1, 0.6f), 1800);
      break;
    }
    case Q_WHISTLE:
      react(faceWith(HAPPY), 2200);
      showOverlay(OV_NOTES, 2200);
      chatter(SND_WHISTLE);
      break;
    case Q_YAWN:
      react(faceWith(TIRED), 2200);
      chatter(SND_YAWN);
      break;
    case Q_SIGH:
      react(faceWith(TIRED), 2200);
      glance(frand(-0.5f, 0.5f), 0.7f, 2200);
      chatter(SND_SIGH);
      break;
    case Q_ATTENTION:
      react(faceWith(DEFAULT), 2500);
      glance(0, -0.4f, 2500);
      showOverlay(OV_QUESTION, 2500);
      chatter(happiness < 40 ? S_SAD : S_CUDDLY);
      break;
    case Q_HICCUPS:
      hiccupsLeft = random(4, 9);
      nextHiccupAt = millis();
      break;
    case Q_SNEEZE:
      sneezeStage = 1;
      sneezeAt = millis();
      break;
    case Q_GIGGLE:
      react(faceWith(HAPPY), 1500);
      laughEyes();
      chatter(SND_GIGGLE);
      break;
  }
  scheduleQuirk(working ? 2.0f : 1.0f);
}

void gazeStep() {
  if (hopRestoreAt) return;
  uint32_t now = millis();

  if (activity == ACT_ASLEEP || activity == ACT_HIDING) return;  // eyes parked by setActivity()

  // Tilt wins over everything else: the eyes roll towards the low side.
  float m = grav.len();
  if (imuOk && m > 0.5f) {
    float gx = grav.x / m, gy = grav.y / m, gz = grav.z / m;
    static const float deadzone = sinf(TILT_DEADZONE_DEG * DEG_TO_RAD);
    if (gy > 0.5f && sqrtf(gx * gx + gz * gz) > deadzone) {
      lookAt(-gx * TILT_GAIN, -gz * TILT_GAIN);
      gazeHoldUntil = 0;
      return;
    }
  }

  if (!reached(gazeHoldUntil)) return;
  if (handling) { setIdleGaze(true, 1, 2); return; }  // looking around while carried
  if (orient == OR_FACE_UP) {  // stargazing
    lookAt(sinf(now * 0.0004f) * 0.8f, -1);
    return;
  }
  if (present() && imuOk && activity == ACT_AWAKE) {  // watching you work
    if (reached(nextWorkDriftAt)) {
      workDriftX = frand(-0.6f, 0.6f);
      nextWorkDriftAt = now + random(2500, 7000);
    }
    lookAt(workDriftX, 0.8f);
    return;
  }
  setIdleGaze(true, activity == ACT_DROWSY ? 5 : 2, 3);
}

#if ENABLE_TOUCH
uint32_t touchBaseline = 0;
bool touchWas = false;
uint32_t touchSince = 0, touchNextPurr = 0;

void touchSetup() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) { sum += touchRead(PIN_TOUCH); delay(5); }
  touchBaseline = sum / 16;
}

void touchStep() {
  uint32_t v = touchRead(PIN_TOUCH);
#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
  bool touched = v > touchBaseline / TOUCH_RATIO;  // S2/S3 readings go up when touched
#else
  bool touched = v < touchBaseline * TOUCH_RATIO;
#endif
  uint32_t now = millis();
  if (touched && !touchWas) {
    touchSince = now;
    touchNextPurr = now + 2500;
    if (activity == ACT_ASLEEP) { interact(); wakeUp(false); }
    else if (activity != ACT_HIDING) { if (breakDue) ackBreak(); else petMe(); }
  } else if (touched && reached(touchNextPurr) && activity == ACT_AWAKE) {  // long pat = purring
    interact();
    react(faceWith(HAPPY), 2500);
    showOverlay(OV_HEARTS, 2500);
    say(SND_PURR);
    addHappiness(2);
    touchNextPurr = now + 2500;
  }
  touchWas = touched;
}
#endif

// =====================================================================
//  IMU CALIBRATION
//  Step 1: stand the pet upright and keep it still  -> "up" axis + gyro offset
//  Step 2: lay it on its back, screen facing up      -> "out of screen" axis
//  "right" follows from those two, so any mounting angle works. Saved to flash.
// =====================================================================
uint8_t calStage = 0;  // 0 = off, 1 = upright, 2 = on its back
uint32_t calStartMs = 0;
float calAcc[3], calGyro[3], calUp[3];
int calCount = 0;

static void calPrompt(const char *l1, const char *l2, uint32_t ms) {
  overlayLine1 = l1;
  overlayLine2 = l2;
  showOverlay(OV_TEXT, ms);
  Serial.printf("[cal] %s %s\n", l1, l2);
}

static void calResetAverage() {
  calCount = 0;
  memset(calAcc, 0, sizeof(calAcc));
  memset(calGyro, 0, sizeof(calGyro));
}

void startCalibration() {
  if (!imuOk) { Serial.println(F("[cal] no motion sensor")); return; }
  if (activity != ACT_AWAKE) setActivity(ACT_AWAKE);
  calStage = 1;
  calStartMs = millis();
  calResetAverage();
  react(faceWith(DEFAULT), 600000);  // neutral face while calibrating
  lookAt(0, -1);
  calPrompt("CALIBRATE 1/2", "stand me upright", 0);
  say(S_CONNECTION);
}

static void calFinish(bool ok, const char *why) {
  calStage = 0;
  reactionUntil = millis();
  if (ok) {
    calPrompt("Calibrated!", "saved to memory", 2500);
    say(S_SUPER_HAPPY);
  } else {
    calPrompt("Calibration failed", why, 3000);
    say(S_SAD);
  }
}

static void norm3(float v[3]) {
  float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (m > 1e-6f) for (int i = 0; i < 3; i++) v[i] /= m;
}

// Called after every IMU sample while calibrating.
void calibrationStep() {
  if (!calStage) return;
  if (since(calStartMs) > 120000) { calFinish(false, "timed out"); return; }

  // Average only while held perfectly still; any movement restarts the average.
  float m = sqrtf(rawAcc[0] * rawAcc[0] + rawAcc[1] * rawAcc[1] + rawAcc[2] * rawAcc[2]);
  if (jerk > 0.05f || fabsf(m - 1.0f) > 0.12f) { calResetAverage(); return; }

  if (calStage == 2) {  // must really have been turned onto its back first
    float d = (rawAcc[0] * calUp[0] + rawAcc[1] * calUp[1] + rawAcc[2] * calUp[2]) / m;
    if (d > 0.5f) { calResetAverage(); return; }
  }

  for (int i = 0; i < 3; i++) { calAcc[i] += rawAcc[i]; calGyro[i] += rawGyro[i]; }
  if (++calCount < 150) return;  // ~1.5 s of stillness
  for (int i = 0; i < 3; i++) { calAcc[i] /= calCount; calGyro[i] /= calCount; }

  if (calStage == 1) {
    memcpy(calUp, calAcc, sizeof(calUp));
    norm3(calUp);
    memcpy(gyroBias, calGyro, sizeof(gyroBias));
    calStage = 2;
    calResetAverage();
    calPrompt("CALIBRATE 2/2", "lay me on my back", 0);
    say(S_BUTTON_PUSHED);
    return;
  }

  // Out-of-screen axis = step-2 gravity with the "up" part removed.
  float out[3], dot = calAcc[0] * calUp[0] + calAcc[1] * calUp[1] + calAcc[2] * calUp[2];
  for (int i = 0; i < 3; i++) out[i] = calAcc[i] - dot * calUp[i];
  float om = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (om < 0.5f) { calFinish(false, "lay it flat on back"); return; }
  norm3(out);
  float right[3] = {calUp[1] * out[2] - calUp[2] * out[1],   // right = up x out
                    calUp[2] * out[0] - calUp[0] * out[2],
                    calUp[0] * out[1] - calUp[1] * out[0]};
  for (int i = 0; i < 3; i++) {
    imuRot[0][i] = right[i];
    imuRot[1][i] = calUp[i];
    imuRot[2][i] = out[i];
  }
  imuCalibrated = true;
  imuRestart = true;
  saveMounting();
  Serial.printf("[cal] right=(%.2f %.2f %.2f) up=(%.2f %.2f %.2f) out=(%.2f %.2f %.2f) gyroBias=(%.1f %.1f %.1f)\n",
                right[0], right[1], right[2], calUp[0], calUp[1], calUp[2], out[0], out[1], out[2],
                gyroBias[0], gyroBias[1], gyroBias[2]);
  calFinish(true, "");
}

// =====================================================================
//  BATTERY + POWER OFF
//  The XIAO charges the LiPo itself; we only watch the voltage on a divider.
//  Charging is detected from: USB to a computer, an optional 5V sense pin,
//  or the voltage jumping up / down when the cable goes in / out.
//  "Off" is deep sleep: screen + motion sensor asleep, the chip wakes every
//  couple of seconds for a few ms to see whether it was moved / plugged in.
// =====================================================================
RTC_DATA_ATTR uint8_t rtcOffReason = OFF_NONE;  // survives deep sleep
RTC_DATA_ATTR float rtcOffVolt = 0;
RTC_DATA_ATTR float battCal = 1.0f;  // learned ADC correction (saved in flash, kept in deep sleep)
RTC_DATA_ATTR float rtcRestVolt = 0;  // battery voltage while off, followed slowly (it relaxes upwards)
RTC_DATA_ATTR bool rtcUsbWas = false;  // computer on USB when it went off / at the last look
RTC_DATA_ATTR uint32_t rtcOffChecks = 0;
RTC_DATA_ATTR float rtcOffGrav[3] = {0, 0, 0};
RTC_DATA_ATTR uint8_t rtcImuAddr = 0;
OffReason wokeFrom = OFF_NONE;

bool battPresent = false;
float battV = 0, battVSlow = 0;
float battSecHist[8];  uint8_t battSecIdx = 0, battSecCount = 0;    // last 8 s
float battMinHist[21]; uint8_t battMinIdx = 0, battMinCount = 0;    // every 30 s, 10 min
uint32_t battLastMinPush = 0;
bool battCharging = false, battFull = false, battUsb = false;
const char *battWhy = "boot";  // why it thinks it is / isn't charging (shown by "batt")
uint32_t battChargeStart = 0, battUnplugAt = 0, battNextNag = 0, battNextNom = 0;
uint8_t battEmptyCount = 0;
uint32_t battJumpAt = 0;  float battJumpBase = 0;  // a possible plug-in, waiting to be confirmed
bool battFake = false;   // serial "fakebatt" for trying the reactions
float battPctEst = 100;
char battMsg1[22], battMsg2[22];

bool battSettling() {
  return activity == ACT_ASLEEP || activity == ACT_HIDING || since(battWokeAt) < minutes(10);
}

void battSetCal(float cal, const char *why) {
  float ratio = cal / battCal;
  battCal = cal;
  battV *= ratio; battVSlow *= ratio;  // rescale what we already measured
  for (auto &h : battSecHist) h *= ratio;
  for (auto &h : battMinHist) h *= ratio;
  prefs.begin("kitbit-batt", false);
  prefs.putFloat("cal", battCal);
  prefs.end();
  Serial.printf("[batt] calibrated (%s): correction x%.3f, now %.3f V\n", why, battCal, battV);
}

bool battOnBattery() { return battState == BATT_OK || battState == BATT_LOW || battState == BATT_CRITICAL; }

static const float LIPO_V[] = {3.30f, 3.50f, 3.64f, 3.68f, 3.71f, 3.75f, 3.80f, 3.85f, 3.92f, 4.00f, 4.10f, 4.20f};
static const float LIPO_P[] = {0,     5,     10,    20,    30,    40,    50,    60,    70,    80,    90,    100};

float lipoPercent(float v) {
  if (v <= LIPO_V[0]) return 0;
  for (size_t i = 1; i < COUNT(LIPO_V); i++)
    if (v < LIPO_V[i]) return LIPO_P[i - 1] + (LIPO_P[i] - LIPO_P[i - 1]) * (v - LIPO_V[i - 1]) / (LIPO_V[i] - LIPO_V[i - 1]);
  return 100;
}

float battReadVolts() {
#if ENABLE_BATTERY
  uint32_t mv = 0;
  for (int i = 0; i < 64; i++) mv += analogReadMilliVolts(PIN_BATTERY);  // average out ADC noise
  return mv / 64000.0f * BATT_DIVIDER * BATT_CAL * battCal;
#else
  return 0;
#endif
}

bool battUsbSense() {
#if PIN_USB_SENSE >= 0
  return digitalRead(PIN_USB_SENSE);
#elif ARDUINO_USB_CDC_ON_BOOT && SOC_USB_SERIAL_JTAG_SUPPORTED
  return HWCDC::isPlugged();  // only true when plugged into a computer, not a phone charger
#else
  return false;
#endif
}

// Is the divider actually there? A floating pin reads garbage, so pull it down
// for a moment: with the divider fitted the voltage stays up, floating it collapses.
bool battDividerFitted() {
#if ENABLE_BATTERY
  uint32_t plain = analogReadMilliVolts(PIN_BATTERY);  // also attaches the ADC
  gpio_pulldown_en((gpio_num_t)PIN_BATTERY);
  delay(5);
  uint32_t pulled = analogReadMilliVolts(PIN_BATTERY);
  gpio_pulldown_dis((gpio_num_t)PIN_BATTERY);
  delay(5);
  Serial.printf("[batt] D0 reads %lu mV (%lu mV with pull-down) -> battery %.2f V\n",
                (unsigned long)plain, (unsigned long)pulled, plain / 1000.0f * BATT_DIVIDER * BATT_CAL);
  return pulled > 100;  // a floating pin collapses to ~0 with the pull-down, a divider doesn't
#else
  return false;
#endif
}

void battText(const char *l1, const char *l2, uint32_t ms) {
  strlcpy(battMsg1, l1, sizeof battMsg1);
  strlcpy(battMsg2, l2, sizeof battMsg2);
  overlayLine1 = battMsg1;
  overlayLine2 = battMsg2;
  showOverlay(OV_TEXT, ms);
}

// ---- cute reactions ----
void onPluggedIn(BattState old) {
  battNextNom = millis() + random(180000, 360000);
  if (activity == ACT_ASLEEP) {  // sleepy munching: one eye peeks, back to sleep
    { BusLock lock; eyes.open(true, false); }
    peekCloseAt = millis() + 900;
    say(SND_NOM, 2);
    return;
  }
  bool starving = (old == BATT_LOW || old == BATT_CRITICAL);
  interact();
  react(faceWith(HAPPY), 2500);
  laughEyes();
  say(SND_NOM, 1, true);
  addHappiness(starving ? 10 : 3);
}

void onFull() {
  if (activity != ACT_AWAKE) return;  // the badge shows it's full anyway
  react(faceWith(HAPPY), 3000);
  laughEyes();
  battText("I'm full!", "  *burp*", 3000);
  say(SND_BURP);
  addHappiness(3);
}

void onUnplugged(BattState now) {
  battBadgeUntil = millis() + 6000;
  if (activity == ACT_ASLEEP) return;
  interact();
  if (now == BATT_OK) {
    react(faceWith(HAPPY), 1800);
    snprintf(battMsg2, sizeof battMsg2, "wheee! %d%%", battPct);
    battText("unplugged!", battMsg2, 1800);
    say(SND_UNPLUG);
  } else {
    react(faceWith(TIRED), 2500);
    battText("hey! I'm still", "hungry...", 2500);
    say(SND_SIGH);
  }
}

void hungry(bool critical) {
  battNextNag = millis() + minutes(critical ? 3 : BATT_NAG_MIN);
  if (activity == ACT_ASLEEP || activity == ACT_HIDING || calStage) return;
  if (activity == ACT_DROWSY) setActivity(ACT_AWAKE);
  Face f; f.mood = TIRED; f.sweat = critical;
  react(f, 3000);
  glance(0, -0.4f, 3000);  // looks up at you, pleading
  snprintf(battMsg2, sizeof battMsg2, "battery %d%%", battPct);
  battText(critical ? "feed me please!" : "I'm hungry...", battMsg2, 3000);
  say(SND_TUMMY, 1, critical);
  Serial.printf("[batt] hungry! %d%%\n", battPct);
}

void battSetState(bool charging, bool full, float pct) {
  BattState old = battState, s;
  if (full) s = BATT_FULL;
  else if (charging) s = BATT_CHARGING;
  else if (pct <= BATT_CRITICAL_PCT || (old == BATT_CRITICAL && pct < BATT_CRITICAL_PCT + 3)) s = BATT_CRITICAL;
  else if (pct <= BATT_LOW_PCT || ((old == BATT_LOW || old == BATT_CRITICAL) && pct < BATT_LOW_PCT + 3)) s = BATT_LOW;
  else s = BATT_OK;
  battPct = (uint8_t)clampf(pct, 0, 100);
  battSave = (s == BATT_CRITICAL);
  if (s == old) return;
  battState = s;
  faceDirty = true;
  Serial.printf("[batt] %s -> %s  %d%%  %.2f V\n", BATT_NAMES[old], BATT_NAMES[s], battPct, battV);
  if (old == BATT_NONE || activity == ACT_HIDING || calStage || bootPending) return;  // no fuss at boot

  bool wasPlugged = (old == BATT_CHARGING || old == BATT_FULL);
  bool plugged = (s == BATT_CHARGING || s == BATT_FULL);
  if (!wasPlugged && plugged) onPluggedIn(old);
  else if (old == BATT_CHARGING && s == BATT_FULL) onFull();
  else if (wasPlugged && !plugged) onUnplugged(s);
  else if (s == BATT_LOW && old == BATT_OK) hungry(false);
  else if (s == BATT_CRITICAL) hungry(true);
}

void battMeasure() {
  uint32_t now = millis();
  float v = battReadVolts();
  battV += 0.4f * (v - battV);          // ~2 s smoothing
  battVSlow += 0.05f * (v - battVSlow); // ~20 s smoothing, for the percentage

  float oldest = battSecHist[battSecIdx];
  battSecHist[battSecIdx] = battV;
  battSecIdx = (battSecIdx + 1) % 8;
  if (battSecCount < 8) battSecCount++;
  float jump = battSecCount >= 8 ? battV - oldest : 0;  // change over the last 8 s

  if (since(battLastMinPush) >= 30000) {
    battLastMinPush = now;
    battMinHist[battMinIdx] = battV;
    battMinIdx = (battMinIdx + 1) % 21;
    if (battMinCount < 21) battMinCount++;
  }
  auto minAgo = [](int steps) { return battMinHist[(battMinIdx + 21 - 1 - steps) % 21]; };
  float trend5 = battMinCount > 10 ? battV - minAgo(10) : 0;   // over 5 min
  float rise10 = battMinCount >= 21 ? battV - minAgo(20) : 1;  // over 10 min

  // The USB "connected" flag needs a few seconds after boot to become reliable.
  bool usb = millis() > 5000 && battUsbSense();
  bool charging = battCharging;
  const char *why = battWhy;
#if PIN_USB_SENSE >= 0
  charging = usb; why = usb ? "5V sense pin high" : "5V sense pin low";
#else
  if (usb) { charging = true; why = "USB computer connected"; }
  else if (battUsb) { charging = false; why = "USB computer disconnected"; }
  // A charger pushes the voltage up quickly and keeps it up. A battery that just has less
  // to do (asleep, dimmed screen) also creeps up a bit, so: bigger thresholds, and a jump
  // only counts if it is still there 20 s later.
  // Asleep / hiding, or just back from sleep: the battery voltage drifts on its own,
  // so use the long check (bigger jump, confirmed after 60 s, no slow-rise rule).
  // The rest of the time the normal check (confirmed after 20 s).
  else if (!charging && battJumpAt && since(battJumpAt) >= (battSettling() ? 60000UL : 20000UL)) {
    if (battV - battJumpBase > (battSettling() ? 0.10f : 0.07f)) { charging = true; why = "voltage jumped up and stayed up"; }
    battJumpAt = 0;
  }
  else if (!charging && !battJumpAt && jump > (battSettling() ? 0.10f : 0.08f)) { battJumpAt = now; battJumpBase = oldest; }
  else if (!charging && !battSettling() && trend5 > 0.07f && since(battUnplugAt) > minutes(6)) { charging = true; why = "voltage rising for 5 min"; }
  else if (charging && jump < (battFull ? -0.03f : -0.06f)) { charging = false; why = "voltage dropped"; }  // a full cell only drops a little
  else if (charging && trend5 < -0.01f && since(battChargeStart) > minutes(3)) { charging = false; why = "voltage falling"; }
#endif
  battUsb = usb;
  if (charging != battCharging) Serial.printf("[batt] %s: %s (%.3f V, jump %+.3f, 5-min trend %+.3f)\n",
                                              charging ? "charging" : "not charging", why, battV, jump, trend5);
  battWhy = why;

  if (charging != battCharging) {
    battCharging = charging;
    battJumpAt = 0;
    battMinCount = 0;  // start the trends fresh
    battLastMinPush = now;
    if (charging) battChargeStart = now;
    else { battUnplugAt = now; battVSlow = battV; }
  }

  // Self-calibration: charging, and the voltage hasn't moved for 10 min -> the charger
  // is holding the cell at exactly BATT_CV_V. (The ADC + 5 % resistors read a few % off.)
  if (charging && since(battChargeStart) > minutes(15) && battMinCount >= 21 && fabsf(rise10) < 0.005f) {
    float cal = clampf(battCal * BATT_CV_V / battV, 0.90f, 1.10f);
    if (fabsf(cal / battCal - 1.0f) > 0.005f) battSetCal(cal, "charging plateau");
  }
  if (!charging) battFull = false;
  else if (!battFull && battV >= BATT_FULL_V && rise10 < 0.01f) battFull = true;  // stopped rising

  if (battFull) battPctEst = 100;
  else if (charging) {
    // Charging pushes the voltage up ~0.1 V: estimate from below, and only ever count up.
    float p = min(99.0f, lipoPercent(battVSlow - 0.10f));
    if (p > battPctEst) battPctEst = p;
  } else {
    float p = lipoPercent(battVSlow);
    // On battery the number only goes down (no bouncing), except while the voltage settles after unplugging.
    if (since(battUnplugAt) < 90000 || p < battPctEst) battPctEst = p;
  }
  battSetState(charging, battFull, battPctEst);
}

void battBegin() {
#if ENABLE_BATTERY
#if PIN_USB_SENSE >= 0
  pinMode(PIN_USB_SENSE, INPUT);
#endif
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
  if (!battDividerFitted()) {
    Serial.println(F("[batt] no voltage divider on PIN_BATTERY - battery features off (see config.h)"));
    return;
  }
  float v = battReadVolts();
  if (v < 2.5f) { Serial.printf("[batt] reads %.2f V - no battery? battery features off\n", v); return; }
  battPresent = true;
  prefs.begin("kitbit-batt", true);
  battCal = prefs.getFloat("cal", 1.0f);
  prefs.end();
  if (fabsf(battCal - 1.0f) > 0.001f) {
    Serial.printf("[batt] learned correction x%.3f\n", battCal);
    v = battReadVolts();
  }
  battV = battVSlow = v;
  for (auto &h : battSecHist) h = v;
  // Don't trust the USB flag or a high voltage right at boot (both misled it when the
  // battery was connected first). Start as "on battery"; USB / voltage changes decide
  // within seconds. Exception: waking up from the empty-battery sleep = charger plugged in.
  battUsb = false;
  battCharging = (wokeFrom == OFF_BATTERY);
  battWhy = battCharging ? "woke up from empty: charger plugged in" : "boot: assuming on battery";
  uint32_t now = millis();
  battChargeStart = battUnplugAt = battLastMinPush = now;
  battPctEst = lipoPercent(v);
  battNextNom = now + minutes(3);
  battSetState(battCharging, false, battPctEst);
#endif
}

void battStep() {  // once per second
  if (!battPresent && !battFake) return;
  uint32_t now = millis();
  if (!battFake) battMeasure();

  // Over-discharge protection
  bool empty = battFake ? battPct == 0 : battV < BATT_SHUTDOWN_V;
  if (battOnBattery() && empty) {
    if (++battEmptyCount >= BATT_SHUTDOWN_S) powerOff(OFF_BATTERY, true);
  } else {
    battEmptyCount = 0;
  }

  if (calStage || bootPending || reacting()) return;
  if ((battState == BATT_LOW || battState == BATT_CRITICAL) && reached(battNextNag)) hungry(battState == BATT_CRITICAL);
  if (battState == BATT_CHARGING && activity == ACT_AWAKE && reached(battNextNom)) {  // happy munching
    battNextNom = now + random(240000, 480000);
    react(faceWith(HAPPY), 1500);
    chatter(SND_NOM);
  }
}

// ---- power off (deep sleep) ----
void powerOff(OffReason why, bool animate) {
  Serial.printf("[power] switching off (%s)\n", why == OFF_BATTERY ? "battery empty" : "goodnight");
  if (animate && activity != ACT_HIDING) {
    hush();
    Face f; f.mood = TIRED;
    react(f, 10000);
    lookAt(0, 0.3f);
    if (why == OFF_BATTERY) battText("too hungry...", "plug me in! zzz", 0);
    else battText("goodnight!", "plug me in to wake", 0);
    say(SND_GOODNIGHT, 1, true);
    delay(2800);
    { BusLock lock; eyes.close(); }
    delay(900);
  }
  hush();
  saveMemory();

  rtcOffReason = why;
  rtcOffVolt = battPresent ? battV : 0;
  rtcRestVolt = 0;
  rtcUsbWas = battUsbSense();
  rtcOffChecks = 1;  // the first look at USB comes ~12 s after switching off
  rtcImuAddr = imuOk ? imuAddr : 0;
  if (imuOk) {
    float m = sqrtf(rawAcc[0] * rawAcc[0] + rawAcc[1] * rawAcc[1] + rawAcc[2] * rawAcc[2]);
    for (int i = 0; i < 3; i++) rtcOffGrav[i] = m > 0.1f ? rawAcc[i] / m : 0;
  }

  xSemaphoreTakeRecursive(busMutex, portMAX_DELAY);  // stops the eyes task for good
  oledCommand(OLED_OFF_CMD);
  if (imuOk) imuWrite(0x6B, 0x40);  // motion sensor to sleep
  noTone(PIN_BUZZER);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)(why == OFF_BATTERY ? BATT_OFF_CHECK_S : OFF_CHECK_S) * 1000000ULL);
  esp_deep_sleep_start();
}

#if OFF_WAKE_BY_TAP
// While "off" and just moved: show sleepy eyes + "tap tap to wake" and listen for a
// double tap with the IMU alone (the mic task isn't running yet). True = wake up.
static bool offListenForDoubleTap() {
  if (!rtcImuAddr) return false;
  imuWrite(0x6B, 0x01);  // wake the sensor (it kept its settings while asleep)
  imuWrite(0x6A, 0x00);  // FIFO off: plain register reads here
  delay(30);
  Wire.setClock(I2C_IDLE_HZ);
  bool screen = oledBegin();
  if (screen) {
    display.setRotation(DISPLAY_ROTATION);
    display.setTextWrap(false);
    display.clearDisplay();
    int cy = (SCREEN_HEIGHT - SCREEN_HIDDEN_BOTTOM) / 2 - 8;
    int lx = SCREEN_WIDTH / 2 - EYE_GAP / 2 - EYE_W, rx = SCREEN_WIDTH / 2 + EYE_GAP / 2;
    display.fillRoundRect(lx, cy, EYE_W, 4, 2, OLED_WHITE);  // closed, sleepy eyes
    display.fillRoundRect(rx, cy, EYE_W, 4, 2, OLED_WHITE);
    display.setTextSize(1);
    display.setTextColor(OLED_WHITE);
    display.setCursor(19, cy + 18);
    display.print(F("tap tap to wake"));
    display.display();
    oledDim(true);
  }
  float g[3] = {0, 0, 0};
  bool haveG = false, armed = true;
  uint32_t start = millis(), firstTap = 0, lastTap = 0;
  bool woke = false;
  while (millis() - start < OFF_LISTEN_MS) {
    uint8_t b[6];
    if (imuRead(0x3B, b, 6)) {
      float s[3], d[3], m = 0;
      for (int k = 0; k < 3; k++) s[k] = (int16_t)(b[k * 2] << 8 | b[k * 2 + 1]) / 8192.0f;
      static float hfPrev[3], hfOut[3];
      static const float A = 1.0f / (1.0f + 2.0f * PI * HF_CUTOFF_HZ * 0.005f);  // ~200 Hz here
      if (!haveG) { memcpy(g, s, sizeof g); memcpy(hfPrev, s, sizeof s); memset(hfOut, 0, sizeof hfOut); haveG = true; }
      for (int k = 0; k < 3; k++) {
        g[k] += (s[k] - g[k]) * 0.02f;  // slow gravity, for "put face down again"
        hfOut[k] = A * (hfOut[k] + s[k] - hfPrev[k]);
        hfPrev[k] = s[k];
        d[k] = hfOut[k];
        m += d[k] * d[k];
      }
      m = sqrtf(m);
      uint32_t now = millis();
      if (armed && m > OFF_TAP_G) {
        armed = false;
        lastTap = now;
        if (firstTap && now - firstTap >= 80 && now - firstTap <= 700) { woke = true; break; }
        firstTap = now;
      }
      if (!armed && m < OFF_TAP_G * 0.5f && now - lastTap > 100) armed = true;
      if (firstTap && now - firstTap > 700) firstTap = 0;
      // Put face down again: no.
      float gm = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), dot = 0;
      for (int k = 0; k < 3; k++) dot += g[k] / gm * rtcOffGrav[k];
      if (now - start > 500 && dot > cosf(OFF_WAKE_DEG * 0.5f * DEG_TO_RAD)) break;
    }
    delay(4);
  }
  if (!woke) {
    if (screen) oledCommand(OLED_OFF_CMD);
    // Remember where it is now: it only listens again after the next move.
    float gm = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (haveG && gm > 0.3f) for (int k = 0; k < 3; k++) rtcOffGrav[k] = g[k] / gm;
    imuWrite(0x6B, 0x40);
  }
  return woke;
}

#endif

// Is a computer on the USB port? It sends a start-of-frame every 1 ms once it has
// noticed the board again after the deep sleep (100-200 ms). A phone charger sends nothing.
static bool usbHostPresent() {
  uint32_t f0 = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX;
  for (int i = 0; i < 150; i++) {
    delay(2);
    if ((REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX) != f0) return true;
  }
  return false;
}

// First thing at boot: if we were "off", take a quick look and go back to sleep.
//  - switched off by hand: wake when a charger / computer is plugged in
//    (and, with OFF_WAKE_BY_TAP, for "moved, then double-tapped")
//  - battery empty: wake only when a charger was plugged in
void offModeCheck() {
  if (rtcOffReason == OFF_NONE) return;
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) { rtcOffReason = OFF_NONE; return; }  // RESET / power cycle

  bool wake = false;
#if ENABLE_BATTERY
  float v = battReadVolts();
  if (rtcOffVolt > 2.5f && v > 2.5f) {
    if (rtcOffReason == OFF_BATTERY && v > rtcOffVolt + 0.25f) wake = true;              // charger plugged in
    else if (rtcOffReason == OFF_USER && v < BATT_SHUTDOWN_V) rtcOffReason = OFF_BATTERY;  // drained while off
    else if (rtcOffReason == OFF_USER) {
      if (rtcRestVolt < 2.5f) rtcRestVolt = v;
      if (v > rtcRestVolt + OFF_CHARGE_RISE_V) wake = true;  // voltage jumped: a charger was plugged in
      else rtcRestVolt += (v - rtcRestVolt) * 0.3f;          // follow the slow relaxation of the cell
    }
  }
#endif
#if PIN_USB_SENSE >= 0
  pinMode(PIN_USB_SENSE, INPUT);
  if (!wake && rtcOffReason == OFF_USER && digitalRead(PIN_USB_SENSE)) wake = true;
#endif
  // Plugged into a computer (only a NEW connection counts: it may have been switched off
  // while on USB). Looking takes ~300 ms, so only every 4th check.
  if (!wake && rtcOffReason == OFF_USER && rtcOffChecks++ % 4 == 0) {
    bool usb = usbHostPresent();
    if (usb && !rtcUsbWas) wake = true;
    rtcUsbWas = usb;
  }
#if OFF_WAKE_BY_TAP
  if (!wake && rtcOffReason == OFF_USER && rtcImuAddr) {
    Wire.begin(PIN_SDA, PIN_SCL);
    imuAddr = rtcImuAddr;
    imuWrite(0x6B, 0x00);  // wake the sensor for one reading
    delay(30);
    uint8_t b[6];
    bool ok = imuRead(0x3B, b, 6);
    bool moved = false;
    if (ok) {
      float a[3], m = 0;
      for (int i = 0; i < 3; i++) { a[i] = (int16_t)(b[i * 2] << 8 | b[i * 2 + 1]); m += a[i] * a[i]; }
      m = sqrtf(m);
      float dot = 0;
      if (m > 0) for (int i = 0; i < 3; i++) dot += a[i] / m * rtcOffGrav[i];
      moved = dot < cosf(OFF_WAKE_DEG * DEG_TO_RAD);  // turned over / picked up
    }
    if (moved) wake = offListenForDoubleTap();
    else imuWrite(0x6B, 0x40);
  }
#endif
  if (wake) {
    wokeFrom = (OffReason)rtcOffReason;
    rtcOffReason = OFF_NONE;
    return;
  }
  esp_sleep_enable_timer_wakeup((uint64_t)(rtcOffReason == OFF_BATTERY ? BATT_OFF_CHECK_S : OFF_CHECK_S) * 1000000ULL);
  esp_deep_sleep_start();
}

void printBattery() {
  if (!battPresent && !battFake) { Serial.println(F("battery: not measured (no divider on PIN_BATTERY)")); return; }
  Serial.printf("battery: %s %d%%  %.3f V (slow %.3f)  usb=%d (raw %d)  why: %s%s\n", BATT_NAMES[battState], battPct,
                battV, battVSlow, battUsb, battUsbSense(), battWhy, battFake ? "  [fake]" : "");
}

// =====================================================================
//  SERIAL CONSOLE (115200 baud) - for testing and tuning
// =====================================================================
void printStatus() {
  Serial.printf("activity=%s orient=%s happiness=%.0f energy=%.0f age=%luh\n",
                ACTIVITY_NAMES[activity], ORIENT_NAMES[orient], happiness, energy, (unsigned long)(ageMinutes / 60));
  Serial.printf("present=%d breakDue=%d work=%lumin quiet=%lus chattiness=%d muted=%d imu=%d calibrated=%d overlay=%s\n",
                present(), breakDue, workActive ? (unsigned long)(since(workStart) / 60000) : 0UL,
                (unsigned long)(since(lastInteraction) / 1000), chattiness, muted, imuOk, imuCalibrated, OVERLAY_NAMES[overlay]);
  Serial.printf("gyro bias %.2f %.2f %.2f dps\n", gyroBias[0], gyroBias[1], gyroBias[2]);
  printBattery();
}

void printHelp() {
  Serial.println(F("commands: status imu mic calibrate calreset mute unmute chatty <0-3> sleep wake pet boop sing"));
  Serial.println(F("          scare annoy dizzy"));
  Serial.println(F("          hiccup sneeze greet break birthday happy sad angry sound <id> reset"));
  Serial.println(F("          batt  off  fakebatt <0-100|charging|full|auto>"));
}

void runCommand(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd == "help") printHelp();
  else if (cmd == "status") printStatus();
  else if (cmd == "imu") { imuStream = !imuStream; }
  else if (cmd.startsWith("note ")) {  // note <hz> <ms>: tools/melody.py --play
    int sp = cmd.indexOf(' ', 5);
    int hz = cmd.substring(5, sp).toInt(), ms = sp > 0 ? cmd.substring(sp + 1).toInt() : 200;
    if (hz >= 50 && hz <= 8000) tone(PIN_BUZZER, hz, constrain(ms, 10, 3000));
  }
  else if (cmd.startsWith("audiodump ")) {
    int sec = constrain(cmd.substring(10).toInt(), 1, 120);
    Serial.printf("I,audiodump,%d\n", sec);
    Serial.flush();
    audioDumpUntil = millis() + sec * 1000UL;
  }
  else if (cmd == "offtest") { offTest = !offTest; Serial.printf("I,offtest,%d\n", offTest); }
  else if (cmd == "tapdebug") { tapDebug = !tapDebug; Serial.printf("I,tapdebug,%d,mic,%d\n", tapDebug, micOk); }
  else if (cmd.startsWith("prompt")) {  // prompt <line1>|<line2>  (empty = clear)
    static char p1[22], p2[22];
    String t = cmd.length() > 7 ? cmd.substring(7) : String();
    int bar = t.indexOf('|');
    strlcpy(p1, (bar >= 0 ? t.substring(0, bar) : t).c_str(), sizeof p1);
    strlcpy(p2, bar >= 0 ? t.substring(bar + 1).c_str() : "", sizeof p2);
    if (t.length()) { overlayLine1 = p1; overlayLine2 = p2; showOverlay(OV_TEXT, 0); }
    else clearOverlay(OV_TEXT);
  }
  else if (cmd == "ears") { earsStream = !earsStream; }
  else if (cmd == "mic") { micStream = !micStream; if (!micOk) Serial.println(F("microphone not running")); }
  else if (cmd == "calibrate") startCalibration();
  else if (cmd == "calreset") { clearMounting(); Serial.println(F("Calibration cleared - using config.h mapping")); }
  else if (cmd == "mute") { muted = true; hush(); }
  else if (cmd == "unmute") { muted = false; }
  else if (cmd.startsWith("chatty ")) { chattiness = constrain(cmd.substring(7).toInt(), 0, 3); }
  else if (cmd == "sleep") fallAsleep();
  else if (cmd == "wake") { interact(); wakeUp(false); }
  else if (cmd == "pet") { if (activity != ACT_AWAKE) setActivity(ACT_AWAKE); petMe(); }
  else if (cmd == "boop") boop(random(2) ? 1 : -1);
  else if (cmd == "sing") sing();
  else if (cmd == "scare") startle();
  else if (cmd == "annoy") annoyed();
  else if (cmd == "dizzy") { dizzy = 4; dizzyShown = false; }
  else if (cmd == "hiccup") { hiccupsLeft = 6; nextHiccupAt = millis(); }
  else if (cmd == "sneeze") { sneezeStage = 1; sneezeAt = millis(); }
  else if (cmd == "greet") greetUser();
  else if (cmd == "break") { breakDue = true; nextBreakNagAt = millis(); workActive = true; workStart = millis(); }
  else if (cmd == "birthday") celebrateBirthday();
  else if (cmd == "happy") { happiness = 95; faceDirty = true; }
  else if (cmd == "sad") { happiness = 15; faceDirty = true; }
  else if (cmd == "angry") react(faceWith(ANGRY), 3000);
  else if (cmd.startsWith("sound ")) { int id = cmd.substring(6).toInt(); xQueueSend(soundQueue, &id, 0); }
  else if (cmd == "reset") { happiness = 70; energy = 90; saveMemory(); }
  else if (cmd == "batt" || cmd == "battery") printBattery();
  else if (cmd.startsWith("battcal ")) {  // battcal <volts measured with a multimeter at BAT+>
    float real = cmd.substring(8).toFloat();
    if (real > 2.5f && real < 4.4f && battV > 2.5f) battSetCal(clampf(battCal * real / battV, 0.85f, 1.15f), "multimeter");
    else if (cmd.substring(8) == "reset") battSetCal(1.0f, "reset");
    else Serial.println(F("usage: battcal <volts>  or  battcal reset"));
  }
  else if (cmd == "off") powerOff(OFF_USER, true);
  else if (cmd.startsWith("fakebatt ")) {
    String a = cmd.substring(9);
    if (a == "auto") { battFake = false; Serial.println(F("battery: measuring again")); }
    else {
      battFake = true;
      if (a == "charging") battSetState(true, false, battPct);
      else if (a == "full") battSetState(true, true, 100);
      else battSetState(false, false, constrain(a.toInt(), 0, 100));
    }
  }
  else if (cmd.length()) { Serial.print(F("unknown: ")); Serial.println(cmd); printHelp(); }
}

void serialStep() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) runCommand(line);
      line = "";
    } else if (line.length() < 64) {
      line += c;
    }
  }
  static uint32_t lastEarsPrint = 0;
  if (earsStream && since(lastEarsPrint) > 500) {
    lastEarsPrint = millis();
    static const char *NN[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    Serial.printf("ears music=%d loud=%4.1fdB cont=%.2f beat=%.2f/%.2f tonal=%.2f steady=%.2f keyfit=%.2f sung=%.2f sing=%d bpm=%5.1f key=%s%s chord=%s%s bass=%s%s pitch=%s%.1f\n",
                  ears.st.music, ears.st.loudAvg, ears.st.continuity, ears.st.beatStrength, ears.st.beatAvg, ears.st.tonalAvg, ears.st.steady, ears.st.keyCorr, ears.st.sung, ears.st.singing, ears.st.bpm,
                  ears.st.key >= 0 ? NN[ears.st.key] : "-", ears.st.keyMinor ? "m" : "",
                  ears.st.chord >= 0 ? NN[ears.st.chord] : "-", ears.st.chordMinor ? "m" : "",
                  ears.st.bass >= 0 ? NN[ears.st.bass] : "-", ears.st.bassMinor ? "m" : "",
                  ears.st.pitch ? (ears.st.pitchKind == 1 ? "whistle " : "hum ") : "-", ears.st.pitch ? ears.st.pitchMidi : 0.0f);
  }
  static uint32_t lastMicPrint = 0;
  if (micStream && micOk && since(lastMicPrint) > 200) {
    lastMicPrint = millis();
    Serial.printf("mic background=%6.0f  loudest=%7.0f  click threshold=%7.0f  clicks=%lu\n", micBg, micPeakMax,
                  max((float)MIC_CLICK_MIN, micBg * MIC_CLICK_RATIO), (unsigned long)micClickCount);
    micPeakMax = 0;
  }
  static uint32_t lastPrint = 0;
  if (imuStream && imuOk && since(lastPrint) > 100) {
    lastPrint = millis();
    Serial.printf("right=%+.2f up=%+.2f out=%+.2f |g|=%.2f gyro=%5.0f tap=%.2f jerk=%.3f vib=%.4f floor=%.4f desk=%d hold=%d shake=%.1f dizzy=%.1f %s\n",
                  grav.x, grav.y, grav.z, accMag, gyroMag, tapPeak, jerk, vib, vibFloor, present(), handling, shakeMeter, dizzy,
                  ORIENT_NAMES[orient]);
    tapPeak = 0;  // peak kick since the last line
  }
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  offModeCheck();  // if we were switched off: only wake up when moved / plugged in
  Serial.begin(115200);
  randomSeed(esp_random());
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(I2C_IDLE_HZ);

  if (!oledBegin()) {
    Serial.println(F("OLED init failed - check wiring / OLED_ADDR / OLED_SH1106"));
    for (;;) delay(1000);
  }
  display.clearDisplay();
  display.display();
  display.setRotation(DISPLAY_ROTATION);
  display.setTextWrap(false);  // text near an edge must not reappear on the other side

  delay(1500);  // give USB serial a moment so the boot log is visible
  i2cScan();
  loadMounting();
  imuOk = imuBegin();
  if (imuOk) imuMeasureGyroBias();
  if (!imuOk) Serial.println(F("Motion sensor not found at 0x68/0x69 - running without motion sensing"));

  cute.init(PIN_BUZZER);
  loadMemory();
  battBegin();

  eyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, EYES_FPS);
  eyes.autoDisplayEnabled = false;  // the eyes task pushes the frame after drawing overlays
  eyes.setDisplayColors(0, 1);
  eyes.setWidth(EYE_W, EYE_W);
  eyes.setHeight(EYE_H, EYE_H);
  eyes.setSpacebetween(EYE_GAP);
  eyes.setBorderradius(EYE_RADIUS, EYE_RADIUS);
  eyes.setPosition(DEFAULT);
  eyes.setAutoblinker(OFF);
  eyes.close();  // wake-up animation starts with closed eyes
  shown.open = false;

#if ENABLE_TOUCH
  touchSetup();
#endif

  busMutex = xSemaphoreCreateRecursiveMutex();
  soundQueue = xQueueCreate(64, sizeof(int));  // room for a whole repeated tune
  // Core 0 exists on every ESP32 (the C3 is single-core; dual-core chips run loop() on core 1).
  xTaskCreatePinnedToCore(eyesTask, "eyes", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(soundTask, "sound", 4096, NULL, 1, NULL, 0);
  micBegin();

  uint32_t now = millis();
  lastInteraction = now;
  lastImuMs = now;
  activitySince = now;
  bootAt = now + 800;
  scheduleQuirk();

  Serial.printf("KitBit is alive! happiness=%.0f energy=%.0f age=%luh. Type 'help'.\n",
                happiness, energy, (unsigned long)(ageMinutes / 60));
}

void loop() {
  static uint32_t lastImu = 0, lastBrain = 0, lastNeeds = 0, lastAge = 0, lastSave = 0, lastTouch = 0;
  uint32_t now = millis();

  // Face down: power save until it's turned over (or double-tapped off).
  if (imuOk && activity == ACT_HIDING && !bootPending && !calStage && reached(psHoldoffUntil)) {
    powerSaveStep();
    return;
  }
  if (powerSave) exitPowerSave();  // left hiding some other way (serial "wake" ...)

  if (imuOk && now - lastImu >= 10) {  // ~100 Hz
    lastImu = now;
    imuStep();
    calibrationStep();
  }
  serialStep();

  if (bootPending) {
    if (reached(bootAt)) {
      bootPending = false;
      faceDirty = true;
      react(faceWith(HAPPY), 1500);
      laughEyes();
      say(SND_BOOT);
      if (wokeFrom == OFF_USER) showOverlay(OV_HEARTS, 1800);  // good morning!
      else if (wokeFrom == OFF_BATTERY) battText("FOOD!! thank you!", "nom nom nom", 2500);
    }
    vTaskDelay(1);
    return;
  }

#if ENABLE_TOUCH
  if (now - lastTouch >= 50) { lastTouch = now; touchStep(); }
#else
  (void)lastTouch;
#endif

  if (now - lastBrain >= 20) {  // 50 Hz
    lastBrain = now;
    if (calStage) {  // the pet holds still while it is being calibrated
      ev = MotionEvents();
      vTaskDelay(1);
      return;
    }
    handleMotionEvents();
    hidingDoubleTapStep();
    presenceStep();
    breakBuddyStep();
    activityStep();
    effectsStep();
    scratchStep();
    musicStep();
    repeatStep();
    quirkStep();
    baselineFace();
    gazeStep();
  }
  if (now - lastNeeds >= 1000) { lastNeeds = now; needsTick(); battStep(); }
  if (now - lastAge >= 60000) { lastAge = now; ageTick(); }
  if (now - lastSave >= minutes(SAVE_EVERY_MIN)) { lastSave = now; saveMemory(); }

  vTaskDelay(1);
}
