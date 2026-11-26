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
    # Opening the port reboots the pet: wait until it's up before talking to it.
    print("waiting for the pet to boot...")
    deadline = time.time() + 12
    while time.time() < deadline:
        pet.pump(0.25)
        lines = [r[3] for r in pet.rows]
        if any("Microphone" in l or "taps use the IMU" in l for l in lines):
            break
        if time.time() > deadline - 10 and not lines:  # nothing at all for 2 s: it didn't reboot
            break
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
        d = {"imu": [], "mic": [], "kicks": [], "clicks": [], "taps": 0, "events": [], "flags": set()}
        for _, ph, tr, line in pet.rows:
            if tr != i or ph not in ("go", "after"):
                continue
            p = line.split(",")
            try:
                if p[0] == "S" and len(p) >= 9 and ph == "go":
                    d["imu"].append((int(p[1]), float(p[2])))
                    d["mic"].append((int(p[1]), float(p[3])))
                    for flag, idx in (("held", 5), ("shaking", 6), ("falling", 7), ("sound", 8)):
                        if p[idx] == "1":
                            d["flags"].add(flag)
                elif p[0] == "K":
                    d["kicks"].append((int(p[1]), float(p[2])))
                elif p[0] == "C":
                    d["clicks"].append((int(p[1]), float(p[2])))
                elif p[0] == "E":
                    d["events"].append(p[2])
                    if p[2] == "taps":
                        d["taps"] += int(p[3])
            except (ValueError, IndexError):
                pass  # a line mangled on the wire
        return d

    data = [trial_data(i) for i in range(len(trials))]
    base = [d for d, n in zip(data, trials) if n == 0]
    noise_imu = max([v for d in base for _, v in d["imu"]] or [0])
    noise_mic = max([v for d in base for _, v in d["mic"]] or [0])

    print("\n#  asked detected  heard  jolts  click loudness            jolt size (g)        result")
    tap_imu, tap_mic, ok = [], [], 0
    for i, (n, d) in enumerate(zip(trials, data)):
        good = d["taps"] == n
        ok += good
        clicks = sorted((v for _, v in d["clicks"]), reverse=True)
        kicks = sorted((v for _, v in d["kicks"]), reverse=True)
        if n:
            tap_mic += clicks[:n]
            tap_imu += kicks[:n]
        notes = []
        for ev in ("click_ignored", "click_blocked"):
            if d["events"].count(ev):
                notes.append(f"{d['events'].count(ev)} {ev.replace('click_', 'click ')}")
        if d["flags"]:
            notes.append("while " + "/".join(sorted(d["flags"])))
        print(f"{i + 1:<2} {n:^5} {d['taps']:^8} {len(clicks):^6} {len(kicks):^6} "
              f"{', '.join(f'{v:.0f}' for v in clicks[:4]):<25} {', '.join(f'{v:.2f}' for v in kicks[:4]):<20} "
              f"{'OK' if good else 'MISS'} {'; '.join(notes)}")

    print(f"\nCorrect: {ok}/{len(trials)}")
    print(f"Hands-off: IMU up to {noise_imu:.3f} g, mic chunks up to {noise_mic:.0f}")
    if tap_imu:
        print(f"Tap jolts:  {min(tap_imu):.3f} .. {max(tap_imu):.3f} g  (median {statistics.median(tap_imu):.3f})")
    if tap_mic:
        print(f"Tap clicks: {min(tap_mic):.0f} .. {max(tap_mic):.0f}  (median {statistics.median(tap_mic):.0f})")
    print("ignored = mic heard a click but the pet didn't move (typing / room noise, or a jolt that was too small)")
    print("blocked = click + jolt, but the pet thought it was being held / shaken / dropped")
    print(f"\nRaw data: {path}")


if __name__ == "__main__":
    main()
