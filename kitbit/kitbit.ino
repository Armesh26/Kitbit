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
//   - "calibrate" (serial) or 6 quick taps: learns how the sensor is mounted, saved to flash
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
#include "config.h"

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
      if (id < 100) {
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
        int x = 96 + (int)(p * 22);
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
      display.fillRect(0, 43, SCREEN_WIDTH, 21, OLED_BLACK);
      display.setTextSize(1);
      display.setTextColor(OLED_WHITE, OLED_BLACK);
      display.setCursor(1, 45);
      display.print((const char *)overlayLine1);
      display.setCursor(1, 55);
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
  const int x = 57, y = 56;  // 14x8 body + 2px nub
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
bool shaking = false;   float shakeMeter = 0;  uint32_t shakeStartMs = 0;
float dizzy = 0;
bool falling = false;   uint8_t lowGCount = 0, normalCount = 0;  uint32_t fallStartMs = 0;
uint8_t tapCount = 0;   float tapSide = 0;  uint32_t lastTapMs = 0, lastTapCandMs = 0;
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
  imuWrite(0x19, 0x09);  // SMPLRT_DIV: 1 kHz / (1+9) = 100 Hz
  imuWrite(0x1A, 0x03);  // CONFIG: DLPF ~44 Hz
  imuWrite(0x1B, 0x08);  // GYRO_CONFIG: +-500 dps
  imuWrite(0x1C, 0x08);  // ACCEL_CONFIG: +-4 g
  if (imuWho != 0x68) imuWrite(0x1D, 0x03);  // ACCEL_CONFIG2 (6500 family): accel DLPF ~44 Hz
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

void imuStep() {
  float sa[3], sg[3];
  {
    BusLock lock;
    if (!imuSample(sa, sg)) return;
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
  if (jerk < 0.02f && dynMag < 0.03f && fabsf(accMag - 1.0f) < 0.08f && gyroMag < 8.0f) {
    for (int i = 0; i < 3; i++) gyroBias[i] += (sg[i]) * 0.002f;
  }

  // --- being held / carried ---
  bool moving = gyroMag > HANDLE_GYRO_DPS || dynMag > 0.12f;
  handleScore += ((moving ? 1.0f : 0.0f) - handleScore) * 0.05f;
  if (!handling && handleScore > 0.45f) { handling = true; ev.pickup = true; tapCount = 0; }
  else if (handling && handleScore < 0.08f) { handling = false; ev.putdown = true; }

  // --- shaking ---
  shakeMeter = shakeMeter * 0.95f + (dynMag > 0.6f ? dynMag * 0.12f : 0.0f);
  if (!shaking && shakeMeter > SHAKE_START) { shaking = true; shakeStartMs = now; ev.shakeStart = true; }
  else if (shaking && shakeMeter < 0.4f) { shaking = false; ev.shakeEnd = true; ev.shakeMs = now - shakeStartMs; }

  // --- dizziness builds up from shaking and spinning ---
  float spin = fabsf(gyr.y);
  if (shaking) dizzy += dt * shakeMeter * 0.6f;
  if (spin > SPIN_DPS) dizzy += dt * spin / SPIN_DPS;
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
  if (!handling && !falling && !shaking && now - lastTapCandMs > TAP_DEBOUNCE_MS) {
    if (jerk > BUMP_JERK_G) {
      ev.bump = true;
      lastTapCandMs = now;
      tapCount = 0;
    } else if (jerk > TAP_JERK_G) {
      tapCount++;
      tapSide += dyn.x;
      lastTapMs = lastTapCandMs = now;
    }
  }
  if (tapCount && now - lastTapMs > TAP_WINDOW_MS) {
    if (!handling) { ev.taps = tapCount; ev.tapSide = tapSide; }
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
  addHappiness(8);
}

void boop(float side) {
  interact();
  blinkEyes();
  // The tap pushes the pet away from where it came from: look towards the finger.
  glance(side > 0 ? -1.0f : 1.0f, frand(-0.3f, 0.2f), 1200);
  say(SND_CHIRP);
  addHappiness(2);
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
  addHappiness(5);
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

void onTaps(uint8_t n, float side) {
  if (activity == ACT_HIDING) return;
  if (n >= 6) { interact(); startCalibration(); return; }  // 6 quick taps = calibrate mounting

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
        react(faceWith(TIRED), 2000);
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
    if (e.shakeMs > 3000) { grumpyPending = true; addHappiness(-12); }
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

  if (e.taps) onTaps(e.taps, e.tapSide);
}

// ---------------- needs, sleep and the rest of daily life ----------------
void needsTick() {  // once per second
  bool resting = (activity == ACT_ASLEEP || activity == ACT_HIDING);
  bool eating = (battState == BATT_CHARGING || battState == BATT_FULL);  // plugged in = being fed
  if (resting) addEnergy(100.0f / (ENERGY_RECHARGE_MIN * 60.0f));
  else if (!eating) addEnergy(-100.0f / (ENERGY_AWAKE_HOURS * 3600.0f));
  if (eating && happiness < 80) addHappiness(1.0f / 60.0f);  // a full tummy is a happy tummy

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
      else if (energy < 25 || happiness < 30 || battState == BATT_LOW || battState == BATT_CRITICAL) f.mood = TIRED;
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
    if (overlay != OV_DIZZY) showOverlay(OV_DIZZY, 600);
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
  if (jerk > 0.02f || fabsf(m - 1.0f) > 0.12f) { calResetAverage(); return; }

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
RTC_DATA_ATTR float rtcOffGrav[3] = {0, 0, 0};
RTC_DATA_ATTR uint8_t rtcImuAddr = 0;
OffReason wokeFrom = OFF_NONE;

bool battPresent = false;
float battV = 0, battVSlow = 0;
float battSecHist[8];  uint8_t battSecIdx = 0, battSecCount = 0;    // last 8 s
float battMinHist[21]; uint8_t battMinIdx = 0, battMinCount = 0;    // every 30 s, 10 min
uint32_t battLastMinPush = 0;
bool battCharging = false, battFull = false, battUsb = false;
uint32_t battChargeStart = 0, battUnplugAt = 0, battNextNag = 0, battNextNom = 0;
uint8_t battEmptyCount = 0;
bool battFake = false;   // serial "fakebatt" for trying the reactions
float battPctEst = 100;
char battMsg1[22], battMsg2[22];

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
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_BATTERY);
  return mv / 16000.0f * BATT_DIVIDER * BATT_CAL;
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
  battText(starving ? "FOOD!! thank you!" : "yum! charging", starving ? "nom nom nom" : "", 2500);
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

  bool usb = battUsbSense();
  bool charging = battCharging;
#if PIN_USB_SENSE >= 0
  charging = usb;
#else
  if (usb) charging = true;
  else if (battUsb) charging = false;  // unplugged from the computer
  else if (!charging && (jump > 0.06f || (trend5 > 0.03f && since(battUnplugAt) > minutes(6)))) charging = true;
  else if (charging && (jump < (battFull ? -0.03f : -0.06f) ||  // a full cell only drops a little
                        (trend5 < -0.02f && since(battChargeStart) > minutes(6)))) charging = false;
#endif
  battUsb = usb;

  if (charging != battCharging) {
    battCharging = charging;
    battMinCount = 0;  // start the trends fresh
    battLastMinPush = now;
    if (charging) battChargeStart = now;
    else { battUnplugAt = now; battVSlow = battV; }
  }

  if (!charging) battFull = false;
  else if (!battFull && battV >= BATT_FULL_V && rise10 < 0.01f) battFull = true;  // stopped rising

  if (battFull) battPctEst = 100;
  else if (!charging) {
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
  battV = battVSlow = v;
  for (auto &h : battSecHist) h = v;
  battUsb = battUsbSense();
  battCharging = battUsb || v > 4.22f || wokeFrom == OFF_BATTERY;  // woke up from empty = a charger was plugged in
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
    else battText("goodnight!", "move me to wake", 0);
    say(SND_GOODNIGHT, 1, true);
    delay(2800);
    { BusLock lock; eyes.close(); }
    delay(900);
  }
  hush();
  saveMemory();

  rtcOffReason = why;
  rtcOffVolt = battPresent ? battV : 0;
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

// First thing at boot: if we were "off", take a quick look and go back to sleep
// unless someone moved the pet or plugged in a charger. Takes a few ms.
void offModeCheck() {
  if (rtcOffReason == OFF_NONE) return;
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) { rtcOffReason = OFF_NONE; return; }  // RESET / power cycle

  bool wake = false;
#if ENABLE_BATTERY
  float v = battReadVolts();
  if (rtcOffVolt > 2.5f && v > 2.5f) {
    if (v > rtcOffVolt + 0.25f) wake = true;                                // charger plugged in
    else if (rtcOffReason == OFF_USER && v < BATT_SHUTDOWN_V) rtcOffReason = OFF_BATTERY;  // drained while off
  }
#endif
  if (!wake && rtcOffReason == OFF_USER && rtcImuAddr) {
    Wire.begin(PIN_SDA, PIN_SCL);
    imuAddr = rtcImuAddr;
    imuWrite(0x6B, 0x00);  // wake the sensor for one reading
    delay(30);
    uint8_t b[6];
    bool ok = imuRead(0x3B, b, 6);
    imuWrite(0x6B, 0x40);
    if (ok) {
      float a[3], m = 0;
      for (int i = 0; i < 3; i++) { a[i] = (int16_t)(b[i * 2] << 8 | b[i * 2 + 1]); m += a[i] * a[i]; }
      m = sqrtf(m);
      float dot = 0;
      if (m > 0) for (int i = 0; i < 3; i++) dot += a[i] / m * rtcOffGrav[i];
      if (dot < cosf(OFF_WAKE_DEG * DEG_TO_RAD)) wake = true;  // turned / picked up
    }
  }
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
  Serial.printf("battery: %s %d%%  %.3f V (slow %.3f)  usb=%d%s\n", BATT_NAMES[battState], battPct, battV, battVSlow,
                battUsb, battFake ? "  [fake]" : "");
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
  Serial.println(F("commands: status imu calibrate calreset mute unmute chatty <0-3> sleep wake pet boop sing"));
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
  static uint32_t lastPrint = 0;
  if (imuStream && imuOk && since(lastPrint) > 100) {
    lastPrint = millis();
    Serial.printf("right=%+.2f up=%+.2f out=%+.2f |g|=%.2f gyro=%5.0f jerk=%.3f vib=%.4f floor=%.4f desk=%d hold=%d shake=%.1f dizzy=%.1f %s\n",
                  grav.x, grav.y, grav.z, accMag, gyroMag, jerk, vib, vibFloor, present(), handling, shakeMeter, dizzy,
                  ORIENT_NAMES[orient]);
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
  soundQueue = xQueueCreate(8, sizeof(int));
  // Core 0 exists on every ESP32 (the C3 is single-core; dual-core chips run loop() on core 1).
  xTaskCreatePinnedToCore(eyesTask, "eyes", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(soundTask, "sound", 4096, NULL, 1, NULL, 0);

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
    presenceStep();
    breakBuddyStep();
    activityStep();
    effectsStep();
    quirkStep();
    baselineFace();
    gazeStep();
  }
  if (now - lastNeeds >= 1000) { lastNeeds = now; needsTick(); battStep(); }
  if (now - lastAge >= 60000) { lastAge = now; ageTick(); }
  if (now - lastSave >= minutes(SAVE_EVERY_MIN)) { lastSave = now; saveMemory(); }

  vTaskDelay(1);
}
