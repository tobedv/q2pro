#!/usr/bin/env python3
"""Grade the M4 recoil mirror/generator from a cl_xerp_debug 3 log.

Phase A lines:  xerpview <rt>: kick <K> ack <A> res <R>
Phase B lines:  xerpview <rt>: kick <K> ack <A> gen <S> out <O>
Events:         xerpkick <rt>: step/spray/gen released/RESIDUE ...

Grades per spray:
- cancellation: |kick - ack| residual buckets (phase A bar: ~100% exact
  on clean standing sprays; contamination legitimately spreads it);
- shape: with prediction on, the rendered curve (out) must be built from
  classic pieces only — every slope a ladder-step lerp (<= 0.75 deg per
  100 ms) or a release (<= 16.5 deg per 100 ms), and its junction values
  on the trunc(-2.8n)/4 ladder;
- lead: how much earlier out moved than kick (the point of phase B).

Usage: xerpkick-residual.py [path-to-xerp.log]
"""
import os
import re
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.expanduser("~/.aqtion-metal/action/logs/xerp.log")

rx4 = re.compile(r"\[([\d:]+)\] xerpview (\d+): kick (-?[\d.]+) "
                 r"ack (-?[\d.]+) gen (-?[\d.]+) out (-?[\d.]+)")
rx3 = re.compile(r"\[([\d:]+)\] xerpview (\d+): kick (-?[\d.]+) "
                 r"ack (-?[\d.]+) res (-?[\d.]+)")

rows = []
for line in open(path, errors="replace"):
    m = rx4.search(line)
    if m:
        rows.append((int(m.group(2)), float(m.group(3)), float(m.group(4)),
                     float(m.group(5)), float(m.group(6)), m.group(1)))
        continue
    m = rx3.search(line)
    if m:
        k = float(m.group(3))
        a = float(m.group(4))
        rows.append((int(m.group(2)), k, a, 0.0, k, m.group(1)))

if not rows:
    sys.exit(f"no xerpview lines in {path}")

sprays, cur = [], [rows[0]]
for r in rows[1:]:
    if r[0] - cur[-1][0] > 1500 or r[0] < cur[-1][0]:
        sprays.append(cur)
        cur = []
    cur.append(r)
sprays.append(cur)

LADDER = [int(-2.8 * n) / 4.0 for n in range(1, 24)] + [0.0]
BUCKETS = (0.005, 0.26, 0.76)
NAMES = ["exact", "<=0.25", "<=0.75", ">0.75"]


def bucket(r):
    for i, b in enumerate(BUCKETS):
        if abs(r) <= b:
            return i
    return len(BUCKETS)


def shape_check(ts, vs):
    """Return (bad_slopes, off_ladder, max_slope_per_100ms) for a curve."""
    bad = 0
    max_slope = 0.0
    releasing = False
    for i in range(1, len(ts)):
        dt = ts[i] - ts[i - 1]
        if dt < 3 or dt > 50:
            continue    # sub-3ms pairs are print-quantization noise
        slope = (vs[i] - vs[i - 1]) / dt * 100.0
        max_slope = max(max_slope, abs(slope))
        # release pieces head toward zero and may run at full cap depth
        # per 100 ms; climb pieces may not exceed one ladder rung
        heading_up = vs[i] > vs[i - 1] + 1e-6
        limit = 16.6 if (heading_up or releasing) else 0.80
        if abs(slope) > limit:
            bad += 1
        releasing = heading_up
    junctions = []
    for i in range(1, len(ts) - 1):
        if ts[i] == ts[i-1] or ts[i+1] == ts[i]:
            continue
        s1 = (vs[i] - vs[i-1]) / (ts[i] - ts[i-1])
        s2 = (vs[i+1] - vs[i]) / (ts[i+1] - ts[i])
        if abs(s2 - s1) > 1.5e-3:
            # a release can start from any mid-lerp value — only climb
            # junctions (curve continuing downward) must sit on the ladder
            junctions.append((vs[i], s2 < 0))
    off = sum(1 for v, down in junctions
              if down and min(abs(v - q) for q in LADDER) > 0.06)
    return bad, off, len(junctions), max_slope


grand = Counter()
n_shape_bad = 0

print(f"{len(rows)} render frames across {len(sprays)} segments\n")

for i, seg in enumerate(sprays):
    ts = [r[0] for r in seg]
    kicks = [r[1] for r in seg]
    acks = [r[2] for r in seg]
    gens = [r[3] for r in seg]
    outs = [r[4] for r in seg]
    active = [(t, k, a, g, o) for t, k, a, g, o, _ in seg
              if k != 0 or a != 0 or g != 0 or o != 0]
    if len(active) < 30:
        continue
    c = Counter(bucket(k - a) for _, k, a, _, _ in active)
    grand.update(c)
    n = sum(c.values())
    predicted = any(g != 0 for _, _, _, g, _ in active)
    print(f"spray {i} @ {seg[0][5]} rt {ts[0]}: {ts[-1]-ts[0]} ms, "
          f"{'PREDICTED' if predicted else 'baseline'}")
    print("   cancel : " + "  ".join(
        f"{NAMES[k]} {100.0 * c.get(k, 0) / n:5.1f}%" for k in range(4)))
    print(f"   depth  : kick {min(kicks):.2f} ack {min(acks):.2f} "
          f"gen {min(gens):.2f} out {min(outs):.2f}")
    if predicted:
        bad, off, junc, mslope = shape_check(ts, outs)
        n_shape_bad += bad + off
        t_out = next((t for t, _, _, _, o in active if o < -0.05), None)
        t_kick = next((t for t, k, _, _, _ in active if k < -0.05), None)
        lead = (t_kick - t_out) if (t_out and t_kick) else 0
        print(f"   shape  : {junc} junctions, {off} off-ladder, "
              f"{bad} over-rate samples, max slope {mslope:.2f} deg/100ms")
        print(f"   lead   : out moved {lead} ms before kick")
    print()

n = sum(grand.values()) or 1
print("overall cancellation: " + "  ".join(
    f"{NAMES[k]} {100.0 * grand.get(k, 0) / n:5.1f}%" for k in range(4)))
if n_shape_bad:
    print(f"SHAPE VIOLATIONS: {n_shape_bad} — rendered curve broke classic rates")
else:
    print("shape: all predicted-spray pieces within classic rates")
