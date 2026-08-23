#!/usr/bin/env python3
"""Playtest database: settings + analytical results per map segment.

Ingests the client's xerp.log (telemetry, `=== session/map/settings`
markers) and console.log (hits, kills, deaths) into a SQLite file, so
weeks of playtest games can be compared by configuration:

    tools/xerp-playtest.py ingest              # parse logs -> db (idempotent)
    tools/xerp-playtest.py report              # aggregate by settings
    tools/xerp-playtest.py report --by map     # aggregate by map
    tools/xerp-playtest.py segments            # list raw segments

A segment is a span of one map under one settings state — changing any
cl_xerp_* dial mid-map starts a new segment (the engine logs a
`=== settings` line on every change and at every map start). Sessions
already ingested are replaced, so re-running ingest after games is
always safe.
"""

import argparse
import json
import os
import re
import bisect
import sqlite3
import sys

DB_DEFAULT = os.path.expanduser("~/.aqtion-metal/playtest.db")
LOGS_DEFAULT = os.path.expanduser("~/.aqtion-metal/action/logs")

TS = re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})\] ")
SESSION = re.compile(r"^=== session ([\d-]+ [\d:]+), (.+)$")
MAP = re.compile(r"=== map (\S+) @ (\S+)")
SETTINGS = re.compile(
    r'=== settings fire=(\d+) cut=([\d.]+) weapons="([^"]*)" ents=([\d.]+) '
    r"minspeed=(\d+) cancel=(\d+)")
PRED = re.compile(r"predicted (\w+)")
ECHO_MS = re.compile(r"click-to-echo (\d+) ms")
XERPENTS = re.compile(
    r"players (\d+) ext, err avg ([\d.]+) max ([\d.]+) \((\d+) checks\)")
SPRAY = re.compile(r"spray A=(\d+) S=(\d+) .*lead (\d+) ms")
HIT = re.compile(r"You hit (.+) in the (head|chest|stomach|legs)")
HEADKILL = re.compile(
    r"had a makeover by|between the eyes from|hole in .{0,8} head|"
    r"caught a bullet in the head|brains")
WEAPONS_TAIL = re.compile(
    r"'s (M4 Assault Rifle|MP5/10 Submachinegun|Mark 23 [Pp]istol|"
    r"Dual .?Mark 23|M3 Super 90|Handcannon|Sniper Rifle|Combat Knife|"
    r"[Hh]andgrenade)")


def secs(line):
    m = TS.match(line)
    if not m:
        return None
    h, mi, s = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s


class Seg:
    def __init__(self, session, map_, server, t_start, settings):
        self.session, self.map, self.server = session, map_, server
        self.t_start, self.t_end = t_start, t_start
        self.settings = dict(settings) if settings else None
        self.m = {"pred": {}, "consumed": 0, "echo_ms": [], "absorbed": 0,
                  "not_predicted": 0, "never_echoed": 0, "residue": 0,
                  "sprays": 0, "spray_lead_ms": [], "ents_ext": 0,
                  "ents_checks": 0, "ents_err_sum": 0.0, "ents_err_max": 0.0,
                  "ssg_shot_t": [], "hits": {}, "ssg_hits": 0,
                  "ssg_heads": 0, "kills": {}, "head_kills": 0, "deaths": 0}

    def empty(self):
        m = self.m
        return not (m["pred"] or m["hits"] or m["kills"] or m["deaths"]
                    or m["not_predicted"] or m["ents_checks"])


