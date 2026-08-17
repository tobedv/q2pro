/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "client.h"

/*
===================
CL_CheckPredictionError
===================
*/
void CL_CheckPredictionError(void)
{
    int         frame;
    int         delta[3];
    unsigned    cmd;
    int         len;

    if (cls.demo.playback) {
        return;
    }

    if (sv_paused->integer) {
        VectorClear(cl.prediction_error);
        return;
    }

    if (!cl_predict->integer || (cl.frame.ps.pmove.pm_flags & PMF_NO_PREDICTION))
        return;

    // calculate the last usercmd_t we sent that the server has processed
    frame = cls.netchan.incoming_acknowledged & CMD_MASK;
    cmd = cl.history[frame].cmdNumber;

    // compare what the server returned with what we had predicted it to be
    VectorSubtract(cl.frame.ps.pmove.origin, cl.predicted_origins[cmd & CMD_MASK], delta);

    // save the prediction error for interpolation
    len = abs(delta[0]) + abs(delta[1]) + abs(delta[2]);
    if (len <= 1 || len > 640) {
        // > 80 world units is a teleport or something
        VectorClear(cl.prediction_error);
        return;
    }

    SHOWMISS("prediction miss on %i: %i (%d %d %d)\n",
             cl.frame.number, len, delta[0], delta[1], delta[2]);

    // don't predict steps against server returned data
    if (cl.predicted_step_frame <= cmd)
        cl.predicted_step_frame = cmd + 1;

    VectorCopy(cl.frame.ps.pmove.origin, cl.predicted_origins[cmd & CMD_MASK]);

    // save for error interpolation
    VectorScale(delta, 0.125f, cl.prediction_error);
}

/*
====================
CL_ClipMoveToEntities
====================
*/
static void CL_ClipMoveToEntities(trace_t *tr, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, int contentmask)
{
    int         i;
    trace_t     trace;
    const mnode_t   *headnode;
    const centity_t *ent;
    const mmodel_t  *cmodel;

    for (i = 0; i < cl.numSolidEntities; i++) {
        ent = cl.solidEntities[i];

        if (cl.csr.extended && ent->current.number <= cl.maxclients && !(contentmask & CONTENTS_PLAYER))
            continue;

        if (ent->current.solid == PACKED_BSP) {
            // special value for bmodel
            cmodel = cl.model_clip[ent->current.modelindex];
            if (!cmodel)
                continue;
            headnode = cmodel->headnode;
        } else {
            headnode = CM_HeadnodeForBox(ent->mins, ent->maxs);
        }

        if (tr->allsolid)
            return;

        CM_TransformedBoxTrace(&trace, start, end,
                               mins, maxs, headnode, contentmask,
                               ent->current.origin, ent->current.angles,
                               cl.csr.extended);

        CM_ClipEntity(tr, &trace, (struct edict_s *)ent);
    }
}

/*
================
CL_Trace
================
*/
void CL_Trace(trace_t *tr, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs, int contentmask)
{
    // check against world
    CM_BoxTrace(tr, start, end, mins, maxs, cl.bsp->nodes, contentmask, cl.csr.extended);
    tr->ent = (struct edict_s *)cl_entities;
    if (tr->fraction == 0)
        return;     // blocked by the world

    // check all other solid models
    CL_ClipMoveToEntities(tr, start, end, mins, maxs, contentmask);
}

static int pm_clipmask;

static trace_t q_gameabi CL_PMTrace(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int contentmask)
{
    trace_t t;
    CL_Trace(&t, start, end, mins, maxs, (cl.csr.extended && contentmask) ? contentmask : pm_clipmask);
    return t;
}

static int CL_PointContents(const vec3_t point)
{
    const centity_t *ent;
    const mmodel_t  *cmodel;
    int i, contents;

    contents = CM_PointContents(point, cl.bsp->nodes, cl.csr.extended);

    for (i = 0; i < cl.numSolidEntities; i++) {
        ent = cl.solidEntities[i];

        if (ent->current.solid != PACKED_BSP) // special value for bmodel
            continue;

        cmodel = cl.model_clip[ent->current.modelindex];
        if (!cmodel)
            continue;

        contents |= CM_TransformedPointContents(point, cmodel->headnode, ent->current.origin,
                                                ent->current.angles, cl.csr.extended);
    }

    return contents;
}

/*
=================
CL_PredictMovement

Sets cl.predicted_origin and cl.predicted_angles
=================
*/
void CL_PredictAngles(void)
{
    cl.predicted_angles[0] = cl.viewangles[0] + SHORT2ANGLE(cl.frame.ps.pmove.delta_angles[0]);
    cl.predicted_angles[1] = cl.viewangles[1] + SHORT2ANGLE(cl.frame.ps.pmove.delta_angles[1]);
    cl.predicted_angles[2] = cl.viewangles[2] + SHORT2ANGLE(cl.frame.ps.pmove.delta_angles[2]);
}

