# The `cl_xerp_*` family — how it works

Client-side netcode feel improvements for the AQtion client. Everything
here is **presentation only**: nothing on the wire changes, no server
change is needed, and hit registration is untouched. The improvements
work identically on every server — old builds, community servers, any
config.

## The one guarantee everything else builds on

**The server remains the only authority on gameplay.** Your commands
reach the server exactly as before; the server fires your shots, rolls
the spread, moves the players, and decides every hit on its own clock,
unchanged. These features only change *when you see and hear* things,
never *what happens*. A prediction can therefore never create or remove
a hit — the worst possible misprediction is a cosmetic flash or a
briefly misplaced player model, and every predictor below has a
data-validated mechanism that bounds exactly that.

## `cl_xerp_fire` — you don't shoot faster, you hear yourself sooner

Without prediction, your own muzzle flash and fire sound are an *echo*:
click → command travels to the server → the server's next 100 ms tick
fires the gun → the `svc_muzzleflash` event travels back. Field
measurements put that feedback delay at ~75 ms median on EU
connections and 90–190 ms on transatlantic ones — the gun *feels*
laggy even though the bullet timing is fine.

With `cl_xerp_fire`, the client mirrors just enough weapon state to
know "this click will fire": weapon identity (from the player entity's
vwep index, which works for `hand 2` players where `ps.gunindex` is
hidden), ammo with in-flight shots subtracted, semi/auto/burst mode,
and per-weapon fire cycles. When the attack input is sampled and the
mirror says yes, the flash and sound play immediately through the same
client path the server echo would use (so `llsound` sound packs work
unchanged). The echo is recognized when it arrives and silently
consumed so nothing plays twice.

Only *fire* effects are predicted. Blood, hit sounds, damage, kill
messages — everything about the *target* — stays server-confirmed,
because the client cannot know the server's spread roll or the enemy's
health, and a fabricated hit would break trust in the feedback.

The mirror keeps itself honest against the server:

- **Cadence self-calibration** — the interval between consumed echoes
  *is* the server's true fire cycle; a per-weapon estimator learns it
  during play (asymmetric: shorter intervals prove the server can fire
  that fast and pull the estimate down quickly; longer ones drift it up
  slowly, band-limited so lag and slow clicking can't corrupt it).
  Calibration paces the *rhythm* of streams only — the first shot of a
  trigger pull is always instant.
- **Mode detection** — the `weapon` command toggles (MK23 semi/auto,
  MP5/M4 full-auto/3-round-burst) are mirrored, and additionally
  detected from the echoes themselves: a stream that stops at exactly 3
  echoes mid-pull is burst mode; one that runs past 3 is full auto.
  This self-heals desyncs (e.g. reconnects reset server modes).
- **Raise holds** — after respawn (~1 s) and weapon switches (~900 ms)
  predictions pause, mirroring the server's weapon raise animation; an
  own-fire echo arriving inside a hold proves it wrong and cancels it.
- **Echo watchdog** — if the oldest in-flight prediction goes 350 ms
  without its echo, the stream pauses until the next fresh click. This
  is the universal backstop: any desync at all costs a few phantom
  bangs at most, then silence until re-click.

## `cl_xerp_ents` — enemies drawn where they are, not where they were

Quake 2 clients render other players *interpolated between the two most
recent server snapshots* — always in the past, up to a full server
frame (100 ms at 10 Hz). `cl_xerp_ents` shifts that window one frame
forward: players render between the newest snapshot and its velocity
projection, with the same interpolation fraction. The result is the
same visual effect the server-side xerp feature produces, computed
locally.

It matters for aim, not just looks: TNG's lag compensation rewinds
hitboxes by *ping only* — the interpolation delay is not compensated
(on servers with `sv_antilag_interp 0`). The stale view therefore
forces players to lead by a wandering 0–100 ms; the extrapolated view
sits approximately where the rewind actually checks, so aiming at what
you see converts more consistently.

Extrapolation is a guess, so it is guarded and graded:

- **Error smoothing** — every projection is compared against the real
  position when the next snapshot arrives; the error is faded out
  across the following interval instead of snapped (errors over 48
  units — genuinely wrong guesses like knockback — snap instead of
  smearing). Rendering stays continuous at snapshot boundaries.
- **Speed damping** — no extrapolation below
  `cl_xerp_ents_minspeed` (default 120 ups), so strafe-wiggle and
  creeping are not amplified. Teleport-scale jumps (>2000 ups) and
  teleport events snap.
- **Strength dial** — the cvar value blends between stock
  interpolation (0) and the full one-frame shift (1). Prediction errors
  shown on screen scale down with it.
- **Players only** — grenades and knives stay server-timed by field
  decision: their exact position (bounces, landing spots) matters more
  than 100 ms of freshness.

## Relationship to the legacy `cl_xerp` / `use_xerp` (server xerp)

The old server xerp (AQtion v1.3) had the same goal as `cl_xerp_ents`
— draw players a frame ahead — implemented on the server: an extra
player-physics simulation per player, per viewer, per tick, baked into
the snapshots. The comparison:

| | server xerp (`use_xerp` + `cl_xerp`) | `cl_xerp_ents` |
|---|---|---|
| Where computed | on the server, per viewer | locally |
| Server cost | extra Pmove per player per viewer per tick | none |
| Works on | only servers with `use_xerp 1` | every server |
| Strength control | none (on/off) | fractional dial |
| Error handling | none (snaps) | smoothed, graded, telemetered |
| Extrapolation input | player's actual held inputs | velocity from snapshots |

The two must not stack (double extrapolation ≈ 200 ms of lead), and
that is handled automatically with no server change: server xerp is
gated per player on the CvarSynced `cl_xerp` value, so while
`cl_xerp_ents` is above 0 the client reports `cl_xerp 0` — every
existing `use_xerp` server stands down for that player. The archived
`cl_xerp` cvar itself is not modified. Note that server xerp never had
an equivalent of `cl_xerp_fire` — your own trigger feel is new ground.

## What was tried and removed — and why that's in this doc

Two ideas were built, field-tested with telemetry, invalidated by their
own data, and **removed** rather than left as dormant switches:

- **Adaptive interpolation buffer** (`cl_xerp_buffer`): premised on
  reclaiming a "random connect phase" of render delay. Telemetry showed
  the engine's existing time clamps already self-stabilize at the
  minimum delay the two-snapshot window allows (~52 ms measured vs
  ~50 ms theoretical), and any requested margin was low-clamped away at
  every snapshot arrival as rhythmic 10 Hz time-skips. Doing better
  requires a third-snapshot renderer — a possible future *stability*
  option for jittery connections (it can only add delay, never
  freshness).
- **Additive spray-climb lead** (superseded, instructive): the M4's
  recoil climb was first led by *adding* an offset on top of the
  server's rendered kick. Every smoothing shape still deviated from
  classic, for a fundamental reason: a curve you are adding to cannot
  arrive earlier without extra velocity during the shift — read by the
  eye as chop, ripple, or a too-fast onset depending on the disguise.
  A second architecture — regenerating the curve from the server's
  known algorithm (run the climb generator from predicted shots, cancel
  the server's arriving copy) — is sound in principle and dissolves the
  velocity constraint, but field traces showed the naive mirror ran at
  2.2× the server's real climb rate, and its ack-side reconstruction
  tracked at roughly half. Both had mechanical causes, found by reading
  the game source against the traces: the M4's real formula is
  `machinegun_shots * -0.7` (p_weapon.c M4_Fire) — the `-1.5` the
  mirror used belongs to mk23/mp5 dead code a few functions up
  (1.5 / 0.7 = 2.14 ≈ the 2.2× overshoot; 23 × 0.7 = the observed −16
  cap) — and the ack stepper counted *consumed prediction echoes*,
  whose "consume this and anything older" batching swallows shots.
  **Recoil still renders fully server-timed.** The true machine is now
  documented in predict.c (climb mirror section) and its value model is
  verified against the recorded baseline traces to the quarter-degree,
  wire quantization included (the client receives
  `trunc(shots × -2.8) / 4`: 0.5°/0.75° alternating stairs, first step
  −0.50, cap exactly −16.00, one 100 ms think per step, release to 0 in
  one think). What ships in this build is **phase A, passive**: an
  echo-driven reconstruction that steps on raw own-entity M4 flashes,
  renders nothing, and logs `|kick − A|` per render frame
  (`cl_xerp_debug 3`: `xerpview ... res` + `xerpkick` decisions).
  Prediction may only be revived by driving this same machine from
  predicted shots after field residuals hold ~0 across sessions and
  stances. Lessons kept: discrete events can be time-shifted;
  continuous curves must be *regenerated*; regeneration demands the
  true state machine, proven passively before it touches the screen.
- **Predicted sniper zoom** (`cl_xerp_zoom`): telemetry showed zoom-in
  confirm times of ~650–700 ms at 15 ms ping — the delay is TNG's
  deliberate server-side weapon-settle window, not the network, and
  predicting the fov desynced it from the server-driven scope overlay.
  SSG *fire* prediction (including zoomed shots, after the mirrored
  700 ms zoom-busy window) remains — it was validated separately.

## Validation telemetry

With `cl_xerp_debug 2`, everything above writes evidence to
`logs/xerp.log` (its own file — the console stays clean): per-shot
predictions, skip reasons, and click-to-echo latency; cadence
calibration and mode detection events; per-window extrapolation error
(every projection graded against the player's real position on the next
snapshot); render-clock statistics. Every behavior in this document was
either derived from or corrected by this data, and any future
regression shows up in the same place.

## Quick reference

| Cvar | Values | Purpose |
|---|---|---|
| `cl_xerp_fire` | 0 / 1 / 2 | instant own-fire feedback; 2 adds per-shot logging |
| `cl_xerp_ents` | 0.0 – 1.0 | player extrapolation strength dial |
| `cl_xerp_ents_minspeed` | ups, default 120 | speed floor below which players interpolate normally |
| `cl_xerp_debug` | 0 / 1 / 2 / 3 | off / overlay / + `logs/xerp.log` telemetry / + per-frame recoil trace |

See `doc/client.md` for the full per-cvar reference.
