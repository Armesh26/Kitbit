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
    ap.add_argument("--mode", choices=["skyline", "loudest"], default="skyline")
    ap.add_argument("--play", action="store_true", help="play the result on the pet's buzzer")
    ap.add_argument("--port", default="/dev/ttyACM0")
    args = ap.parse_args()

    x = load(args.wav)
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
