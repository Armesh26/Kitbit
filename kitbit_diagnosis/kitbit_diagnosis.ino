// =====================================================================
//  KitBit diagnosis
//  Checks every component one by one and reports PASS / FAIL on the
//  serial monitor (115200) and on the OLED.
//
//  Board: Seeed XIAO ESP32C3
//    OLED SH1106 1.3"  -> SDA D7, SCL D5 (0x3C)
//    MPU6050 / clone   -> SDA D7, SCL D5 (0x68 / 0x69)
//    Passive buzzer    -> D1
//    INMP441 mic       -> SCK D2, WS D3, SD D10, L/R GND, VDD 3V3
//
//  Serial commands:  a = run all tests   1..5 = run one test
//                    l = live sensor view (any key to stop)
// =====================================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ESP_I2S.h>

// ---------------- pins ----------------
#define PIN_SDA     D7      // moved from D4 (GPIO6 shorted to GND)
#define PIN_SCL     D5
#define PIN_BUZZER  D1
#define PIN_MIC_SCK D2
#define PIN_MIC_WS  D3
#define PIN_MIC_SD  D10

#define OLED_ADDR   0x3C
#define MIC_RATE    16000

Adafruit_SH1106G display(128, 64, &Wire, -1);
I2SClass i2s;

bool oledOk = false;
uint8_t imuAddr = 0;
bool micStarted = false;
int8_t micSlot = I2S_STD_SLOT_LEFT;

enum Result { R_PASS, R_FAIL, R_CHECK };  // CHECK = needs your eyes/ears
const char *RESULT_NAMES[] = {"PASS", "FAIL", "CHECK"};
Result results[5];
struct MicStats { float rms; int samples; int zeros; int allOnes; };
const char *TEST_NAMES[] = {"I2C bus", "OLED", "IMU", "Buzzer", "Mic"};

