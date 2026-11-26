// =====================================================================
//  KitBit ears: music (beat, tempo, key, chord) + pitch (whistle / hum)
//
//  Fed with every 16 kHz mic sample by the mic task. The ESP32-C3 has no
//  floating-point unit, so all per-sample work is integer; floats are only
//  used a few dozen times per second.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

struct EarsState {          // read by the pet (written by the mic task)
  bool music = false;       // music is playing
  float bpm = 0;            // tempo (quarter notes)
  uint32_t beatMs = 0;      // beat period
  uint32_t lastBeat = 0;    // millis() of a recent beat
  float beatStrength = 0;   // 0..1, how regular the beat is
  float tonality = 0;       // max/mean of the chroma: clear notes ~2-4, noise ~1.2
  float loud = 0;           // level above the quiet room, dB
  int8_t key = -1;          // 0 = C .. 11 = B
  bool keyMinor = false;
  int8_t chord = -1;        // root of the current chord
  bool chordMinor = false;
  bool pitch = false;       // a steady single pitch (whistle / hum) right now
  float pitchMidi = 0;
  uint8_t pitchKind = 0;    // 1 = whistle, 2 = hum
  uint32_t pitchMs = 0;     // when pitch was last updated
};

class Ears {
 public:
  volatile EarsState st;

  void reset() {
    dec = decN = 0;
    st.pitchKind = 0;
    gN = 0;
    for (auto &g : gs) g.s1 = g.s2 = 0;
    onN = 0;
    onFill = 0;
    framesSinceTempo = 0;
    hpPrev = hpOut = 0;
    zcArmed = true;
    zcN = 0;
    zcFirst = zcLast = 0;
    sampleIdx = 0;
    acFill = 0;
    musicHist = 0;
    st.music = false;
    st.pitch = false;
  }

  void begin() {
    for (int i = 0; i < NOTES; i++) {
      float f = 440.0f * powf(2.0f, (MIDI_LO + i - 69) / 12.0f);
      gs[i].coef = (int32_t)(2.0f * cosf(2.0f * PI * f / 4000.0f) * 16384.0f);
    }
    reset();
  }

  // One 16 kHz sample (24-bit value). selfSound = our own buzzer is playing.
  void feed(int32_t v, bool selfSound, uint32_t now) {
    sampleIdx++;
    dirty |= selfSound;
    whistleSample(v, now);
    dec += v;
    if (++decN < 4) return;
    int32_t x = constrain(dec >> 8, -32767, 32767);  // 4 kHz, 16-bit
    dec = decN = 0;
    decimated(x, now);
  }

 private:
  static const int MIDI_LO = 55, NOTES = 36;  // G3 .. F#6
  static const int GN = 512;                  // Goertzel block: 128 ms at 4 kHz
  static const int OHOP = 64;                 // onset frame: 16 ms at 4 kHz
  static const int ON = 256;                  // onset history: 4.1 s
  struct G { int32_t coef, s1, s2; };  // 32-bit state: 32x32->64 multiply is 2 native instructions
  G gs[NOTES];
  int32_t dec = 0, decN = 0, gN = 0;
  bool dirty = false, blockDirty = false;
  float chromaLong[12] = {0}, chromaShort[12] = {0};
  // onsets
  float onset[ON];
  int onN = 0, onFill = 0, framesSinceTempo = 0;
  int64_t frameE = 0;
  int frameN = 0;
  float logEslow = 0, logE = 0, quietLog = 99, onsetMean = 0;
  bool frameDirty = false;
  uint32_t frameTimes[ON];
  uint16_t musicHist = 0;
  // hum autocorrelation (4 kHz)
  int16_t acBuf[256];
  int acPos = 0, acFill = 0, acHop = 0;
  // whistle zero crossings (16 kHz)
  int32_t hpPrev = 0, hpOut = 0, zcPeak = 0;
  bool zcArmed = true;
  int zcN = 0;
  uint32_t zcFirst = 0, zcLast = 0, sampleIdx = 0, zcFrame = 0;
  uint32_t zcPeriods[64];

  // ---------------- whistle: nearly a pure tone, time its zero crossings --------
  void whistleSample(int32_t v, uint32_t now) {
    hpOut = (v - hpPrev) + ((hpOut * 15) >> 4);  // high-pass ~160 Hz
    hpPrev = v;
    int32_t a = abs(hpOut);
    zcPeak = max(a, zcPeak - (zcPeak >> 10));
    int32_t hyst = zcPeak >> 2;
    if (zcArmed && hpOut < -hyst) zcArmed = false;
    else if (!zcArmed && hpOut > hyst) {  // rising through zero (with hysteresis)
      zcArmed = true;
      if (zcN && zcN < 64) zcPeriods[zcN - 1] = sampleIdx - zcLast;
      if (!zcN) zcFirst = sampleIdx;
      zcLast = sampleIdx;
      zcN++;
    }
    if (++zcFrame >= 256) {  // every 16 ms
      zcFrame = 0;
      whistleFrame(now);
    }
  }

