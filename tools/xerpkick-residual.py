#!/usr/bin/env python3
"""Grade the passive recoil mirror (phase A) from a cl_xerp_debug 3 log.

Reads logs/xerp.log lines written by the metal-xerp client:

  [HH:MM:SS] xerpview <rt>: kick <K> ack <A> res <R>     per render frame
  [HH:MM:SS] xerpkick <rt>: step <n> -> <v> (frame <f>)  per mirrored shot
  [HH:MM:SS] xerpkick <rt>: spray ended - ...            per spray close

and reports, per spray and overall, how exactly the echo-driven
reconstruction A tracked the server's kick channel. The acceptance bar
for phase A: residual ~0 at every render frame of a clean standing
spray (damage/fall kicks and movement pitch ride only on the server
channel, so hits taken or moving while spraying legitimately spread the
residual — judge those windows by their timestamps).

Usage: xerpkick-residual.py [path-to-xerp.log]
Default path: ~/.aqtion-metal/action/logs/xerp.log
"""
import os
import re
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.expanduser("~/.aqtion-metal/action/logs/xerp.log")

rx_view = re.compile(
    r"\[([\d:]+)\] xerpview (\d+): kick (-?[\d.]+) ack (-?[\d.]+) res (-?[\d.]+)")
rx_step = re.compile(
    r"xerpkick (\d+): step (\d+) -> (-?[\d.]+) \(frame (\d+)\)")
rx_end = re.compile(
    r"xerpkick (\d+): spray ended - (\d+) steps, (-?[\d.]+) deep, (\d+) ms \((.+)\)")

views, steps, ends = [], [], []
with open(path, errors="replace") as f:
    for line in f:
        m = rx_view.search(line)
        if m:
            views.append((int(m.group(2)), float(m.group(3)),
                          float(m.group(4)), float(m.group(5)), m.group(1)))
            continue
        m = rx_step.search(line)
        if m:
            steps.append((int(m.group(1)), int(m.group(2)),
                          float(m.group(3)), int(m.group(4))))
            continue
        m = rx_end.search(line)
        if m:
            ends.append((int(m.group(1)), int(m.group(2)),
                         float(m.group(3)), int(m.group(4)), m.group(5)))

if not views:
    sys.exit(f"no new-format xerpview lines (kick/ack/res) in {path}")

# split render-frame rows into sprays on realtime gaps
sprays, cur = [], [views[0]]
for v in views[1:]:
    if v[0] - cur[-1][0] > 1500:
        sprays.append(cur)
        cur = []
    cur.append(v)
sprays.append(cur)

BUCKETS = (0.005, 0.26, 0.76)   # exact / one wire quantum / one step

def bucket(r):
    for i, b in enumerate(BUCKETS):
        if abs(r) <= b:
            return i
    return len(BUCKETS)

names = ["exact", "<=0.25", "<=0.75", ">0.75"]
grand = Counter()

print(f"{len(views)} render frames, {len(steps)} mirrored steps, "
      f"{len(ends)} spray closures\n")

for i, seg in enumerate(sprays):
    t0, t1 = seg[0][0], seg[-1][0]
    active = [v for v in seg if v[1] != 0 or v[2] != 0]
    if len(active) < 30:
        continue                    # single shots, tails
    c = Counter(bucket(v[3]) for v in active)
    grand.update(c)
    n = sum(c.values())
    worst = sorted(active, key=lambda v: -abs(v[3]))[:3]
    ssteps = [s for s in steps if t0 - 200 <= s[0] <= t1]
    send = [e for e in ends if t0 <= e[0] <= t1 + 1500]
    print(f"spray {i} @ {seg[0][4]} rt {t0}: {t1-t0} ms, {len(ssteps)} steps"
          f"{', end: ' + send[-1][4] if send else ''}")
    print("   residual: " + "  ".join(
        f"{names[k]} {100.0 * c.get(k, 0) / n:5.1f}%" for k in range(4)))
    kick_depth = min(v[1] for v in active)
    ack_depth = min(v[2] for v in active)
    print(f"   depth: kick {kick_depth:.2f} ack {ack_depth:.2f}")
    if c.get(2, 0) + c.get(3, 0):
        for v in worst:
            print(f"   worst t+{v[0]-t0:5d}: kick {v[1]:8.3f} "
                  f"ack {v[2]:8.3f} res {v[3]:+.3f}")
    print()

n = sum(grand.values()) or 1
print("overall active-frame residual: " + "  ".join(
    f"{names[k]} {100.0 * grand.get(k, 0) / n:5.1f}%" for k in range(4)))
print("\nphase A passes when clean standing sprays sit at ~100% 'exact'.")
