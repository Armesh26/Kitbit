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

WORDS = {0: "DON'T TOUCH", 1: "TAP ONCE", 2: "TAP TWICE", 3: "TAP 3 TIMES", -1: "SLAM THE DESK",
         -2: "SCRATCH MY HEAD", -3: "TYPE ON KEYBOARD", -4: "MOVE ME AROUND"}
SLAM, SCRATCH, TYPE, MOVE = -1, -2, -3, -4
CONTEXT_INTRO = {"desk": ("PUT ME ON THE DESK", "and let go"),
                 "hand": ("HOLD ME IN YOUR HAND", "tilted, keep holding")}


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
    ap.add_argument("--slam", action="store_true", help="tap once vs slam the desk, to tell them apart")
    ap.add_argument("--scratch", action="store_true", help="scratching the head vs typing vs taps")
    ap.add_argument("--context", action="store_true", help="every gesture on the desk and in your hand")
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

    trials, ctxs = [], []
    if args.context:
        for ctx, block in (("desk", [1, 2, 3, SCRATCH, SLAM, 0]), ("hand", [1, 2, 3, SCRATCH, 0, MOVE])):
            trials += block
            ctxs += [ctx] * len(block)
    elif args.scratch:
        trials = [SCRATCH, TYPE, SCRATCH, 0, SCRATCH, 1, TYPE, SCRATCH]
    elif args.slam:
        for _ in range(max(args.rounds, 4)):
            trials += [1, SLAM]
    else:
        for _ in range(args.rounds):
            trials += [1, 2, 3]
        trials += [0, 0]  # baseline: hands off (the second one: type on your keyboard)

    if len(ctxs) != len(trials):
        ctxs = ["desk"] * len(trials)
    print("Watch the pet's screen and do what it says.\n")
    for i, n in enumerate(trials):
        if args.context and (i == 0 or ctxs[i] != ctxs[i - 1]):
            pet.phase, pet.trial = "intro", -1
            pet.say(*CONTEXT_INTRO[ctxs[i]])
            print(f"== {CONTEXT_INTRO[ctxs[i]][0]} ==")
            pet.pump(7.0)
        hint = "type on keyboard" if n == 0 and i == len(trials) - 1 and not args.scratch else ""
        if n == SCRATCH:
            hint = "keep going 4 s"
        elif n == MOVE:
            hint = "slowly, no taps"
        elif n == 0 and args.context and ctxs[i] == "hand":
            hint = "just hold still"
        pet.phase, pet.trial = "ready", i
        pet.say(f"{i + 1}/{len(trials)} get ready", WORDS[n].lower())
        print(f"[{i + 1}/{len(trials)}] get ready: {WORDS[n]} {hint}")
        pet.pump(2.0)
        pet.phase = "go"
        pet.say(f">> {WORDS[n]} <<", hint or "now!")
        pet.pump(4.0 if (args.scratch or args.context) else 3.5)
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
                    if len(p) >= 11:
                        d.setdefault("gyro", []).append(float(p[9]))
                        d.setdefault("surface", []).append(p[10] == "1")
                elif p[0] == "K":
                    d["kicks"].append((int(p[1]), float(p[2])))
                    if len(p) >= 6:
                        d.setdefault("dirs", []).append((int(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5])))
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
        clean = d["taps"] == 0 and "bump" not in d["events"] and "scratch_start" not in d["events"]
        if n == SLAM:
            good = d["taps"] == 0 and "bump" in d["events"]
        elif n in (0, MOVE):
            good = clean
        elif n == SCRATCH:
            good = d["taps"] == 0 and "scratch_start" in d["events"]
        elif n == TYPE:
            good = d["taps"] == 0 and "scratch_start" not in d["events"]
        else:
            good = d["taps"] == n
        ok += good
        clicks = sorted((v for _, v in d["clicks"]), reverse=True)
        kicks = sorted((v for _, v in d["kicks"]), reverse=True)
        if n > 0:
            tap_mic += clicks[:n]
            tap_imu += kicks[:n]
        notes = []
        for ev in ("click_ignored", "click_blocked"):
            if d["events"].count(ev):
                notes.append(f"{d['events'].count(ev)} {ev.replace('click_', 'click ')}")
        if d["flags"]:
            notes.append("while " + "/".join(sorted(d["flags"])))
        if args.context and (i == 0 or ctxs[i] != ctxs[i - 1]):
            print(f"--- {ctxs[i]} ---")
        if d.get("surface"):
            notes.append(f"surface {100 * sum(d['surface']) / len(d['surface']):.0f}%, gyro {statistics.median(d['gyro']):.1f} dps")
        for ev in ("bump", "scratch_start"):
            if ev in d["events"] and n not in (SLAM, SCRATCH):
                notes.append(ev)
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
    if args.scratch:
        # Sustained features over the whole "go" window, per ~100 ms window.
        print("\nwhat each activity looks like over time (per 100 ms window):")
        print("  trial kind       mic loud%  mic level/bg (median)  IMU busy%  IMU level g (median)  biggest jolt")
        names = {SCRATCH: "scratch", TYPE: "typing", 0: "nothing", 1: "tap"}
        for i, (n, d) in enumerate(zip(trials, data)):
            imu, micv = d["imu"], d["mic"]
            if not imu:
                continue
            bg = max(1.0, statistics.median([float(l.split(",")[4]) for _, ph, tr, l in pet.rows
                                              if tr == i and ph == "go" and l.startswith("S,")] or [1]))
            t0 = imu[0][0]
            wins = {}
            for (t, a), (_, m) in zip(imu, micv):
                w = (t - t0) // 100
                wa, wm = wins.get(w, (0, 0))
                wins[w] = (max(wa, a), max(wm, m))
            ws = list(wins.values())
            mic_ratio = [m / bg for _, m in ws]
            loud = sum(r > 4 for r in mic_ratio) / len(ws)
            busy = sum(a > 0.03 for a, _ in ws) / len(ws)
            print(f"  {i + 1:<5} {names.get(n, n):<9} {loud * 100:8.0f}%  {statistics.median(mic_ratio):20.1f}  "
                  f"{busy * 100:8.0f}%  {statistics.median(a for a, _ in ws):19.3f}  {max(a for a, _ in ws):11.2f}")

    if args.slam:
        # Features of the first event in each trial, to see what separates a tap from a slam.
        print("\nfeatures of the first hit:  kind   size g  up-share  side-share  ring ms  #jolts/300ms  click")
        feats = {1: [], SLAM: []}
        for i, (n, d) in enumerate(zip(trials, data)):
            dirs = d.get("dirs", [])
            if not dirs:
                print(f"  trial {i + 1}: nothing felt")
                continue
            t0 = dirs[0][0]
            burst = [x for x in dirs if x[0] - t0 <= 300]
            big = max(burst, key=lambda x: x[1])
            m = big[1] or 1
            up, side = abs(big[3]) / m, (abs(big[2]) + abs(big[4])) / m
            ring = max(x[0] for x in burst if x[1] > 0.1) - t0 if any(x[1] > 0.1 for x in burst) else 0
            click = max([v for t, v in d["clicks"] if abs(t - t0) < 200] or [0])
            feats[n].append((big[1], up, ring, len(burst), click))
            print(f"  trial {i + 1:<2} {'SLAM' if n == SLAM else 'tap ':<6} {big[1]:6.2f}  {up:8.2f}  {side:10.2f}  {ring:7d}  {len(burst):12d}  {click:9.0f}")
        for n, name in ((1, "taps "), (SLAM, "slams")):
            if feats[n]:
                cols = list(zip(*feats[n]))
                print(f"  {name}: size {min(cols[0]):.2f}-{max(cols[0]):.2f} g, up-share {min(cols[1]):.2f}-{max(cols[1]):.2f}, "
                      f"ring {min(cols[2])}-{max(cols[2])} ms, jolts {min(cols[3])}-{max(cols[3])}, click {min(cols[4]):.0f}-{max(cols[4]):.0f}")
    print(f"\nRaw data: {path}")


if __name__ == "__main__":
    main()