// ---------------- helpers ----------------
void screen(const char *title, const char *l1 = "", const char *l2 = "", const char *l3 = "") {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.drawFastHLine(0, 10, 128, SH110X_WHITE);
  display.setCursor(0, 16); display.print(l1);
  display.setCursor(0, 30); display.print(l2);
  display.setCursor(0, 44); display.print(l3);
  display.display();
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool imuRead(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(imuAddr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// accel in g, gyro in deg/s, temp in C
bool imuSample(float a[3], float g[3], float *t) {
  uint8_t b[14];
  if (!imuRead(0x3B, b, 14)) return false;
  for (int i = 0; i < 3; i++) {
    a[i] = (int16_t)(b[i * 2] << 8 | b[i * 2 + 1]) / 8192.0f;      // +-4 g
    g[i] = (int16_t)(b[8 + i * 2] << 8 | b[9 + i * 2]) / 65.5f;    // +-500 dps
  }
  if (t) *t = (int16_t)(b[6] << 8 | b[7]) / 340.0f + 36.53f;
  return true;
}

void beep(int freq, int ms) {
  tone(PIN_BUZZER, freq, ms);
  delay(ms + 30);
}

void report(int idx, Result r, const char *detail) {
  results[idx] = r;
  Serial.printf("  => %s: %s  (%s)\n\n", TEST_NAMES[idx], RESULT_NAMES[r], detail);
}

// ---------------- 1. I2C bus ----------------
void testI2C() {
  Serial.println("[1/5] I2C bus scan (SDA D7, SCL D5)");
  int found = 0;
  bool sawOled = false, sawImu = false;
  for (uint8_t a = 1; a < 127; a++) {
    if (!i2cPresent(a)) continue;
    found++;
    const char *what = "unknown";
    if (a == 0x3C || a == 0x3D) { what = "OLED"; sawOled = true; }
    if (a == 0x68 || a == 0x69) { what = "MPU6050 / clone"; sawImu = true; }
    Serial.printf("  found 0x%02X  %s\n", a, what);
  }
  if (!found) Serial.println("  nothing found - check SDA/SCL wiring, 3V3 and GND");
  char d[48];
  snprintf(d, sizeof d, "%d device(s), OLED %s, IMU %s", found, sawOled ? "yes" : "NO", sawImu ? "yes" : "NO");
  report(0, (sawOled && sawImu) ? R_PASS : R_FAIL, d);
}

// ---------------- 2. OLED ----------------
void testOLED() {
  Serial.println("[2/5] OLED (SH1106 at 0x3C)");
  if (!i2cPresent(OLED_ADDR)) {
    oledOk = false;
    report(1, R_FAIL, "no reply at 0x3C");
    return;
  }
  oledOk = display.begin(OLED_ADDR, true);
  if (!oledOk) { report(1, R_FAIL, "begin() failed"); return; }

  Serial.println("  full white...");
  display.clearDisplay();
  display.fillRect(0, 0, 128, 64, SH110X_WHITE);
  display.display();
  delay(800);

  Serial.println("  checkerboard (look for dead pixels / garbage stripes)...");
  display.clearDisplay();
  for (int y = 0; y < 64; y += 4)
    for (int x = 0; x < 128; x += 4)
      if (((x + y) / 4) % 2 == 0) display.fillRect(x, y, 4, 4, SH110X_WHITE);
  display.display();
  delay(1200);

  Serial.println("  border + text (border must touch all 4 edges)...");
  display.clearDisplay();
  display.drawRect(0, 0, 128, 64, SH110X_WHITE);
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(2);
  display.setCursor(16, 16);
  display.print("KitBit");
  display.setTextSize(1);
  display.setCursor(22, 40);
  display.print("display test");
  display.display();
  delay(1500);

  report(1, R_CHECK, "drawn - white, checkerboard, border must look clean; "
                   "a noisy strip on one side means wrong driver (SSD1306 vs SH1106)");
}

// ---------------- 3. IMU ----------------
void testIMU() {
  Serial.println("[3/5] IMU (MPU6050 or clone) - keep it STILL");
  screen("3/5 IMU", "keep me still...");
  imuAddr = 0;
  if (i2cPresent(0x68)) imuAddr = 0x68;
  else if (i2cPresent(0x69)) imuAddr = 0x69;
  if (!imuAddr) { report(2, R_FAIL, "no reply at 0x68 / 0x69"); return; }

  uint8_t who = 0;
  imuRead(0x75, &who, 1);
  const char *chip = "unknown clone";
  if (who == 0x68) chip = "MPU6050";
  else if (who == 0x70) chip = "MPU6500";
  else if (who == 0x71) chip = "MPU9250";
  else if (who == 0x73) chip = "MPU9255";
  else if (who == 0x98) chip = "clone (0x98)";
  Serial.printf("  address 0x%02X, WHO_AM_I 0x%02X = %s\n", imuAddr, who, chip);

  imuWrite(0x6B, 0x80); delay(100);  // reset
  imuWrite(0x6B, 0x01); delay(50);   // wake, PLL clock
  imuWrite(0x1A, 0x03);              // DLPF
  imuWrite(0x1B, 0x08);              // +-500 dps
  imuWrite(0x1C, 0x08);              // +-4 g
  delay(100);

  const int N = 100;
  float a[3], g[3], t = 0;
  float sumMag = 0, gMax = 0, sumA[3] = {0, 0, 0};
  float minMag = 99, maxMag = 0;
  int good = 0;
  for (int i = 0; i < N; i++) {
    if (imuSample(a, g, &t)) {
      float m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
      sumMag += m;
      minMag = min(minMag, m);
      maxMag = max(maxMag, m);
      for (int k = 0; k < 3; k++) { sumA[k] += a[k]; gMax = max(gMax, fabsf(g[k])); }
      good++;
    }
    delay(10);
  }
  if (good < N / 2) { report(2, R_FAIL, "reads failing on the bus"); return; }

  float mag = sumMag / good;
  bool stuck = (maxMag - minMag) < 0.0005f;  // perfectly identical data = frozen sensor
  Serial.printf("  accel avg  x=%+.2f y=%+.2f z=%+.2f g   |a|=%.3f g (ideal 1.00)\n",
                sumA[0] / good, sumA[1] / good, sumA[2] / good, mag);
  Serial.printf("  gyro max at rest = %.1f dps (raw, before bias removal)\n", gMax);
  Serial.printf("  temperature      = %.1f C\n", t);

  char d[64];
  if (stuck) { report(2, R_FAIL, "data never changes - sensor frozen / not awake"); return; }
  if (mag < 0.85f || mag > 1.15f) {
    snprintf(d, sizeof d, "gravity reads %.2f g, expected ~1.00", mag);
    report(2, R_FAIL, d);
    return;
  }
  if (gMax > 30) {
    snprintf(d, sizeof d, "gyro at rest %.0f dps - was it moving?", gMax);
    report(2, R_FAIL, d);
    return;
  }

  Serial.println("  now TILT / MOVE it for 3 s...");
  screen("3/5 IMU", "now tilt / move me", "for 3 seconds");
  float gyroPeak = 0;
  uint32_t start = millis();
  while (millis() - start < 3000) {
    if (imuSample(a, g, nullptr))
      for (int k = 0; k < 3; k++) gyroPeak = max(gyroPeak, fabsf(g[k]));
    delay(10);
  }
  snprintf(d, sizeof d, "%s, |a|=%.2f g, motion peak %.0f dps", chip, mag, gyroPeak);
  report(2, gyroPeak > 40 ? R_PASS : R_FAIL, gyroPeak > 40 ? d : "no rotation seen while moving - gyro dead?");
}

// ---------------- 4. Buzzer ----------------
void testBuzzer() {
  Serial.println("[4/5] Buzzer on D1 - listen: 3 rising beeps, then a sweep");
  screen("4/5 Buzzer", "listen:", "3 beeps + a sweep");
  beep(1000, 150);
  beep(1500, 150);
  beep(2000, 150);
  for (int f = 500; f <= 4000; f += 50) { tone(PIN_BUZZER, f); delay(6); }
  noTone(PIN_BUZZER);
  delay(100);
  report(3, R_CHECK, "played - you should have heard 3 beeps and a sweep. "
                   "Silent = wiring / active-vs-passive buzzer; one flat click = active buzzer");
}

// ---------------- 5. INMP441 mic ----------------
bool micBegin(int8_t slot) {
  if (micStarted) { i2s.end(); micStarted = false; }
  i2s.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_SD);
  micStarted = i2s.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, slot);
  micSlot = slot;
  return micStarted;
}

// Reads ~ms of audio: RMS (24-bit units, DC removed) and stuck-line counts.

MicStats micMeasure(int ms) {
  MicStats s = {0, 0, 0, 0};
  static int32_t buf[256];
  double sum = 0, sumSq = 0;
  uint32_t start = millis();
  while (millis() - start < (uint32_t)ms) {
    size_t got = i2s.readBytes((char *)buf, sizeof buf) / 4;
    for (size_t i = 0; i < got; i++) {
      if (buf[i] == 0) s.zeros++;
      if (buf[i] == -1) s.allOnes++;
      int32_t v = buf[i] >> 8;  // 24-bit sample, left-justified in 32 bits
      sum += v;
      sumSq += (double)v * v;
      s.samples++;
    }
  }
  if (s.samples) {
    double mean = sum / s.samples;
    s.rms = sqrt(max(0.0, sumSq / s.samples - mean * mean));  // remove DC offset
  }
  return s;
}

float micLevel(int ms) {  // short-window peak RMS, for clap detection
  float best = 0;
  uint32_t start = millis();
  while (millis() - start < (uint32_t)ms) best = max(best, micMeasure(30).rms);
  return best;
}

void testMic() {
  Serial.println("[5/5] INMP441 mic (SCK D2, WS D3, SD D10, L/R GND)");
  screen("5/5 Mic", "be quiet...");
  if (!micBegin(I2S_STD_SLOT_LEFT)) { report(4, R_FAIL, "I2S driver failed to start"); return; }
  delay(200);
  micMeasure(200);  // discard start-up samples

  MicStats q = micMeasure(800);
  Serial.printf("  left slot : %d samples, rms %.0f, zeros %d, stuck-high %d\n", q.samples, q.rms, q.zeros, q.allOnes);
  if (q.samples == 0) { report(4, R_FAIL, "no I2S data at all - check SCK / WS wiring"); return; }

  if (q.zeros > q.samples * 0.95f || q.allOnes > q.samples * 0.95f) {
    Serial.println("  left slot is silent, trying right slot (L/R pin may be high or floating)...");
    micBegin(I2S_STD_SLOT_RIGHT);
    delay(200);
    micMeasure(200);
    q = micMeasure(800);
    Serial.printf("  right slot: %d samples, rms %.0f, zeros %d, stuck-high %d\n", q.samples, q.rms, q.zeros, q.allOnes);
    if (q.zeros > q.samples * 0.95f) { report(4, R_FAIL, "data line always 0 - check SD wire, VDD 3V3, GND"); return; }
    if (q.allOnes > q.samples * 0.95f) { report(4, R_FAIL, "data line stuck high - check SD wire / short"); return; }
    Serial.println("  mic answers on the RIGHT slot: tie L/R to GND (or keep right slot in code)");
  }

  float quiet = micLevel(1000);
  Serial.printf("  quiet level %.0f\n", quiet);
  Serial.println("  now CLAP or talk loudly near it (4 s)...");
  screen("5/5 Mic", "CLAP or talk", "near me (4 s)");
  float loud = micLevel(4000);
  Serial.printf("  loud level  %.0f  (x%.1f louder)\n", loud, quiet > 0 ? loud / quiet : 0);

  char d[64];
  snprintf(d, sizeof d, "quiet %.0f, loud %.0f, slot %s", quiet, loud, micSlot == I2S_STD_SLOT_LEFT ? "left" : "right");
  report(4, loud > quiet * 3 && loud > 2000 ? R_PASS : R_FAIL,
         loud > quiet * 3 && loud > 2000 ? d : "no clear reaction to sound - check the sound hole / wiring");
}

// ---------------- summary + live view ----------------
void summary() {
  Serial.println("================ SUMMARY ================");
  for (int i = 0; i < 5; i++) Serial.printf("  %-8s %s\n", TEST_NAMES[i], RESULT_NAMES[results[i]]);
  Serial.println("  CHECK = confirm with your eyes / ears");
  Serial.println("  commands: a = all, 1..5 = one test, l = live view");
  Serial.println("=========================================");
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.print("KitBit diagnosis");
  display.drawFastHLine(0, 10, 128, SH110X_WHITE);
  for (int i = 0; i < 5; i++) {
    display.setCursor(0, 13 + i * 10);
    display.printf("%-8s %s", TEST_NAMES[i], RESULT_NAMES[results[i]]);
  }
  display.display();
}

void liveView() {
  Serial.println("Live view - press any key to stop");
  if (!micStarted) micBegin(micSlot);
  while (!Serial.available()) {
    float a[3] = {0, 0, 0}, g[3] = {0, 0, 0}, t = 0;
    bool imu = imuAddr && imuSample(a, g, &t);
    float lvl = micStarted ? micMeasure(40).rms : 0;
    Serial.printf("acc %+.2f %+.2f %+.2f  gyro %+6.1f %+6.1f %+6.1f  mic %6.0f\n",
                  a[0], a[1], a[2], g[0], g[1], g[2], lvl);
    if (oledOk) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.print("LIVE (key = stop)");
      if (imu) {
        display.setCursor(0, 14); display.printf("ax%+.2f ay%+.2f", a[0], a[1]);
        display.setCursor(0, 24); display.printf("az%+.2f  %.1fC", a[2], t);
        display.setCursor(0, 34); display.printf("g %+4.0f %+4.0f %+4.0f", g[0], g[1], g[2]);
      }
      display.setCursor(0, 50); display.print("mic");
      int bar = constrain((int)(log10f(lvl + 1) * 20), 0, 104);
      display.fillRect(24, 50, bar, 8, SH110X_WHITE);
      display.display();
    }
    delay(60);
  }
  while (Serial.available()) Serial.read();
  summary();
}

void runAll() {
  testI2C();
  testOLED();
  testIMU();
  testBuzzer();
  testMic();
  summary();
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // time to open the serial monitor
  Serial.println("\n===== KitBit diagnosis =====\n");
  pinMode(PIN_BUZZER, OUTPUT);
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  for (int i = 0; i < 5; i++) results[i] = R_FAIL;
  runAll();
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  char c = Serial.read();
  switch (c) {
    case 'a': runAll(); break;
    case '1': testI2C(); summary(); break;
    case '2': testOLED(); summary(); break;
    case '3': testIMU(); summary(); break;
    case '4': testBuzzer(); summary(); break;
    case '5': testMic(); summary(); break;
    case 'l': liveView(); break;
  }
}
