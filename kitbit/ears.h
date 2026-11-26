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
  float loud = 0;           // level above the quiet room, dB (this 16 ms)
  float loudAvg = 0;        // ... averaged over the last second
  float continuity = 0;     // share of the last second that wasn't quiet (music ~1, talking has pauses)
  float tonalAvg = 0;       // tonality averaged over the last second
  float keyCorr = 0;        // how well the last ~6 s fit one key (0..1): music stays in a key
  float beatAvg = 0;        // beat strength averaged over ~4 s: is the beat clear enough to sing on?
  float steady = 0;         // how alike consecutive 128 ms note pictures are (held notes ~1, gliding speech lower)
  float sung = 0;           // share of the last second spent on held notes (singing / humming)
  bool singing = false;     // someone is singing / humming
  int8_t songPc[3] = {-1, -1, -1};  // the 3 notes (pitch classes) the song played most over ~6 s
  int8_t key = -1;          // 0 = C .. 11 = B
  bool keyMinor = false;
  int8_t chord = -1;        // root of the current chord (raw, every 128 ms)
  bool chordMinor = false;
  int8_t bass = -1;         // stable chord root: only chords of the key, clear winner, held ~0.25 s (-1 = unsure)
  bool bassMinor = false;
  uint32_t bassChanges = 0; // counts chord changes
  uint32_t chordMs = 0;     // typical time between chord changes
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
    lp1 = Biquad();
    lp2 = Biquad();
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
    st.beatAvg = 0;
    st.bass = -1;
    st.pitch = false;
  }

  // Butterworth low-pass, fs 16 kHz, fc 1.5 kHz, Q14 coefficients; two in a row = 4th order.
  struct Biquad { int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
  Biquad lp1, lp2;
  static int32_t biquad(Biquad &q, int32_t x) {
    const int32_t B0 = 991, B1 = 1983, B2 = 991, A1 = -19562, A2 = 7143;
    int64_t acc = (int64_t)B0 * x + (int64_t)B1 * q.x1 + (int64_t)B2 * q.x2 - (int64_t)A1 * q.y1 - (int64_t)A2 * q.y2;
    int32_t y = (int32_t)(acc >> 14);
    q.x2 = q.x1; q.x1 = x;
    q.y2 = q.y1; q.y1 = y;
    return y;
  }
  int32_t lowpass(int32_t x) { return biquad(lp2, biquad(lp1, x)); }

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
    // Anti-alias low-pass (2 x biquad, 1.5 kHz) before going down to 4 kHz: without it,
    // overtones above 2 kHz fold back as fake notes and wreck key / chord detection.
    int32_t x = lowpass(v >> 6);  // same scale as before (24-bit -> ~18-bit), 64-bit maths inside
    if (++decN < 4) return;
    decN = 0;
    decimated(constrain(x, -32767, 32767), now);  // 4 kHz, 16-bit
  }

 private:
  static const int MIDI_LO = 55, NOTES = 30;  // G3 .. C6 (well below the 1.5 kHz low-pass)
  static const int GN = 512;                  // Goertzel block: 128 ms at 4 kHz
  static const int OHOP = 64;                 // onset frame: 16 ms at 4 kHz
  static const int ON = 256;                  // onset history: 4.1 s
  struct G { int32_t coef, s1, s2; };  // 32-bit state: 32x32->64 multiply is 2 native instructions
  G gs[NOTES];
  int32_t dec = 0, decN = 0, gN = 0;
  bool dirty = false, blockDirty = false;
  float chromaLong[12] = {0}, chromaShort[12] = {0};
  float chromaKey[12] = {0};  // ~6 s: music keeps its key, talking averages out to "no key"
  float chromaBeat[12] = {0}; // ~0.5 s: for the stable chord
  int8_t bassCand = -1;
  bool bassCandMinor = false;
  int bassCandN = 0;
  uint32_t lastBassChange = 0;
  float keyAvg[24] = {0};
  // onsets
  float onset[ON];
  int onN = 0, onFill = 0, framesSinceTempo = 0;
  int64_t frameE = 0;
  int frameN = 0;
  float logEslow = 0, logE = 0, quietLog = 99, onsetMean = 0;
  float secLoud = 0, secTonal = 0, secSteady = 0, prevC[12] = {0};
  int secLoudN = 0, secSound = 0, secTonalN = 0, secSteadyN = 0;
  // held notes (singing / humming)
  float heldRef = 0, secHeld = 0;
  int heldLen = 0;
  uint8_t singHist = 0;
  void heldNote(bool have, float midi) {
    const int minFrames = SUNG_HOLD_MS / 16;
    if (have && heldLen && fabsf(midi - heldRef) < 0.5f) {
      heldLen++;
      if (heldLen == minFrames) secHeld += minFrames;  // the note just became "held"
      else if (heldLen > minFrames) secHeld += 1;
      return;
    }
    heldLen = have ? 1 : 0;
    heldRef = midi;
  }
  bool frameDirty = false;
  uint32_t frameTimes[ON];
  uint16_t musicHist = 0;
  int quietSecs = 0;
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
    float mx = 0, dot = 0, na = 0, nb = 0;
    for (int k = 0; k < 12; k++) {
      c[k] /= sum;
      dot += c[k] * prevC[k]; na += c[k] * c[k]; nb += prevC[k] * prevC[k];
      chromaLong[k] += (c[k] - chromaLong[k]) * 0.08f;  // ~1.6 s
      chromaKey[k] += (c[k] - chromaKey[k]) * 0.021f;   // ~6 s
      chromaShort[k] += (c[k] - chromaShort[k]) * 0.5f; // ~0.25 s
      chromaBeat[k] += (c[k] - chromaBeat[k]) * 0.25f;  // ~0.5 s, about a beat
      mx = max(mx, chromaShort[k]);
    }
    if (nb > 0) { secSteady += dot / sqrtf(na * nb); secSteadyN++; }
    memcpy(prevC, c, sizeof prevC);
    st.tonality = mx * 12.0f;  // max / mean
    secTonal += st.tonality;
    secTonalN++;
    findKey();
    findChord();
    findBassChord();
  }

  void findKey() {
    static const float MAJ[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const float MIN[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    // Every key's fit, smoothed over ~2.5 s; switch when another key is clearly better
    // on average (a single first guess made in a quiet room must never stick).
    float best = -2, bestAvg = -2;
    int bestI = 0;
    for (int r = 0; r < 12; r++)
      for (int m = 0; m < 2; m++) {
        const float *p = m ? MIN : MAJ;
        float mp = 0, mc = 0;
        for (int k = 0; k < 12; k++) { mp += p[k]; mc += chromaKey[k]; }
        mp /= 12; mc /= 12;
        float num = 0, dp = 0, dc = 0;
        for (int k = 0; k < 12; k++) {
          float a = p[(k - r + 12) % 12] - mp, b = chromaKey[k] - mc;
          num += a * b; dp += a * a; dc += b * b;
        }
        float corr = num / sqrtf(dp * dc + 1e-12f);
        int i = r * 2 + m;
        keyAvg[i] += (corr - keyAvg[i]) * 0.05f;
        if (corr > best) best = corr;
        if (keyAvg[i] > bestAvg) { bestAvg = keyAvg[i]; bestI = i; }
      }
    int curI = st.key < 0 ? -1 : st.key * 2 + st.keyMinor;
    if (curI < 0 || (bestI != curI && bestAvg > keyAvg[curI] + 0.02f)) { st.key = bestI / 2; st.keyMinor = bestI % 2; }
    st.keyCorr = max(0.0f, best);
    // the song's own strongest notes
    int8_t top[3] = {-1, -1, -1};
    for (int t = 0; t < 3; t++) {
      float bv = -1;
      for (int k = 0; k < 12; k++) {
        if (k == top[0] || k == top[1]) continue;
        if (chromaKey[k] > bv) { bv = chromaKey[k]; top[t] = k; }
      }
    }
    for (int t = 0; t < 3; t++) st.songPc[t] = top[t];
  }

  void findChord() {
    float best = -1;
    for (int r = 0; r < 12; r++)
      for (int m = 0; m < 2; m++) {
        float sc = chromaShort[r] + chromaShort[(r + (m ? 3 : 4)) % 12] + chromaShort[(r + 7) % 12];
        if (sc > best) { best = sc; st.chord = r; st.chordMinor = m; }
      }
  }

  // The 6 chords that belong to the key (major: I ii iii IV V vi; minor: i III iv v VI VII),
  // scored on ~0.5 s of audio. Changes only when a new chord clearly wins twice in a row.
  void findBassChord() {
    if (st.key < 0) return;
    static const int8_t MAJ_DEG[6] = {0, 2, 4, 5, 7, 9}, MAJ_MIN[6] = {0, 1, 1, 0, 0, 1};
    static const int8_t MIN_DEG[6] = {0, 3, 5, 7, 8, 10}, MIN_MIN[6] = {1, 0, 1, 1, 0, 0};
    const int8_t *deg = st.keyMinor ? MIN_DEG : MAJ_DEG, *mn = st.keyMinor ? MIN_MIN : MAJ_MIN;
    float best = -1, second = -1;
    int bestI = 0;
    for (int i = 0; i < 6; i++) {
      int r = (st.key + deg[i]) % 12;
      float in = chromaBeat[r] + 0.8f * chromaBeat[(r + (mn[i] ? 3 : 4)) % 12] + 0.8f * chromaBeat[(r + 7) % 12];
      if (in > best) { second = best; best = in; bestI = i; }
      else if (in > second) second = in;
    }
    if (best - second < BASS_MARGIN) { bassCandN = 0; return; }  // no clear winner: keep what we had
    int8_t r = (st.key + deg[bestI]) % 12;
    bool m = mn[bestI];
    if (r == st.bass && m == st.bassMinor) { bassCandN = 0; return; }
    if (r == bassCand && m == bassCandMinor) {
      if (++bassCandN >= 2) {
        uint32_t now = millis();
        if (lastBassChange) {  // how long chords last in this song (for sustained notes)
          uint32_t d = constrain(now - lastBassChange, 300UL, 4000UL);
          st.chordMs = st.chordMs ? (st.chordMs * 3 + d) / 4 : d;
        }
        lastBassChange = now;
        st.bass = r; st.bassMinor = m; st.bassChanges = st.bassChanges + 1; bassCandN = 0;
      }
    } else {
      bassCand = r; bassCandMinor = m; bassCandN = 1;
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
      secLoud += st.loud;
      secLoudN++;
      if (st.loud > MUSIC_SOUND_DB) secSound++;
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
    // the last second, summed up
    st.loudAvg = secLoudN ? secLoud / secLoudN : 0;
    st.continuity = secLoudN ? (float)secSound / secLoudN : 0;
    st.tonalAvg = secTonalN ? secTonal / secTonalN : 0;
    st.steady = secSteadyN ? secSteady / secSteadyN : 0;
    st.sung = min(1.0f, secHeld / 62.5f);
    secHeld = 0;
    singHist = ((singHist << 1) | (st.sung > SUNG_MIN)) & 0x7;  // last 3 seconds
    if (!st.singing && __builtin_popcount(singHist) >= 2) st.singing = true;
    else if (st.singing && (singHist & 0x3) == 0) st.singing = false;  // 2 s without held notes
    secLoud = secTonal = secSteady = 0;
    secLoudN = secSound = secTonalN = secSteadyN = 0;
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
    st.beatAvg += (st.beatStrength - st.beatAvg) * 0.25f;
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
    // Music: loud enough, (almost) never pausing, clear notes, and either staying in one
    // key or having a beat. Gentle music (no drums) has a weak beat, talking has pauses
    // and no steady key.
    bool musical = st.loudAvg > MUSIC_MIN_DB && st.continuity > MUSIC_MIN_CONT && st.tonalAvg > MUSIC_MIN_TONAL &&
                   (st.keyCorr > MUSIC_MIN_KEY || (st.beatStrength > MUSIC_MIN_BEAT && st.keyCorr > MUSIC_MIN_KEY - 0.15f));
    decide(musical);
  }

  void decide(bool musical) {
    musicHist = ((musicHist << 1) | musical) & 0xFF;  // last 8 seconds
    int n = __builtin_popcount(musicHist), recent = __builtin_popcount(musicHist & 0xF);
    // Quiet for 2 s: the music has stopped, no need to wait for the slow evidence.
    quietSecs = (st.loudAvg < MUSIC_MIN_DB * 0.6f || st.continuity < 0.5f) ? quietSecs + 1 : 0;
    if (!st.music && n >= 5) st.music = true;
    else if (st.music && (recent <= 1 || quietSecs >= 2)) { st.music = false; musicHist = 0; }
  }

  // Voice pitch (singing / humming): autocorrelation at 4 kHz, ~80-800 Hz.
  void humFrame(uint32_t now) {
    if (acFill < 256 || frameDirty || st.loud < 8) {
      if (st.pitchKind == 2) st.pitch = false;
      heldNote(st.pitch && st.pitchKind == 1, st.pitchMidi);
      return;
    }
    int32_t ac0 = 0;
    int16_t b[256];
    for (int i = 0; i < 256; i++) { b[i] = acBuf[(acPos + i) & 255]; ac0 += (int32_t)b[i] * b[i] >> 4; }
    if (ac0 <= 0) return;
    int32_t best = 0;
    int bestL = 0;
    int32_t acv[52];
    for (int L = 4; L <= 51; L++) {
      int32_t s = 0;
      for (int i = 0; i + L < 256; i++) s += (int32_t)b[i] * b[i + L] >> 4;
      acv[L] = s;
      if (L >= 5 && L <= 50 && s > best) { best = s; bestL = L; }
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
    heldNote(st.pitch, st.pitchMidi);
  }
};