  void whistleFrame(uint32_t now) {
    bool ok = false;
    float midi = 0;
    if (zcN >= 9 && !frameDirty) {
      float period = (float)(zcLast - zcFirst) / (zcN - 1);  // samples
      float f = 16000.0f / period;
      // regular? (spread of the individual periods)
      int n = min(zcN - 1, 63);
      float dev = 0;
      for (int i = 0; i < n; i++) dev += fabsf(zcPeriods[i] - period);
      dev /= n;
      if (f > 500 && f < 4000 && dev / period < 0.06f && st.loud > 12) {
        ok = true;
        midi = 69 + 12 * log2f(f / 440.0f);
      }
    }
    zcN = 0;
    if (ok) setPitch(midi, 1, now);
    else if (st.pitchKind == 1) st.pitch = false;
  }

  void setPitch(float midi, uint8_t kind, uint32_t now) {
    st.pitch = true;
    st.pitchMidi = midi;
    st.pitchKind = kind;
    st.pitchMs = now;
  }

  // ---------------- 4 kHz stream: chroma, onsets, hum ------------------------
  void decimated(int32_t x, uint32_t now) {
    // Goertzel bank
    for (int i = 0; i < NOTES; i++) {
      G &g = gs[i];
      int32_t s = x + (int32_t)(((int64_t)g.coef * g.s1) >> 14) - g.s2;  // |s| < 2^24 for 16-bit x, 512 samples
      g.s2 = g.s1;
      g.s1 = s;
    }
    if (dirty) blockDirty = frameDirty = true;
    dirty = false;
    if (++gN >= GN) chromaBlock();
    // onset frame
    frameE += (int64_t)x * x;
    if (++frameN >= OHOP) onsetFrame(now);
    // hum autocorrelation buffer (12-bit to keep the sums in 32 bits)
    acBuf[acPos] = (int16_t)constrain(x >> 4, -2047, 2047);
    acPos = (acPos + 1) & 255;
    if (acFill < 256) acFill++;
    if (++acHop >= OHOP) { acHop = 0; humFrame(now); }
  }

  void chromaBlock() {
    gN = 0;
    float e[NOTES], total = 0;
    for (int i = 0; i < NOTES; i++) {
      float s1 = (float)gs[i].s1, s2 = (float)gs[i].s2, c = gs[i].coef / 16384.0f;
      e[i] = s1 * s1 + s2 * s2 - c * s1 * s2;
      if (e[i] < 0) e[i] = 0;
      gs[i].s1 = gs[i].s2 = 0;
      total += e[i];
    }
    if (blockDirty || total <= 0) { blockDirty = false; return; }  // our own buzzer was in it
    float c[12] = {0}, sum = 0;
    for (int i = 0; i < NOTES; i++) c[(MIDI_LO + i) % 12] += sqrtf(e[i]);
    for (float v : c) sum += v;
    if (sum <= 0) return;
    float mx = 0;
    for (int k = 0; k < 12; k++) {
      c[k] /= sum;
      chromaLong[k] += (c[k] - chromaLong[k]) * 0.08f;  // ~1.6 s
      chromaShort[k] += (c[k] - chromaShort[k]) * 0.5f; // ~0.25 s
      mx = max(mx, chromaShort[k]);
    }
    st.tonality = mx * 12.0f;  // max / mean
    findKey();
    findChord();
  }