void CL_PredictMovement(void)
{
    unsigned    ack, current, frame;
    pmove_t     pm;
    int         step, oldz;

    if (cls.state != ca_active) {
        return;
    }

    if (cls.demo.playback) {
        return;
    }

    if (sv_paused->integer) {
        return;
    }

    if (!cl_predict->integer || (cl.frame.ps.pmove.pm_flags & PMF_NO_PREDICTION)) {
        // just set angles
        CL_PredictAngles();
        return;
    }

    ack = cl.history[cls.netchan.incoming_acknowledged & CMD_MASK].cmdNumber;
    current = cl.cmdNumber;

    // if we are too far out of date, just freeze
    if (current - ack > CMD_BACKUP - 1) {
        SHOWMISS("%i: exceeded CMD_BACKUP\n", cl.frame.number);
        return;
    }

    if (!cl.cmd.msec && current == ack) {
        SHOWMISS("%i: not moved\n", cl.frame.number);
        return;
    }

    pm_clipmask = MASK_PLAYERSOLID;

    // remaster player collision rules
    if (cl.csr.extended) {
        if (cl.frame.ps.pmove.pm_type == PM_DEAD || cl.frame.ps.pmove.pm_type == PM_GIB)
            pm_clipmask = MASK_DEADSOLID;

        if (!(cl.frame.ps.pmove.pm_flags & PMF_IGNORE_PLAYER_COLLISION))
            pm_clipmask |= CONTENTS_PLAYER;
    }

    // copy current state to pmove
    memset(&pm, 0, sizeof(pm));
    pm.trace = CL_PMTrace;
    pm.pointcontents = CL_PointContents;
    pm.s = cl.frame.ps.pmove;
    pm.snapinitial = qtrue;

    // run frames
    while (++ack <= current) {
        pm.cmd = cl.cmds[ack & CMD_MASK];
        PmoveNew(&pm, &cl.pmp);
        pm.snapinitial = qfalse;

        // save for debug checking
        VectorCopy(pm.s.origin, cl.predicted_origins[ack & CMD_MASK]);
    }

    // run pending cmd
    if (cl.cmd.msec) {
        pm.cmd = cl.cmd;
        pm.cmd.forwardmove = cl.localmove[0];
        pm.cmd.sidemove = cl.localmove[1];
        pm.cmd.upmove = cl.localmove[2];
        PmoveNew(&pm, &cl.pmp);
        frame = current;

        // save for debug checking
        VectorCopy(pm.s.origin, cl.predicted_origins[(current + 1) & CMD_MASK]);
    } else {
        frame = current - 1;
    }

    if (pm.s.pm_type != PM_SPECTATOR && (pm.s.pm_flags & PMF_ON_GROUND)) {
        oldz = cl.predicted_origins[cl.predicted_step_frame & CMD_MASK][2];
        step = pm.s.origin[2] - oldz;
        if (step >= 63 && step < 160) {
            // check for stepping up before a previous step is completed
            unsigned delta = cls.realtime - cl.predicted_step_time;
            float prev_step = 0;
            if (delta < 100)
                prev_step = cl.predicted_step * (100 - delta) * 0.01f;

            cl.predicted_step = min(prev_step + step * 0.125f, 32);
            cl.predicted_step_time = cls.realtime;
            cl.predicted_step_frame = frame + 1;    // don't double step
        }
    }

    if (cl.predicted_step_frame < frame) {
        cl.predicted_step_frame = frame;
    }


	if (cl.predicted_viewheight_frame != cl.frame.number
#if USE_FPS
		&& cl.frame.number == cl.keyframe.number
#endif
		)
	{
		cl.predicted_viewheight_frame = cl.frame.number;
		cl.predicted_viewheight[1] = cl.predicted_viewheight[0];

		if (cl.view_predict)
		{
			if (pm.s.pm_flags & PMF_DUCKED)
				cl.predicted_viewheight[0] = cl.view_low;
			else
				cl.predicted_viewheight[0] = cl.view_high;
		}
		else
		{
			cl.predicted_viewheight[0] = pm.viewheight;
		}
	}


    // copy results out for rendering
    VectorScale(pm.s.origin, 0.125f, cl.predicted_origin);
    VectorScale(pm.s.velocity, 0.125f, cl.predicted_velocity);
    VectorCopy(pm.viewangles, cl.predicted_angles);
}

/*
==============================================================================
XERP FIRE — predicted local weapon fire feedback (cl_xerp_fire, Phase 1 of
the cl_xerp_* netcode feel features).

When the attack input is sampled and the mirrored weapon state says the shot
will happen, the muzzle flash and fire sound play immediately instead of
after the server round-trip. The echoed svc_muzzleflash is then consumed so
nothing plays twice. Fire effects only: bullets, hits, blood and damage
remain fully server-authoritative — this cannot create or remove a hit.
==============================================================================
*/

cvar_t *cl_xerp_fire;

// All xerp telemetry is written to its own file, logs/xerp.log, instead of
// the console — full per-shot data without drowning out chat and game
// messages. Rare anomalies (zoom REVERTED) additionally go to the console.
static qhandle_t xerp_logfile;

void CL_XerpLog(const char *fmt, ...)
{
    char buf[MAXPRINTMSG], line[MAXPRINTMSG], stamp[32], path[MAX_OSPATH];
    va_list ap;
    size_t len;

    if (SCR_XerpDebugLevel() < 2)
        return;
    if (xerp_logfile == (qhandle_t)-1)
        return;                     // open failed earlier, stay quiet
    if (!xerp_logfile) {
        xerp_logfile = FS_EasyOpenFile(path, sizeof(path),
                                       FS_MODE_APPEND | FS_BUF_LINE | FS_FLAG_TEXT,
                                       "logs/", "xerp", ".log");
        if (!xerp_logfile) {
            xerp_logfile = (qhandle_t)-1;
            return;
        }
        Com_Printf("Logging xerp telemetry to %s\n", path);
    }

    va_start(ap, fmt);
    Q_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Com_FormatLocalTime(stamp, sizeof(stamp), "%H:%M:%S");
    len = Q_snprintf(line, sizeof(line), "[%s] %s", stamp, buf);
    if (len < sizeof(line))
        FS_Write(line, len, xerp_logfile);
}