def parse_xerp(path):
    """-> list of sessions, each a list of Seg (console pass fills more)."""
    sessions = []
    cur_session = None
    cur_settings = None
    seg = None
    segs = None

    def new_seg(map_, server, t):
        nonlocal seg
        seg = Seg(cur_session, map_, server, t, cur_settings)
        segs.append(seg)

    for line in open(path, errors="replace"):
        sm = SESSION.match(line)
        if sm:
            cur_session = sm.group(1)
            cur_settings = None
            segs = []
            sessions.append((cur_session, segs))
            seg = None
            continue
        if cur_session is None:
            continue
        t = secs(line)
        if t is None:
            continue
        mm = MAP.search(line)
        if mm:
            new_seg(mm.group(1), mm.group(2), t)
            continue
        sm = SETTINGS.search(line)
        if sm:
            cur_settings = {
                "fire": int(sm.group(1)), "cut": float(sm.group(2)),
                "weapons": sm.group(3), "ents": float(sm.group(4)),
                "minspeed": int(sm.group(5)), "cancel": int(sm.group(6))}
            if seg and not seg.empty():
                new_seg(seg.map, seg.server, t)   # dial change mid-map
            elif seg:
                seg.settings = dict(cur_settings)
            continue
        if not seg:
            continue
        seg.t_end = max(seg.t_end, t)
        m = seg.m
        pm = PRED.search(line)
        if pm and "xerpfire" in line:
            m["pred"][pm.group(1)] = m["pred"].get(pm.group(1), 0) + 1
            if pm.group(1) == "ssg":
                m["ssg_shot_t"].append(t)
        em = ECHO_MS.search(line)
        if em:
            m["consumed"] += 1
            m["echo_ms"].append(int(em.group(1)))
        if "absorbed" in line:
            m["absorbed"] += 1
        if "NOT predicted" in line:
            m["not_predicted"] += 1
            if "(mz 14)" in line:
                m["ssg_shot_t"].append(t)
        if "never echoed" in line:
            m["never_echoed"] += 1
        if "RESIDUE" in line:
            m["residue"] += 1
        sm = SPRAY.search(line)
        if sm:
            m["sprays"] += 1
            if int(sm.group(3)):
                m["spray_lead_ms"].append(int(sm.group(3)))
        xm = XERPENTS.search(line)
        if xm:
            m["ents_ext"] += int(xm.group(1))
            checks = int(xm.group(4))
            m["ents_checks"] += checks
            m["ents_err_sum"] += float(xm.group(2)) * checks
            m["ents_err_max"] = max(m["ents_err_max"], float(xm.group(3)))
    return sessions


def parse_console(path, sessions, name):
    """Second pass: distribute console events into segments by session
    order ('Logging xerp telemetry' boundaries) and timestamp."""
    # console.log holds boot boundaries from before the session-marker
    # era too — the LAST len(sessions) boundaries are the marker-era ones
    total = sum(1 for l in open(path, errors="replace")
                if "Logging xerp telemetry to" in l)
    first = total - len(sessions)
    sidx = -1
    segs = []
    for line in open(path, errors="replace"):
        if "Logging xerp telemetry to" in line:
            sidx += 1
            k = sidx - first
            segs = sessions[k][1] if 0 <= k < len(sessions) else []
            continue
        if not segs:
            continue
        t = secs(line)
        if t is None:
            continue
        seg = None
        for s in segs:
            if s.t_start <= t <= s.t_end + 30:
                seg = s
        if seg is None:
            continue
        m = seg.m
        hm = HIT.search(line)
        if hm:
            loc = hm.group(2)
            m["hits"][loc] = m["hits"].get(loc, 0) + 1
            i = bisect.bisect_right(m["ssg_shot_t"], t)
            if i and t - m["ssg_shot_t"][i - 1] <= 2:
                m["ssg_hits"] += 1
                if loc == "head":
                    m["ssg_heads"] += 1
            continue
        after = TS.sub("", line)
        if " says" in line or ": " in after[len(name):] and after.startswith(name + ":"):
            continue
        involved = f"{name}'s " in line or f"from {name}" in line
        if after.startswith(name + " "):
            if ("thanks to" in line or " by " in line or " from " in line
                    or "didn't" in line or "was " in after):
                m["deaths"] += 1
        elif involved:
            wm = WEAPONS_TAIL.search(line)
            wep = wm.group(1) if wm else (
                "Sniper Rifle" if "between the eyes" in line else "other")
            m["kills"][wep] = m["kills"].get(wep, 0) + 1
            if HEADKILL.search(line):
                m["head_kills"] += 1


