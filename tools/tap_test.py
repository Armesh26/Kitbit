#!/usr/bin/env python3
"""
KitBit tap test.

Shows "TAP ONCE / TWICE / THRICE / DON'T TOUCH" on the pet's screen, records the
raw IMU and microphone data the pet streams in "tapdebug" mode, then compares
what you did with what the pet detected and suggests thresholds.

    python3 tools/tap_test.py [--port /dev/ttyACM0] [--rounds 3]

Close the Arduino serial monitor first (only one program can use the port).
Raw data is saved to tools/tap_logs/ for a closer look.
"""
import argparse
import csv
import os
import statistics
import sys
import time

import serial

WORDS = {0: "DON'T TOUCH", 1: "TAP ONCE", 2: "TAP TWICE", 3: "TAP 3 TIMES"}


def open_port(port, baud):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.05
    s.dtr = False  # don't reset the board when opening the port
    s.rts = False
    s.open()
    return s


class Pet:
    def __init__(self, ser):
        self.ser = ser
        self.buf = b""
        self.rows = []  # (host_time, phase, trial, line)
        self.phase, self.trial = "idle", -1

    def send(self, cmd):
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.buf += self.ser.read(4096)
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if text:
                    self.rows.append((time.time(), self.phase, self.trial, text))

    def say(self, l1, l2=""):
        self.send(f"prompt {l1}|{l2}")