// cl_xerp_fire 2: log every fire decision to logs/xerp.log
#define XF_VERBOSE  (cl_xerp_fire->integer >= 2)

#define XF_LOG(fmt, ...) \
    CL_XerpLog("xerpfire %u: " fmt, cls.realtime, ##__VA_ARGS__)

typedef struct {
    const char  *wwep;      // world weapon (vwep) model substring, from the
                            // player entity's skinnum — present for every
                            // player regardless of hand/cl_gun settings
    const char  *vwep;      // view weapon model substring, from ps.gunindex —
                            // fallback, absent for center-handed players
    const char  *name;      // for cl_xerp_fire 2 logging
    int         mz_weapon;  // MZ_* code the server echoes for this weapon
    unsigned    refire;     // ms between fire cycles, measured from echoes
    bool        automatic;  // keeps firing while attack is held
    int         follow;     // extra shots 100 ms after each cycle start
                            // (akimbo: the second pistol's bang)
} xf_weapon_t;

// refire values are field-measured server echo cadences, not guesses:
// mk23 full-auto cycles at 400 ms, akimbo fires 100 ms pairs every 400 ms,
// mp5/m4 stream at the 100 ms server frame, m3 pump ~900 ms
static const xf_weapon_t xf_weapons[] = {
    { "w_mk23",    "v_blast",  "mk23",   MZ_BLASTER,      400, true,  0 },
    { "w_mp5",     "v_machn",  "mp5",    MZ_MACHINEGUN,   100, true,  0 },
    { "w_m4",      "v_m4",     "m4",     MZ_ROCKET,       100, true,  0 },
    { "w_super90", "v_shotg",  "m3",     MZ_SHOTGUN,      900, false, 0 },
    { "w_cannon",  "v_cannon", "hc",     MZ_SSHOTGUN,    1500, false, 0 },
    { "w_akimbo",  "v_dual",   "akimbo", MZ_BLASTER,      400, true,  1 },
    { "w_sniper",  "v_sniper", "ssg",    MZ_HYPERBLASTER, 1400, false, 0 },
    // The SSG's FIRE is predicted (field-validated, including zoomed shots
    // once the zoom-busy window has passed); zoom-visual prediction was
    // tried, invalidated by field data, and removed.
    // Absent on purpose: knife and grenades keep server-echo behavior.
};

// identify the held weapon: primary source is the own player entity's vwep
// index (skinnum high bits), which is always present; ps.gunindex is only a
// fallback since center-handed players (hand 2) get gunindex 0
static const xf_weapon_t *xf_find_weapon(bool log)
{
    centity_t *self = &cl_entities[cl.frame.clientNum + 1];
    const char *model = NULL;
    int i;

    if (self->serverframe == cl.frame.number &&
        self->current.modelindex2 == MODELINDEX_PLAYER) {
        i = self->current.skinnum >> 8;
        if (cl.csr.extended)
            i &= 0xff;
        if (i >= 0 && i < cl.numWeaponModels)
            model = cl.weaponModels[i];
        for (i = 0; model && i < q_countof(xf_weapons); i++)
            if (strstr(model, xf_weapons[i].wwep))
                return &xf_weapons[i];
    }

    i = cl.frame.ps.gunindex & GUNINDEX_MASK;
    if (i) {
        model = cl.configstrings[cl.csr.models + i];
        for (i = 0; i < q_countof(xf_weapons); i++)
            if (strstr(model, xf_weapons[i].vwep))
                return &xf_weapons[i];
    }

    if (log && XF_VERBOSE)
        XF_LOG("skip: weapon not predicted (%s)\n", model ? model : "unknown");
    return NULL;
}

// TNG puts the health icon in STAT_HELPICON only while bandaging
static bool xf_bandaging(void)
{
    int icon = cl.frame.ps.stats[STAT_HELPICON];

    return icon > 0 && icon < cl.csr.max_images &&
        !strcmp(cl.configstrings[cl.csr.images + icon], "i_health");
}

#define XF_PENDING_MAX  8
#define XF_ECHO_WINDOW  600     // ms a predicted shot waits for its echo

static struct {
    bool        prev_attack;
    unsigned    last_fire;
    unsigned    zoom_busy_until;    // sniper: mirrors the server's WEAPON_BUSY
                                    // window after a zoom change
    int         burst_left;         // shots remaining in a 3RB trigger pull
    unsigned    burst_start;        // when the current burst began
    int         follow_left;        // paired shots left in this fire cycle
    unsigned    raise_until;        // weapon raise after respawn/switch:
                                    // the server can't fire during it
    int         stream_echoes;      // echoes consumed since the last edge,
                                    // for burst-mode detection
    bool        was_normal;         // previous frame's pm_type was NORMAL
    int         last_widx;          // weapon held last frame, -1 = none
    struct {
        unsigned    time;
        int         mz_weapon;
        int         widx;           // xf_weapons index, for cadence learning
    } pending[XF_PENDING_MAX];
    unsigned    head, tail;     // pending ring, head > tail
} xf;

// live cadence calibration: the server's echo stream reveals its true fire
// cycle per weapon, so the table refire values act only as priors. Learning
// is asymmetric — intervals shorter than the estimate pull it down fast
// (they prove the server can fire that fast), longer ones only drift it up
// slowly and only within a sane band around the prior (slow clicking and
// lag spikes must not corrupt the estimate). Persists across map changes;
// the echo watchdog remains the backstop if an estimate ever goes wrong.
static struct {
    float       cycle;          // learned ms between shots, 0 = use prior
    float       pending_short;  // candidate shorter cycle awaiting confirmation
    unsigned    last_echo;
    int         samples;
} xf_cad[q_countof(xf_weapons)];

static int xf_cycle(const xf_weapon_t *w)
{
    int widx = (int)(w - xf_weapons);

    if (xf_cad[widx].cycle)
        return (int)xf_cad[widx].cycle;
    return w->refire;
}

static void xf_learn_cadence(int widx, unsigned now)
{
    const xf_weapon_t *w = &xf_weapons[widx];
    float interval, old;

    if (xf_cad[widx].last_echo) {
        interval = now - xf_cad[widx].last_echo;
        // band-pass around the prior: rejects akimbo's 100 ms pair gaps,
        // slow-click gaps and lag bursts
        if (interval >= w->refire * 0.6f && interval <= w->refire * 1.6f) {
            old = xf_cad[widx].cycle ? xf_cad[widx].cycle : w->refire;
            if (interval < old - 5) {
                // shorter cycles need TWO consecutive agreeing samples: a
                // lagging server that batches snapshots produces fake-short
                // intervals, and a single one must not yank the estimate
                if (xf_cad[widx].pending_short &&
                    fabsf(interval - xf_cad[widx].pending_short) < 25) {
                    xf_cad[widx].cycle = old * 0.5f + interval * 0.5f;
                    xf_cad[widx].pending_short = 0;
                } else {
                    xf_cad[widx].pending_short = interval;
                }
            } else {
                xf_cad[widx].cycle = old * 0.95f + interval * 0.05f;
                xf_cad[widx].pending_short = 0;
            }
            xf_cad[widx].samples++;
            if (XF_VERBOSE && fabsf(xf_cad[widx].cycle - old) >= 5)
                XF_LOG("%s cadence calibrated: %d -> %d ms (n=%d)\n",
                       w->name, (int)old, (int)xf_cad[widx].cycle,
                       xf_cad[widx].samples);
        }
    }
    xf_cad[widx].last_echo = now;
}

// the server's persistent MP5/M4 fire mode (full auto vs 3 round burst),
// toggled by the "weapon" command while holding that gun; deliberately NOT
// reset on level change since the server persists it per connection
static struct {
    bool    mp5_burst, m4_burst;
    bool    mk23_semi;      // server default is 0 = full auto
} xf_mode;

// echo watchdog: if the oldest in-flight prediction is this stale, stop
// auto-stream predictions until the next fresh click — self-heals any
// desync between the mirrored fire mode and the server's actual mode.
// The threshold adapts to measured echo latency so high-ping players
// (echoes at 200+ ms) don't get their streams falsely paused.
#define XF_ECHO_STALE_MIN   350

static float xf_echo_latency;   // EWMA of measured click-to-echo ms

static unsigned xf_echo_stale(void)
{
    unsigned t = (unsigned)(xf_echo_latency * 2.0f) + 150;

    return t > XF_ECHO_STALE_MIN ? t : XF_ECHO_STALE_MIN;
}

// called when a zoom command ("weapon"/"lens") is forwarded while holding
// the sniper: the server enters WEAPON_BUSY for up to 6 frames (600 ms,
// zoom_comp only ever shortens it), so hold off predictions until then
void CL_XerpFireZoomChanged(void)
{
    const xf_weapon_t *w;

    if (!cl_xerp_fire->integer || cls.state != ca_active)
        return;

    w = xf_find_weapon(false);
    if (w && w->mz_weapon == MZ_HYPERBLASTER) {
        xf.zoom_busy_until = cls.realtime + 700;
        if (XF_VERBOSE)
            XF_LOG("zoom change - sniper hold for 700 ms\n");
    }
}

void CL_XerpFireClear(void)
{
    memset(&xf, 0, sizeof(xf));
    xf.last_widx = -1;
}

/*
Predicted spray punch: the M4's recoil climb (kick_angles pitch of -1.5
degrees per full-auto shot, and the bullets follow the climbed angle) is
part of the server player state, so under fire prediction the bangs lead
the visual climb by a round-trip — spray compensation timing feels
desynced. The client applies the climb for exactly the in-flight
predicted shots (predicted but not yet echoed); as each echo arrives the
server's own kick takes over that shot's share, so the handoff is
continuous and never double-counts. No climb in burst mode, matching the
server.
*/
float CL_XerpFireKickPitch(void)
{
    static float    cur;
    static unsigned last;
    unsigned now;
    float target, frac, lead;
    bool spraying;

    if (!cl_xerp_fire->integer || xf_mode.m4_burst) {
        cur = 0;
        return 0;
    }

    // an M4 full-auto spray is active: trigger held with a recent M4
    // prediction. The lead is a CONSTANT while spraying — the average
    // earliness, measured echo latency over the fire cycle, in climb
    // units — because any per-shot dynamics on top of the server's
    // smooth ramp read as shake. Constant + smooth ramp = smooth ramp.
    spraying = xf.prev_attack &&
        xf.last_widx >= 0 &&
        xf_weapons[xf.last_widx].mz_weapon == MZ_ROCKET &&
        xf.last_fire && cls.realtime - xf.last_fire < 300;

    if (spraying) {
        lead = xf_echo_latency / (float)xf_cycle(&xf_weapons[xf.last_widx]);
        if (lead > 3)
            lead = 3;
        // mirror the server's 23-shot climb cap
        if (xf.stream_echoes >= 23)
            lead = 0;
        target = lead * -1.5f;
    } else {
        target = 0;
    }

    now = cls.realtime;
    if (!last || now - last > 400) {
        cur = target;
    } else {
        // the lead must never MOVE faster than classic moves: the server
        // lerps each kick over a full 100 ms frame, so ease in and release
        // at that same pace (a 30 ms ease-in read as a jolt at spray start
        // — field feel report). With the constant-lead design there is no
        // ripple to mask, so one symmetric classic-pace constant suffices.
        float tc = 100;

        frac = (now - last) * (1.0f / tc);
        if (frac > 1)
            frac = 1;
        cur += (target - cur) * frac;
    }
    last = now;
    return cur;
}

// cl_xerp_debug 3: per-render-frame recoil trace while firing (and a short
// tail after), for A/B comparison of spray smoothness with and without
// prediction. Records only the recoil components — server kick pitch and
// the predicted lead — so mouse movement never pollutes the trace.
void CL_XerpViewTrace(float server_kick_pitch, float pred_pitch)
{
    if (SCR_XerpDebugLevel() < 3)
        return;
    if (!xf.prev_attack &&
        (!xf.last_fire || cls.realtime - xf.last_fire > 300))
        return;

    CL_XerpLog("xerpview %u: kick %.3f pred %.3f sum %.3f\n",
               cls.realtime, server_kick_pitch, pred_pitch,
               server_kick_pitch + pred_pitch);
}

// TNG blocks all firing during the round-start countdown, signalled only
// by its centerprints — hold predictions through "LIGHTS.../CAMERA..."
// and release on "ACTION!". The strings have been stable since the 90s;
// if they ever change, the echo-cancel and watchdog still bound the cost.
void CL_XerpFireLCA(const char *s)
{
    if (!strncmp(s, "LIGHTS", 6) || !strncmp(s, "CAMERA", 6)) {
        xf.raise_until = cls.realtime + 1600;
        if (XF_VERBOSE)
            XF_LOG("round countdown - holding fire\n");
    } else if (!strncmp(s, "ACTION", 6)) {
        xf.raise_until = 0;
        if (XF_VERBOSE)
            XF_LOG("round live - fire released\n");
    }
}

// the "weapon" command while holding the MP5/M4 toggles the server's
// persistent fire mode — mirror it so burst mode isn't over-predicted
void CL_XerpFireModeToggle(void)
{
    const xf_weapon_t *w;

    if (cls.state != ca_active || cls.demo.playback)
        return;

    w = xf_find_weapon(false);
    if (!w)
        return;
    if (w->mz_weapon == MZ_BLASTER && !strcmp(w->name, "mk23")) {
        xf_mode.mk23_semi = !xf_mode.mk23_semi;
        if (XF_VERBOSE)
            XF_LOG("mk23 mode mirrored: %s\n",
                   xf_mode.mk23_semi ? "semi" : "full auto");
    } else if (w->mz_weapon == MZ_MACHINEGUN) {
        xf_mode.mp5_burst = !xf_mode.mp5_burst;
        if (XF_VERBOSE)
            XF_LOG("mp5 mode mirrored: %s\n",
                   xf_mode.mp5_burst ? "3 round burst" : "full auto");
    } else if (w->mz_weapon == MZ_ROCKET) {
        xf_mode.m4_burst = !xf_mode.m4_burst;
        if (XF_VERBOSE)
            XF_LOG("m4 mode mirrored: %s\n",
                   xf_mode.m4_burst ? "3 round burst" : "full auto");
    }
}

// called from CL_FinalizeCmd once per client frame with the sampled state
void CL_XerpFireCheck(bool attack)
{
    const xf_weapon_t *w = NULL;
    player_state_t *ps = &cl.frame.ps;
    unsigned now;
    bool edge;

    edge = attack && !xf.prev_attack;
    xf.prev_attack = attack;
    if (edge)
        xf.stream_echoes = 0;

    if (!cl_xerp_fire->integer)
        return;
    if (cls.state != ca_active || cls.demo.playback)
        return;

    // respawn detection: the server plays the weapon raise animation
    // (~1 s of activate frames) before it can fire — hold predictions,
    // or a held respawn click bangs the instant we spawn
    if (ps->pmove.pm_type != PM_NORMAL) {
        xf.was_normal = false;
        if (edge && XF_VERBOSE)
            XF_LOG("skip: not in normal play (pm_type %d)\n", ps->pmove.pm_type);
        return;                     // dead, spectating, frozen
    }
    if (!xf.was_normal) {
        xf.was_normal = true;
        xf.raise_until = cls.realtime + 1000;
        if (XF_VERBOSE)
            XF_LOG("spawned - holding fire for the weapon raise\n");
    }

    if (!attack)
        return;
    if (xf_bandaging()) {
        if (edge && XF_VERBOSE)
            XF_LOG("skip: bandaging\n");
        return;
    }
    // age out predicted shots whose echo never came, so they stop counting
    // as in-flight ammo
    while (xf.tail != xf.head && cls.realtime -
           xf.pending[xf.tail % XF_PENDING_MAX].time > XF_ECHO_WINDOW)
        xf.tail++;

    // the ammo stat is a round-trip stale, so shots we predicted but whose
    // echoes haven't arrived yet (the pending ring) are subtracted — this
    // stops auto streams from over-predicting past the end of the clip
    if (ps->stats[STAT_AMMO] - (int)(xf.head - xf.tail) <= 0) {
        if (edge && XF_VERBOSE)
            XF_LOG("skip: clip empty (%d in stat, %u in flight)\n",
                   ps->stats[STAT_AMMO], xf.head - xf.tail);
        return;
    }

    w = xf_find_weapon(edge);       // logs its own skip when verbose
    if (!w)
        return;

    now = cls.realtime;

    // switching weapons also plays a raise animation server-side
    if (xf.last_widx != (int)(w - xf_weapons)) {
        xf.last_widx = (int)(w - xf_weapons);
        if (xf.raise_until < now + 900)
            xf.raise_until = now + 900;
    }
    if (now < xf.raise_until) {
        if (edge && XF_VERBOSE)
            XF_LOG("skip: weapon raising, %u ms left\n", xf.raise_until - now);
        return;
    }
    if (w->mz_weapon == MZ_HYPERBLASTER &&
        xf.zoom_busy_until && now < xf.zoom_busy_until) {
        if (edge && XF_VERBOSE)
            XF_LOG("skip: sniper zoom busy for %u more ms\n",
                   xf.zoom_busy_until - now);
        return;
    }

    now = cls.realtime;
    bool follow = false;
    bool automatic = w->automatic;

    // the mk23's own semi/auto toggle (server default: auto)
    if (!strcmp(w->name, "mk23") && xf_mode.mk23_semi)
        automatic = false;

    // echo-based mode detection: the server streaming past 3 echoes in one
    // held pull proves full auto, whatever the mirrored toggle thinks
    if (xf.stream_echoes >= 4) {
        if (w->mz_weapon == MZ_MACHINEGUN && xf_mode.mp5_burst) {
            xf_mode.mp5_burst = false;
            if (XF_VERBOSE)
                XF_LOG("mp5 full auto detected from echo stream\n");
        } else if (w->mz_weapon == MZ_ROCKET && xf_mode.m4_burst) {
            xf_mode.m4_burst = false;
            if (XF_VERBOSE)
                XF_LOG("m4 full auto detected from echo stream\n");
        }
    }

    bool in_burst = (w->mz_weapon == MZ_MACHINEGUN && xf_mode.mp5_burst) ||
                    (w->mz_weapon == MZ_ROCKET && xf_mode.m4_burst);

    // outside burst mode, the learned fire cycle paces streams; burst mode
    // has its own deterministic pacing below and bypasses this gate
    if (!in_burst && xf.last_fire && now - xf.last_fire < xf_cycle(w)) {
        // paired weapons (akimbo): the second bang rides 100 ms after
        // the cycle start, inside the refire window
        if (xf.follow_left > 0 && now - xf.last_fire >= 100 && attack) {
            follow = true;
        } else {
            if (edge && XF_VERBOSE)
                XF_LOG("skip: %s refire, %u ms of %d\n",
                       w->name, now - xf.last_fire, xf_cycle(w));
            return;
        }
    }
    if (!follow && !edge && !automatic)
        return;                     // semi-auto needs a fresh click

    if (in_burst) {
        // the server auto-repeats bursts while the trigger is held (ready
        // state + attack starts a new burst after recovery) — mirror that,
        // or held-through bursts play as late echoes and the rhythm mixes
        if (!edge && xf.burst_left <= 0 && xf.burst_start &&
            now - xf.burst_start >= 550) {
            xf.burst_left = 3;
            xf.burst_start = now;
        }
        if (edge) {
            // the server needs its burst + recovery cycle before the next
            // burst can start — 550 ms measured from field echoes (the
            // server accepted re-clicks at 553+ ms that 700 was skipping)
            if (xf.burst_start && now - xf.burst_start < 550) {
                if (XF_VERBOSE)
                    XF_LOG("skip: burst recovery, %u ms of 550\n",
                           now - xf.burst_start);
                return;
            }
            xf.burst_left = 3;
            xf.burst_start = now;
        }
        if (xf.burst_left <= 0) {
            if (edge && XF_VERBOSE)
                XF_LOG("skip: burst spent, release trigger\n");
            return;
        }
        // deterministic pacing: burst shots ride the server's fixed 100 ms
        // frames from the burst start, immune to cadence-learner noise
        if (now < xf.burst_start + (unsigned)(3 - xf.burst_left) * 100)
            return;
        xf.burst_left--;
    }

    // echo watchdog: predictions are outstanding way past any sane
    // round-trip — the server isn't firing (mode desync, lag spike),
    // so stop streaming until the player clicks again
    if (!edge && xf.tail != xf.head &&
        now - xf.pending[xf.tail % XF_PENDING_MAX].time > xf_echo_stale()) {
        // the server stopped at exactly 3 echoes mid-pull: that IS burst
        // mode — adopt it even if the toggle mirror missed it
        if (xf.stream_echoes == 3) {
            if (w->mz_weapon == MZ_MACHINEGUN && !xf_mode.mp5_burst) {
                xf_mode.mp5_burst = true;
                if (XF_VERBOSE)
                    XF_LOG("mp5 3 round burst detected from echo stream\n");
            } else if (w->mz_weapon == MZ_ROCKET && !xf_mode.m4_burst) {
                xf_mode.m4_burst = true;
                if (XF_VERBOSE)
                    XF_LOG("m4 3 round burst detected from echo stream\n");
            }
        }
        if (XF_VERBOSE)
            XF_LOG("skip: oldest echo %u ms overdue, pausing stream\n",
                   now - xf.pending[xf.tail % XF_PENDING_MAX].time);
        return;
    }

    if (follow) {
        xf.follow_left--;           // cycle timing stays on the first bang
    } else {
        xf.last_fire = now;
        xf.follow_left = w->follow;
    }
    if (XF_VERBOSE)
        XF_LOG("predicted %s%s\n", w->name, edge ? "" : " (auto)");

    // remember the shot so the server echo can be consumed
    if (xf.head - xf.tail == XF_PENDING_MAX)
        xf.tail++;
    xf.pending[xf.head % XF_PENDING_MAX].time = now;
    xf.pending[xf.head % XF_PENDING_MAX].mz_weapon = w->mz_weapon;
    xf.pending[xf.head % XF_PENDING_MAX].widx = (int)(w - xf_weapons);
    xf.head++;

    // synthesize exactly what the server echo would have produced
    mz.entity = cl.frame.clientNum + 1;
    mz.weapon = w->mz_weapon;
    mz.silenced = false;
    CL_MuzzleFlash();
}

// called for each incoming svc_muzzleflash; true = already played locally
bool CL_XerpFireSuppress(void)
{
    unsigned now, i;

    if (!cl_xerp_fire->integer)
        return false;
    if (mz.entity != cl.frame.clientNum + 1)
        return false;               // someone else's flash

    now = cls.realtime;

    // an own-fire echo during a raise hold proves the server can already
    // fire — the hold was a false positive (or the raise ended early), so
    // cancel it instead of suppressing predictions for the full window
    if (xf.raise_until && now < xf.raise_until) {
        xf.raise_until = 0;
        if (XF_VERBOSE)
            XF_LOG("raise hold canceled - server is firing\n");
    }
    while (xf.tail != xf.head &&
           now - xf.pending[xf.tail % XF_PENDING_MAX].time > XF_ECHO_WINDOW) {
        if (XF_VERBOSE)
            XF_LOG("shot never echoed (mz %d) - server rejected it?\n",
                   xf.pending[xf.tail % XF_PENDING_MAX].mz_weapon);
        xf.tail++;                  // drop echoes that never came
    }

    for (i = xf.tail; i != xf.head; i++) {
        // llsound 0 servers collapse all hitscan echoes to MZ_MACHINEGUN
        // (see PlayWeaponSound in the game DLL), so accept that as a match
        // for any pending shot; llsound 1 servers echo per-weapon codes
        if (xf.pending[i % XF_PENDING_MAX].mz_weapon == mz.weapon ||
            mz.weapon == MZ_MACHINEGUN) {
            if (XF_VERBOSE)
                XF_LOG("echo consumed, click-to-echo %u ms (mz %d)\n",
                       now - xf.pending[i % XF_PENDING_MAX].time, mz.weapon);
            // feed the adaptive watchdog threshold
            xf_echo_latency = xf_echo_latency
                ? xf_echo_latency * 0.9f +
                  (now - xf.pending[i % XF_PENDING_MAX].time) * 0.1f
                : (float)(now - xf.pending[i % XF_PENDING_MAX].time);
            // the echo stream reveals the server's true fire cycle and mode
            xf_learn_cadence(xf.pending[i % XF_PENDING_MAX].widx, now);
            xf.stream_echoes++;
            // consume this and anything older
            xf.tail = i + 1;
            return true;
        }
    }
    if (XF_VERBOSE)
        XF_LOG("own fire NOT predicted (mz %d%s) - full round-trip delay\n",
               mz.weapon, mz.silenced ? ", silenced" : "");
    return false;                   // unpredicted (silencer, knife, ...): play it
}

/*
==============================================================================
XERP ENTS — client-side extrapolation of remote players (cl_xerp_ents,
Phase 2 of the cl_xerp_* netcode feel features).

Remote players normally render interpolated between the two most recent
snapshots — up to a full server frame in the past. When enabled, the
interpolation window is shifted one frame forward: render between the
newest snapshot and its velocity projection, same lerp fraction. This is
the same effect server-side xerp (use_xerp) produces, computed locally so
it works on every server. Purely visual; hitboxes are server-side.

While enabled, CL_SendCvarSync reports cl_xerp 0 to the server so
use_xerp servers don't extrapolate on top of us (double xerp).
==============================================================================
*/

cvar_t *cl_xerp_ents;
cvar_t *cl_xerp_ents_minspeed;

// per-entity record of the position we projected for the next snapshot,
// so its error against the real position is measurable when it arrives
static struct {
    vec3_t      pred;
    vec3_t      err;            // last projection's error, decayed into the
                                // render so corrections never snap
    int         frame;          // cl.frame.number the projection was made on
} xe_hist[MAX_EDICTS];

// cl_xerp_debug 2: rolling validation stats, one "xerpents" line per
// window, players and ballistic projectiles graded separately so the
// gravity projection can be validated on its own
typedef struct {
    int         extrapolated;   // entity-frames drawn extrapolated
    int         checks;         // predictions checked against a real snapshot
    float       err_sum, err_max;
} xe_class_stats_t;

static struct {
    unsigned    start;
    xe_class_stats_t players, proj;
    int         slow, jump;     // entity-frames that fell back to interp
} xe_stats;

static void xe_report(void)
{
    if (SCR_XerpDebugLevel() < 2)
        return;
    if (!xe_stats.start) {
        xe_stats.start = cls.realtime;
        return;
    }
    if (cls.realtime - xe_stats.start < 5000)
        return;

    CL_XerpLog("xerpents %u: players %d ext, err avg %.1f max %.1f (%d checks); "
               "fell back %d slow %d jump\n",
               cls.realtime,
               xe_stats.players.extrapolated,
               xe_stats.players.checks ?
                   xe_stats.players.err_sum / xe_stats.players.checks : 0,
               xe_stats.players.err_max, xe_stats.players.checks,
               xe_stats.slow, xe_stats.jump);

    memset(&xe_stats, 0, sizeof(xe_stats));
    xe_stats.start = cls.realtime;
}

bool CL_XerpEntsOrigin(centity_t *cent, entity_state_t *s1, vec3_t org)
{
    vec3_t vel, pred;
    float speed, err;

    if (cl_xerp_ents->value <= 0 || cls.demo.playback)
        return false;
    if (s1->modelindex != MODELINDEX_PLAYER)
        return false;   // players only — grenades/knives stay server-timed
                        // by field decision: their exact position (bounces,
                        // landing spot) matters more than freshness
    if (s1->event == EV_PLAYER_TELEPORT)
        return false;

    xe_report();

    // per-server-frame displacement between the two newest snapshots;
    // a fresh entity has prev == current, giving zero and falling through
    VectorSubtract(cent->current.origin, cent->prev.origin, vel);
    speed = VectorLength(vel) * (1000.0f / CL_FRAMETIME);

    // teleport/respawn-scale jumps: bail before grading so spawn
    // relocations don't pollute the prediction-error statistics
    if (speed > 2000) {
        xe_stats.jump++;
        xe_hist[s1->number].frame = 0;
        return false;
    }

    // grade the previous projection against where the player really went
    // (once per entity per snapshot: the store below ends the comparison),
    // and carry the error so it can be blended out instead of snapping
    if (cl.frame.number != xe_hist[s1->number].frame) {
        if (xe_hist[s1->number].frame &&
            cl.frame.number == xe_hist[s1->number].frame + 1) {
            xe_class_stats_t *cs = &xe_stats.players;

            VectorSubtract(xe_hist[s1->number].pred, cent->current.origin,
                           xe_hist[s1->number].err);
            err = VectorLength(xe_hist[s1->number].err);
            cs->err_sum += err;
            if (err > cs->err_max)
                cs->err_max = err;
            cs->checks++;
            if (err > 48)           // too wrong to smooth: snap
                VectorClear(xe_hist[s1->number].err);
        } else {
            VectorClear(xe_hist[s1->number].err);
        }
    }

    // speed damping is a ramp, not a cliff: full extrapolation at minspeed,
    // fading to none at half of it. A hard cutoff made every abrupt stop
    // (bots especially) pop back from the extrapolated lead in one step —
    // the "floating" artifact. With the ramp, decelerating players shed
    // their lead gradually.
    float minspeed = cl_xerp_ents_minspeed->value;
    float speed_scale = 1.0f;

    if (minspeed > 0 && speed < minspeed) {
        speed_scale = (speed - minspeed * 0.5f) / (minspeed * 0.5f);
        if (speed_scale <= 0) {
            xe_stats.slow++;
            xe_hist[s1->number].frame = 0;
            return false;           // genuinely stationary: don't guess
        }
    }
    VectorAdd(cent->current.origin, vel, pred);

    VectorCopy(pred, xe_hist[s1->number].pred);
    xe_hist[s1->number].frame = cl.frame.number;

    // render the fresh projection plus the old projection's error faded
    // out over the frame — continuous at snapshot boundaries, converged
    // to the new data by the end of the interval
    LerpVector(cent->current.origin, pred, cl.lerpfrac, org);
    VectorMA(org, 1.0f - cl.lerpfrac, xe_hist[s1->number].err, org);

    // fractional strength: the cl_xerp_ents dial (0..1) times the speed
    // ramp blends between stock interpolation and full extrapolation
    float scale = cl_xerp_ents->value * speed_scale;
    if (scale < 1) {
        vec3_t stock;

        LerpVector(cent->prev.origin, cent->current.origin,
                   cl.lerpfrac, stock);
        LerpVector(stock, org, scale > 0 ? scale : 0, org);
    }

    xe_stats.players.extrapolated++;
    return true;
}

void CL_XerpEntsClear(void)
{
    memset(xe_hist, 0, sizeof(xe_hist));
    memset(&xe_stats, 0, sizeof(xe_stats));
}