  void findKey() {
    static const float MAJ[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const float MIN[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    float best = -2;
    for (int r = 0; r < 12; r++)
      for (int m = 0; m < 2; m++) {
        const float *p = m ? MIN : MAJ;
        float mp = 0, mc = 0;
        for (int k = 0; k < 12; k++) { mp += p[k]; mc += chromaLong[k]; }
        mp /= 12; mc /= 12;
        float num = 0, dp = 0, dc = 0;
        for (int k = 0; k < 12; k++) {
          float a = p[(k - r + 12) % 12] - mp, b = chromaLong[k] - mc;
          num += a * b; dp += a * a; dc += b * b;
        }
        float corr = num / sqrtf(dp * dc + 1e-12f);
        if (corr > best) { best = corr; st.key = r; st.keyMinor = m; }
      }
  }

  void findChord() {
    float best = -1;
    for (int r = 0; r < 12; r++)
      for (int m = 0; m < 2; m++) {
        float sc = chromaShort[r] + chromaShort[(r + (m ? 3 : 4)) % 12] + chromaShort[(r + 7) % 12];
        if (sc > best) { best = sc; st.chord = r; st.chordMinor = m; }
      }
  }

  void onsetFrame(uint32_t now) {
    float e = (float)frameE / frameN + 1.0f;
    frameE = 0;
    frameN = 0;
    float l = 10.0f * log10f(e);  // dB
    float flux;
    if (frameDirty) {
      // Our own buzzer was playing: no information. Count it as an average moment
      // (not as silence - it sings ON the beat, zeros there would erase the beat it
      // follows), and don't let our own sound count as loudness.
      flux = onsetMean;
    } else {
      // quiet-room level: follows quickly down, very slowly up
      if (l < quietLog) quietLog = l;
      else quietLog += 0.0005f * (l - quietLog);
      st.loud = l - quietLog;
      flux = max(0.0f, l - logEslow);
      logEslow += (l - logEslow) * 0.3f;
      onsetMean += (flux - onsetMean) * 0.02f;
    }
    frameDirty = false;
    onset[onN] = flux;
    frameTimes[onN] = now;
    onN = (onN + 1) % ON;
    if (onFill < ON) onFill++;
    if (++framesSinceTempo >= 62) { framesSinceTempo = 0; tempo(now); }
  }

  // Once a second: tempo + beat phase from the last 4 s of onsets.
  void tempo(uint32_t now) {
    if (onFill < ON) return;
    float o[ON], mean = 0;
    for (int i = 0; i < ON; i++) { o[i] = onset[(onN + i) % ON]; mean += o[i]; }
    mean /= ON;
    float var = 0;
    for (int i = 0; i < ON; i++) { o[i] -= mean; var += o[i] * o[i]; }
    if (var <= 1e-6f) { decide(false); return; }
    // lags: 62.5 frames/s -> 60..180 BPM = lags 62..21
    float ac[66];
    for (int L = 18; L < 66; L++) {
      float s = 0;
      for (int i = 0; i + L < ON; i++) s += o[i] * o[i + L];
      ac[L] = s / var;
    }
    int bestL = 0;
    float bestS = -1;
    for (int L = 21; L <= 62; L++) {
      float bpm = 3750.0f / L;
      float pref = expf(-0.5f * powf(log2f(bpm / 110.0f) / 0.6f, 2));  // tempos near 110 a bit preferred
      float s = ac[L] * (0.7f + 0.3f * pref);
      if (L * 2 < 66) s += 0.3f * ac[L * 2];
      if (s > bestS) { bestS = s; bestL = L; }
    }
    // parabolic refinement
    float Lf = bestL;
    if (bestL > 18 && bestL < 65) {
      float a = ac[bestL - 1], b = ac[bestL], c = ac[bestL + 1], d = a - 2 * b + c;
      if (d < 0) Lf += 0.5f * (a - c) / d;
    }
    st.beatStrength = max(0.0f, ac[bestL]);
    st.bpm = 3750.0f / Lf;
    st.beatMs = (uint32_t)(Lf * 16.0f);
    // phase: which offset collects the most onset energy on a comb
    int P = bestL;
    float bestP = -1;
    int bestO = 0;
    for (int off = 0; off < P; off++) {
      float s = 0;
      for (int i = ON - 1 - off; i >= 0; i -= P) s += o[i];
      if (s > bestP) { bestP = s; bestO = off; }
    }
    st.lastBeat = frameTimes[(onN + ON - 1 - bestO) % ON];
    bool musical = st.loud > MUSIC_MIN_DB && st.beatStrength > MUSIC_MIN_BEAT && st.tonality > MUSIC_MIN_TONAL;
    decide(musical);
  }

  void decide(bool musical) {
    musicHist = ((musicHist << 1) | musical) & 0xF;  // last 4 seconds
    int n = __builtin_popcount(musicHist);
    if (!st.music && n >= 3) st.music = true;
    else if (st.music && n <= 1) st.music = false;
  }

  // Hum: autocorrelation at 4 kHz, 100-500 Hz.
  void humFrame(uint32_t now) {
    if (acFill < 256 || frameDirty || st.loud < 15) {
      if (st.pitchKind == 2) st.pitch = false;
      return;
    }
    int32_t ac0 = 0;
    int16_t b[256];
    for (int i = 0; i < 256; i++) { b[i] = acBuf[(acPos + i) & 255]; ac0 += (int32_t)b[i] * b[i] >> 4; }
    if (ac0 <= 0) return;
    int32_t best = 0;
    int bestL = 0;
    int32_t acv[42];
    for (int L = 7; L <= 41; L++) {
      int32_t s = 0;
      for (int i = 0; i + L < 256; i++) s += (int32_t)b[i] * b[i + L] >> 4;
      acv[L] = s;
      if (L >= 8 && L <= 40 && s > best) { best = s; bestL = L; }
    }
    float clarity = (float)best / ac0 * 256.0f / (256 - bestL);
    bool whistling = st.pitch && st.pitchKind == 1;
    if (bestL && clarity > 0.85f && !whistling) {
      float Lf = bestL, a = acv[bestL - 1], bb = acv[bestL], c = acv[bestL + 1], d = a - 2 * bb + c;
      if (d < 0) Lf += 0.5f * (a - c) / d;
      float f = 4000.0f / Lf;
      setPitch(69 + 12 * log2f(f / 440.0f), 2, now);
    } else if (st.pitchKind == 2) {
      st.pitch = false;
    }
  }
};
