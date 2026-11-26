#!/usr/bin/env python3
"""
Record what KitBit's microphone hears into a WAV file.

    python3 tools/audio_dump.py --seconds 15 --name song1

The pet streams raw 16 kHz audio over USB ("audiodump" command); this script
saves it as tools/audio/<name>.wav. Close the Arduino serial monitor first.
"""
import argparse
import os
import struct
import time
import wave

import numpy as np
import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--name", default=time.strftime("clip_%H%M%S"))
    args = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, 115200, 0.05
    s.dtr = s.rts = False  # don't reset the board
    s.open()

    # Opening the port may reboot the pet: wait until it's talking (or quiet for 2 s).
    print("waiting for the pet...")
    text, t0 = b"", time.time()
    while time.time() - t0 < 12:
        text += s.read(4096)
        if b"Microphone" in text or (time.time() - t0 > 2 and not text):
            break
    time.sleep(1.0)
    s.read(100000)

    def cmd(c):
        s.write((c + "\n").encode())
        s.flush()
        time.sleep(0.2)

    cmd("mute")
    cmd(f"prompt recording {args.seconds}s|play some music!")
    time.sleep(1.0)
    s.read(100000)
    print(f"recording {args.seconds} s - play the music now")
    cmd(f"audiodump {args.seconds}")

    raw = bytearray()
    end = time.time() + args.seconds + 2
    while time.time() < end:
        raw += s.read(65536)
    cmd("prompt")
    cmd("unmute")
    s.close()

    # Parse frames: "AUD!" + uint16 count + count * 3 bytes (24-bit little endian)
    samples, i, frames = [], 0, 0
    while True:
        i = raw.find(b"AUD!", i)
        if i < 0 or i + 6 > len(raw):
            break
        n = struct.unpack_from("<H", raw, i + 4)[0]
        j = i + 6 + n * 3
        if n > 256 or j > len(raw):
            i += 4
            continue
        b = np.frombuffer(bytes(raw[i + 6:j]), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - (1 << 24), v)
        samples.append(v)
        frames += 1
        i = j
    if not samples:
        raise SystemExit("no audio received - is the new firmware on the pet?")
    x = np.concatenate(samples).astype(np.float64)
    x -= x.mean()
    peak = np.abs(x).max() or 1
    got = len(x) / 16000
    print(f"got {got:.1f} s of audio ({frames} frames), expected {args.seconds} s"
          + ("" if got > args.seconds * 0.95 else "  <- some audio was lost on USB"))
    print(f"level: peak {peak:.0f}, rms {np.sqrt((x ** 2).mean()):.0f} (24-bit units)")

    os.makedirs(os.path.join(os.path.dirname(__file__), "audio"), exist_ok=True)
    path = os.path.join(os.path.dirname(__file__), "audio", args.name + ".wav")
    pcm = (x / peak * 0.9 * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm.tobytes())
    print(f"saved {path}")


if __name__ == "__main__":
    main()
