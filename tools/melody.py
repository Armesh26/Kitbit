#!/usr/bin/env python3
"""
Melody-extraction prototype for KitBit (runs on the PC, numpy only).

    python3 tools/melody.py tools/audio/song1.wav [--mode skyline|loudest] [--play]

For every 32 ms it finds the pitches present (a note is grouped with its
overtones: "harmonic summation"), then picks either the highest strong pitch in
the melody range ("skyline") or the strongest one ("loudest"). Notes are
smoothed and written as a buzzer-style square-wave WAV next to the input, so you
can hear what the pet would play. --play also plays them on the pet's buzzer.
"""
import argparse
import json
import os
import time
import wave

import numpy as np

SR = 16000
FRAME, HOP = 2048, 512             # 128 ms window, 32 ms steps
MIDI_LO, MIDI_HI = 50, 91          # ~147 Hz .. ~1568 Hz: where melodies live
HARMONICS = 8


def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def load(path):
    with wave.open(path) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        sr = w.getframerate()
    if sr != SR:
        raise SystemExit(f"expected {SR} Hz audio, got {sr}")
    return x / 32768.0


def salience(mag):
    """Pitch strength for each MIDI note: its fundamental + overtones, decaying weights."""
    freqs = np.fft.rfftfreq(FRAME, 1 / SR)
    out = np.zeros(MIDI_HI - MIDI_LO + 1)
    for k, m in enumerate(range(MIDI_LO, MIDI_HI + 1)):
        f0 = midi_hz(m)
        total = 0.0
        for h in range(1, HARMONICS + 1):
            f = f0 * h
            if f > SR / 2 - 100:
                break
            lo, hi = f * 2 ** (-0.5 / 12), f * 2 ** (0.5 / 12)
            idx = np.where((freqs >= lo) & (freqs <= hi))[0]
            if len(idx):
                total += 0.8 ** (h - 1) * mag[idx].max()
        out[k] = total
    return out


def track(x, mode):
    win = np.hanning(FRAME)
    notes = []  # per frame: midi or 0
    energies = []
    for start in range(0, len(x) - FRAME, HOP):
        seg = x[start:start + FRAME] * win
        energies.append(np.sqrt((seg ** 2).mean()))
        mag = np.abs(np.fft.rfft(seg)) ** 0.5  # compress: quieter notes still count
        sal = salience(mag)
        best = sal.max()
        if best <= 0:
            notes.append(0)
            continue
        if mode == "loudest":
            pick = int(sal.argmax())
        else:  # skyline: highest pitch that is a clear peak and strong enough
            peaks = [k for k in range(1, len(sal) - 1)
                     if sal[k] >= sal[k - 1] and sal[k] >= sal[k + 1] and sal[k] > 0.7 * best]
            pick = max(peaks) if peaks else int(sal.argmax())
        notes.append(MIDI_LO + pick)
    notes = np.array(notes)
    energies = np.array(energies)
    notes[energies < 0.25 * np.median(energies)] = 0  # silence / breaks
    # median filter (5 frames) to remove blips
    smooth = notes.copy()
    for i in range(2, len(notes) - 2):
        smooth[i] = int(np.median(notes[i - 2:i + 3]))
    return smooth


# ---------------------------------------------------------------------------
# "canon" mode: knows the key, tracks one smooth melody line, snaps to the beat
# ---------------------------------------------------------------------------
KEY_PCS = {"D": [2, 4, 6, 7, 9, 11, 1]}  # D major: D E F# G A B C#
CANON_LO, CANON_HI = 64, 90  # E4 .. F#6


def spectra(x, hop):
    win = np.hanning(FRAME)
    return np.array([np.abs(np.fft.rfft(x[s:s + FRAME] * win)) for s in range(0, len(x) - FRAME, hop)])


def estimate_beat(x, bpm_lo=60, bpm_hi=100):
    """Tempo (quarter-note BPM) and phase (s) from spectral-flux onsets."""
    hop = 256
    M = spectra(x, hop)
    L = np.log1p(10 * M)
    flux = np.maximum(0, np.diff(L, axis=0)).sum(1)
    flux = flux - flux.mean()
    fps = SR / hop
    ac = np.correlate(flux, flux, "full")[len(flux) - 1:]
    best = max(range(bpm_lo, bpm_hi + 1),
               key=lambda b: ac[int(round(fps * 60 / b))] + ac[int(round(fps * 30 / b))])  # beat + half-beat
    # finer: 0.5 BPM steps around it
    cands = np.arange(best - 1.5, best + 1.6, 0.25)
    bpm = max(cands, key=lambda b: sum(ac[int(round(fps * 60 * k / b))] for k in (1, 2, 4) if int(round(fps * 60 * k / b)) < len(ac)))
    period = fps * 60 / bpm / 2  # eighth notes, in flux frames
    phases = np.arange(0, period, 0.5)
    def score(ph):
        idx = (ph + np.arange(0, len(flux) - ph, period)).astype(int)
        return flux[idx].sum()
    phase = max(phases, key=score) / fps
    return float(bpm), float(phase)


