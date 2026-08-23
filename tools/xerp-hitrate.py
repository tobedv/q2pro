#!/usr/bin/env python3
"""Hit-rate analyzer for cl_xerp_ents A/B sessions.

Reads the client's console.log (hits, kills, test markers) and xerp.log
(shots fired, session/map markers) and reports hit percentages per test
segment, so settings are judged by numbers instead of impressions.

Segments are delimited by marker lines the player emits in-game via the
xerp.cfg aliases (they echo into console.log):

    alias ents_a "set cl_xerp_ents 0;   echo XERPTEST ents 0"
    alias ents_b "set cl_xerp_ents 0.5; echo XERPTEST ents 0.5"

Shot counts need per-shot fire logging: cl_xerp_fire 2 and cl_xerp_debug
2 or 4 (the default launch-metal.sh setup). Hits are correlated to SSG
shots by timestamp (within WINDOW seconds of a fired sniper shot).

Usage:
    tools/xerp-hitrate.py [--name _z0] [--logs ~/.aqtion-metal/action/logs]
"""

import argparse
import os
import re
import sys

WINDOW = 2          # seconds between an SSG shot and a hit to correlate
MZ_SSG = 14         # MZ_HYPERBLASTER: the SSG's muzzleflash code

TS = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\] ")


def ts_seconds(line):
    m = TS.match(line)
    if not m:
        return None
    h, mi, s = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s


class Segment:
    def __init__(self, label, start):
        self.label = label
        self.start = start          # seconds-of-day of the marker
        self.ssg_shots = 0
        self.ssg_hits = 0
        self.ssg_heads = 0
        self.hits = 0               # all "You hit" lines, any weapon
        self.heads = 0
        self.snipe_kills = 0        # SSG kills (any body part)
        self.snipe_headkills = 0    # "between the eyes" one-shot kills
        self.deaths = 0

    def rate(self, n, d):
        return "%d/%d (%.0f%%)" % (n, d, 100.0 * n / d) if d else "%d/0" % n


def parse(console_path, xerp_path, name):
    segments = [Segment("(before first marker)", -1)]

    # --- xerp.log: SSG shots fired, per timestamp -----------------------
    ssg_shot_times = []
    if os.path.exists(xerp_path):
        with open(xerp_path, errors="replace") as f:
            for line in f:
                t = ts_seconds(line)
                if t is None:
                    continue
                if "predicted ssg" in line or f"NOT predicted (mz {MZ_SSG})" in line:
                    ssg_shot_times.append(t)
    ssg_shot_times.sort()

    def near_ssg_shot(t):
        # binary search would be overkill for log sizes; linear from end
        for st in reversed(ssg_shot_times):
            if st > t:
                continue
            return t - st <= WINDOW
        return False

    # --- console.log: markers, hits, kills ------------------------------
    hit_re = re.compile(r"You hit (.+) in the (head|chest|stomach|legs)")
    marker_re = re.compile(r"XERPTEST (.+)")
    eyes_re = re.compile(
        r"caught a sniper bullet between the eyes from " + re.escape(name))
    ssgkill_re = re.compile(re.escape(name) + r"'s (Sniper Rifle|SSG)")
    death_re = re.compile(r"^\[[\d:]+\] " + re.escape(name) + r" ")

    with open(console_path, errors="replace") as f:
        for line in f:
            t = ts_seconds(line)
            if t is None:
                continue
            m = marker_re.search(line)
            if m:
                segments.append(Segment(m.group(1).strip(), t))
                continue
            if "Logging xerp telemetry to" in line:
                # client restart: new segment either way, marker or not
                segments.append(Segment("session %02d:%02d" %
                                        (t // 3600, t // 60 % 60), t))
                continue
            seg = segments[-1]

            m = hit_re.search(line)
            if m:
                seg.hits += 1
                head = m.group(2) == "head"
                if head:
                    seg.heads += 1
                if near_ssg_shot(t):
                    seg.ssg_hits += 1
                    if head:
                        seg.ssg_heads += 1
                continue
            if eyes_re.search(line):
                seg.snipe_headkills += 1
                seg.snipe_kills += 1
                continue
            if ssgkill_re.search(line) and " says" not in line:
                seg.snipe_kills += 1
                continue
            if death_re.match(line) and ("thanks to" in line or " by " in line
                                         or " from " in line):
                seg.deaths += 1

    # SSG shots per segment (by time range)
    bounds = [s.start for s in segments[1:]] + [24 * 3600 + 1]
    for i, seg in enumerate(segments):
        lo = seg.start
        hi = bounds[i] if i < len(bounds) else 24 * 3600 + 1
        seg.ssg_shots = sum(1 for st in ssg_shot_times if lo <= st < hi)

    return segments


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="_z0")
    ap.add_argument("--logs",
                    default=os.path.expanduser("~/.aqtion-metal/action/logs"))
    args = ap.parse_args()

    console = os.path.join(args.logs, "console.log")
    xerp = os.path.join(args.logs, "xerp.log")
    if not os.path.exists(console):
        sys.exit(f"no console.log at {console}")

    segments = parse(console, xerp, args.name)

    print(f"{'segment':<24} {'ssg shots':>9} {'ssg hits':>14} "
          f"{'ssg heads':>10} {'ssg kills':>9} {'1-tap':>5} "
          f"{'all hits':>8} {'deaths':>6}")
    for seg in segments:
        if not (seg.hits or seg.ssg_shots or seg.deaths):
            continue
        print(f"{seg.label:<24} {seg.ssg_shots:>9} "
              f"{seg.rate(seg.ssg_hits, seg.ssg_shots):>14} "
              f"{seg.ssg_heads:>10} {seg.snipe_kills:>9} "
              f"{seg.snipe_headkills:>5} {seg.hits:>8} {seg.deaths:>6}")
    print("\nnote: 'ssg hits' correlates You-hit lines to SSG shots within "
          f"{WINDOW}s (needs cl_xerp_fire 2 + cl_xerp_debug 2/4 for shot "
          "counts); day-crossing sessions split at midnight.")


if __name__ == "__main__":
    main()