def peaks(samples, floor, gap_ms=120):
    """Local maxima above floor, at least gap_ms apart: (ms, value)."""
    out = []
    for ms, v in samples:
        if v < floor:
            continue
        if out and ms - out[-1][0] < gap_ms:
            if v > out[-1][1]:
                out[-1] = (ms, v)
        else:
            out.append((ms, v))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()

    try:
        ser = open_port(args.port, args.baud)
    except serial.SerialException as e:
        sys.exit(f"Can't open {args.port}: {e}\nClose the Arduino serial monitor and try again.")
    pet = Pet(ser)
    pet.pump(1.0)

    # Quiet, awake pet with the debug stream on.
    for cmd in ("mute", "wake"):
        pet.send(cmd)
        pet.pump(0.3)
    pet.send("tapdebug")
    pet.pump(0.5)
    state = [r[3] for r in pet.rows if r[3].startswith("I,tapdebug")]
    if state and state[-1].startswith("I,tapdebug,0"):  # it was already on, we just switched it off
        pet.send("tapdebug")
        pet.pump(0.5)
        state = [r[3] for r in pet.rows if r[3].startswith("I,tapdebug")]
    if not state:
        sys.exit("The pet didn't answer 'tapdebug'. Is the new firmware uploaded?")
    mic = state[-1].endswith(",1")
    print(f"tapdebug on, microphone {'working' if mic else 'NOT working (IMU only)'}")

    trials = []
    for _ in range(args.rounds):
        trials += [1, 2, 3]
    trials += [0, 0]  # baseline: hands off (the second one: type on your keyboard)

    print("Watch the pet's screen and do what it says.\n")
    for i, n in enumerate(trials):
        hint = "type on keyboard" if n == 0 and i == len(trials) - 1 else ""
        pet.phase, pet.trial = "ready", i
        pet.say(f"{i + 1}/{len(trials)} get ready", WORDS[n].lower())
        print(f"[{i + 1}/{len(trials)}] get ready: {WORDS[n]} {hint}")
        pet.pump(2.0)
        pet.phase = "go"
        pet.say(f">> {WORDS[n]} <<", hint or "now!")
        pet.pump(3.5)
        pet.phase = "after"
        pet.say("ok, hands off", "")
        pet.pump(1.5)

    pet.phase, pet.trial = "done", -1
    pet.say("test done!", "thank you")
    pet.pump(0.5)
    pet.send("tapdebug")
    pet.send("unmute")
    pet.pump(0.5)
    pet.send("prompt")
    ser.close()

    # ---- save raw data ----
    os.makedirs(os.path.join(os.path.dirname(__file__), "tap_logs"), exist_ok=True)
    path = os.path.join(os.path.dirname(__file__), "tap_logs", time.strftime("tap_%Y%m%d_%H%M%S.csv"))
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["host_time", "phase", "trial", "expected", "line"])
        for t, ph, tr, line in pet.rows:
            w.writerow([f"{t:.3f}", ph, tr, trials[tr] if 0 <= tr < len(trials) else "", line])

    # ---- analyse ----
    def trial_data(i):
        imu, micv, taps, events, flags = [], [], 0, [], set()
        for _, ph, tr, line in pet.rows:
            if tr != i or ph not in ("go", "after"):
                continue
            p = line.split(",")
            if p[0] == "S" and len(p) >= 9:
                ms = int(p[1])
                if ph == "go":
                    imu.append((ms, float(p[2])))
                    micv.append((ms, float(p[3])))
                    if p[5] == "1": flags.add("held")
                    if p[6] == "1": flags.add("shaking")
                    if p[7] == "1": flags.add("falling")
                    if p[8] == "1": flags.add("sound")
            elif p[0] == "E":
                events.append(p[2])
                if p[2] == "taps":
                    taps += int(p[3])
        return imu, micv, taps, events, flags

    base_imu, base_mic = [], []
    for i, n in enumerate(trials):
        if n == 0:
            imu, micv, *_ = trial_data(i)
            base_imu += [v for _, v in imu]
            base_mic += [v for _, v in micv]
    noise_imu = max(base_imu) if base_imu else 0.02
    noise_mic = max(base_mic) if base_mic else 0

    print("\n#  asked  detected  IMU jolts (g)                 mic clicks                     notes")
    tap_imu, tap_mic, ok = [], [], 0
    for i, n in enumerate(trials):
        imu, micv, taps, events, flags = trial_data(i)
        ip = sorted(peaks(imu, max(0.02, noise_imu * 1.2)), key=lambda x: -x[1])[: max(n, 3)]
        mp = sorted(peaks(micv, noise_mic * 1.2 if noise_mic else 1), key=lambda x: -x[1])[: max(n, 3)]
        if n:
            tap_imu += [v for _, v in ip[:n]]
            tap_mic += [v for _, v in mp[:n]]
        good = taps == n
        ok += good
        notes = []
        if events.count("click_ignored"): notes.append(f"{events.count('click_ignored')} click(s) without jolt")
        if flags: notes.append("while " + "/".join(sorted(flags)))
        print(f"{i + 1:<2} {n:^5}  {taps:^8}  {', '.join(f'{v:.2f}' for _, v in ip):<29} "
              f"{', '.join(f'{v:.0f}' for _, v in mp):<30} {'OK ' if good else 'MISS'} {'; '.join(notes)}")

    print(f"\nCorrect: {ok}/{len(trials)}")
    print(f"Hands-off noise: IMU up to {noise_imu:.3f} g" + (f", mic up to {noise_mic:.0f}" if mic else ""))
    if tap_imu:
        lo = min(tap_imu)
        print(f"Your taps: IMU {lo:.3f} .. {max(tap_imu):.3f} g (median {statistics.median(tap_imu):.3f})")
        if lo > noise_imu:
            print(f"  -> suggested TAP_G / TAP_CONFIRM_G ~ {(lo + noise_imu) / 2:.3f}")
        else:
            print("  -> some taps are no stronger than hands-off noise: the IMU alone can't separate them")
    if mic and tap_mic:
        lo = min(tap_mic)
        print(f"Your taps: mic {lo:.0f} .. {max(tap_mic):.0f} (median {statistics.median(tap_mic):.0f})")
        if lo > noise_mic:
            print(f"  -> suggested MIC_CLICK_MIN ~ {(lo + noise_mic) / 2:.0f}")
        else:
            print("  -> typing / room noise is as loud as some taps: keep TAP_SOURCE 1 (mic + IMU together)")
    print(f"\nRaw data: {path}")


if __name__ == "__main__":
    main()