def canon_track(x, key="D"):
    """Viterbi melody line over key notes. Returns (frame times s, midi per frame, 0 = rest)."""
    hop = HOP // 2  # 16 ms
    M = spectra(x, hop) ** 0.5
    allowed = [m for m in range(CANON_LO, CANON_HI + 1) if m % 12 in KEY_PCS[key]]
    sal = np.array([salience(row) for row in M])  # frames x MIDI_LO..MIDI_HI
    S = np.stack([sal[:, m - MIDI_LO] if MIDI_LO <= m <= MIDI_HI else np.zeros(len(sal)) for m in allowed], 1)
    S = S * (1 + 0.03 * (np.array(allowed) - CANON_LO))  # preference for the top line (chords sit below it)
    energy = np.sqrt((M ** 4).mean(1))
    voiced = energy > 0.3 * np.median(energy)
    # emission: log of normalised salience
    E = np.log(S / (S.max(1, keepdims=True) + 1e-9) + 1e-3)
    n, k = E.shape
    jumps = np.abs(np.subtract.outer(allowed, allowed))
    T = -0.15 * jumps - 3.0 * (jumps >= 12)  # mostly smooth, but runs move fast; no octave flips
    T[jumps == 0] += 0.1                      # a little stickiness against flicker
    score = E[0].copy()
    back = np.zeros((n, k), dtype=int)
    for t in range(1, n):
        cand = score[:, None] + T
        back[t] = cand.argmax(0)
        score = cand.max(0) + E[t]
    path = [int(score.argmax())]
    for t in range(n - 1, 0, -1):
        path.append(back[t][path[-1]])
    path = path[::-1]
    midi = np.array([allowed[i] for i in path])
    midi[~voiced] = 0
    times = (np.arange(n) * hop + FRAME / 2) / SR
    return times, midi


def canon_events(x, key="D"):
    bpm, phase = estimate_beat(x)
    times, midi = canon_track(x, key)
    eighth = 60.0 / bpm / 4  # sixteenth-note grid (the canon has fast runs)
    events = []
    t = phase
    if phase > 0.02:
        events.append([0, int(phase * 1000)])
    while t < times[-1]:
        sel = midi[(times >= t) & (times < t + eighth)]
        if len(sel):
            vals, counts = np.unique(sel, return_counts=True)
            m = int(vals[counts.argmax()])
        else:
            m = 0
        if events and events[-1][0] == m:
            events[-1][1] += eighth * 1000
        else:
            events.append([m, eighth * 1000])
        t += eighth
    return bpm, [(m, int(round(d))) for m, d in events]


def to_events(frames, min_ms=90):
    """Frame track -> list of (midi, ms); midi 0 = rest."""
    events = []
    step = HOP * 1000 / SR
    for m in frames:
        if events and events[-1][0] == m:
            events[-1][1] += step
        else:
            events.append([int(m), step])
    # fold very short notes into their neighbour
    out = []
    for m, d in events:
        if out and d < min_ms:
            out[-1][1] += d
        else:
            out.append([m, d])
    merged = []
    for m, d in out:
        if merged and merged[-1][0] == m:
            merged[-1][1] += d
        else:
            merged.append([m, d])
    return [(m, int(d)) for m, d in merged]


def render(events, path):
    """Square wave, like the pet's buzzer."""
    out = []
    for m, d in events:
        n = int(SR * d / 1000)
        if m == 0:
            out.append(np.zeros(n))
        else:
            t = np.arange(n) / SR
            out.append(0.3 * np.sign(np.sin(2 * np.pi * midi_hz(m) * t)))
    y = np.concatenate(out) if out else np.zeros(1)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((y * 32767).astype("<i2").tobytes())


def play_on_pet(events, port):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.5)
    for m, d in events:
        if m:
            s.write(f"note {midi_hz(m):.0f} {max(20, d - 15)}\n".encode())
        time.sleep(d / 1000)
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--mode", choices=["skyline", "loudest", "canon"], default="canon")
    ap.add_argument("--play", action="store_true", help="play the result on the pet's buzzer")
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()

    x = load(args.wav)
    if args.mode == "canon":
        bpm, events = canon_events(x)
        print(f"tempo {bpm:.1f} BPM (quarter notes), key D major, notes snapped to sixteenths")
    else:
        events = to_events(track(x, args.mode))
    base = os.path.splitext(args.wav)[0]
    out_wav = f"{base}_{args.mode}_buzzer.wav"
    render(events, out_wav)
    with open(f"{base}_{args.mode}_notes.json", "w") as f:
        json.dump(events, f)
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    shown = " ".join(f"{names[m % 12]}{m // 12 - 1}:{d}" if m else f"-:{d}" for m, d in events[:40])
    print(f"{len(events)} notes/rests ({args.mode}). first ones: {shown}")
    print(f"buzzer version: {out_wav}")
    if args.play:
        play_on_pet(events, args.port)


if __name__ == "__main__":
    main()