def ingest(args):
    xerp = os.path.join(args.logs, "xerp.log")
    console = os.path.join(args.logs, "console.log")
    sessions = parse_xerp(xerp)
    if os.path.exists(console):
        parse_console(console, sessions, args.name)

    db = sqlite3.connect(args.db)
    db.execute("""CREATE TABLE IF NOT EXISTS segments(
        session TEXT, seg INTEGER, map TEXT, server TEXT,
        t_start TEXT, dur_s INTEGER, settings TEXT, metrics TEXT,
        PRIMARY KEY(session, seg))""")
    n = 0
    for session, segs in sessions:
        db.execute("DELETE FROM segments WHERE session = ?", (session,))
        for i, s in enumerate(seg for seg in segs if not seg.empty()):
            s.m["ssg_shots"] = len(s.m.pop("ssg_shot_t", []))
            db.execute("INSERT INTO segments VALUES(?,?,?,?,?,?,?,?)",
                       (session, i, s.map, s.server,
                        "%02d:%02d:%02d" % (s.t_start // 3600,
                                            s.t_start // 60 % 60,
                                            s.t_start % 60),
                        s.t_end - s.t_start,
                        json.dumps(s.settings) if s.settings else None,
                        json.dumps(s.m)))
            n += 1
    db.commit()
    print(f"ingested {len(sessions)} sessions, {n} segments -> {args.db}")


def fmt_pct(n, d):
    return "%3d/%-3d %3.0f%%" % (n, d, 100.0 * n / d) if d else "  -    "


def report(args):
    db = sqlite3.connect(args.db)
    rows = db.execute("SELECT session,map,server,dur_s,settings,metrics "
                      "FROM segments").fetchall()
    groups = {}
    for session, map_, server, dur, settings, metrics in rows:
        st = json.loads(settings) if settings else {}
        m = json.loads(metrics)
        if args.by == "map":
            key = map_
        else:
            key = "ents=%.2f weapons=[%s]" % (
                st.get("ents", -1), st.get("weapons", "?")) \
                if st else "(settings unknown)"
        g = groups.setdefault(key, {"dur": 0, "segs": 0, "ssg_shots": 0,
                                    "ssg_hits": 0, "ssg_heads": 0,
                                    "kills": 0, "hkills": 0, "deaths": 0,
                                    "echo": [], "err_sum": 0.0, "checks": 0})
        g["dur"] += dur
        g["segs"] += 1
        g["ssg_hits"] += m["ssg_hits"]
        g["ssg_heads"] += m["ssg_heads"]
        g["kills"] += sum(m["kills"].values())
        g["hkills"] += m["head_kills"]
        g["deaths"] += m["deaths"]
        g["echo"] += m["echo_ms"]
        g["err_sum"] += m["ents_err_sum"]
        g["checks"] += m["ents_checks"]
        g.setdefault("_ssg_total", 0)
        g["_ssg_total"] += m.get("ssg_shots", 0)

    print(f"{'group':<40} {'min':>4} {'ssg hit%':>12} {'heads':>5} "
          f"{'K':>4} {'HK':>3} {'D':>4} {'K/D':>5} {'echo':>5} {'entserr':>7}")
    for key in sorted(groups):
        g = groups[key]
        ssg = g["_ssg_total"]
        echo = sum(g["echo"]) // len(g["echo"]) if g["echo"] else 0
        err = g["err_sum"] / g["checks"] if g["checks"] else 0
        kd = g["kills"] / g["deaths"] if g["deaths"] else float(g["kills"])
        print(f"{key:<40} {g['dur'] // 60:>4} "
              f"{fmt_pct(g['ssg_hits'], ssg):>12} {g['ssg_heads']:>5} "
              f"{g['kills']:>4} {g['hkills']:>3} {g['deaths']:>4} "
              f"{kd:>5.2f} {echo:>4}ms {err:>6.1f}u")


def segments(args):
    db = sqlite3.connect(args.db)
    for r in db.execute("SELECT session,seg,map,server,t_start,dur_s,"
                        "settings FROM segments ORDER BY session,seg"):
        st = json.loads(r[6]) if r[6] else {}
        print(f"{r[0]}  #{r[1]} {r[2]:<12} {r[4]} {r[5] // 60:>3}min  "
              f"ents={st.get('ents', '?')} weapons={st.get('weapons', '?')}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["ingest", "report", "segments"])
    ap.add_argument("--db", default=DB_DEFAULT)
    ap.add_argument("--logs", default=LOGS_DEFAULT)
    ap.add_argument("--name", default="_z0")
    ap.add_argument("--by", default="settings", choices=["settings", "map"])
    args = ap.parse_args()
    {"ingest": ingest, "report": report, "segments": segments}[args.cmd](args)


if __name__ == "__main__":
    main()
