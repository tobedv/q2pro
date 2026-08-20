# Kill Assists — Feasibility Investigation

**Question:** can the AQtion/TNG game server be extended to award *assists* on a kill?

**Answer:** yes, and it is a small, low-risk change. The game DLL already has every hook
needed (a single choke point for damage, a single choke point for death, free per-life and
per-match reset points, and an existing time-windowed "you helped" award in CTF to copy the
style from). Nothing about it requires a network protocol change unless you want an assists
*column* on the HUD.

This document records what exists today, the AQ2-specific wrinkle that makes assists more
valuable here than in most shooters, and a concrete implementation plan.

---

## 1. How a kill is attributed today

| Step | Location |
| --- | --- |
| Damage applied, last attacker recorded | `T_Damage()` — `src/action/g_combat.c:445` (sets `client->attacker`, `attacker_mod`, `attacker_loc` at `g_combat.c:884` and `g_combat.c:954`) |
| Death dispatched | `Killed()` — `src/action/g_combat.c:217` → `targ->die` → `player_die` (`src/action/p_client.c:1863`) |
| Message + scoring | `ClientObituary()` — `src/action/p_client.c:1035` |
| Score mutation | `Add_Frag()` `p_client.c:403`, `Subtract_Frag()` `p_client.c:823`, `Add_Death()` `p_client.c:836`, `Add_TeamKill()` `p_client.c:907` |
| Telemetry | `LogKill()` — `src/action/tng_stats.c:980`, emitted through `Write_Stats()` (`tng_stats.c:955`) to the stats API or `action/logs/<logfile_name>.stats` |

The important limitation: **only the most recent attacker is remembered.** `client->attacker`
is a single `edict_t *` (`src/action/g_local.h:2313`). There is no per-attacker damage ledger
anywhere in the client structs. Everything else — `resp.damage_dealt`, `resp.gunstats[]`,
`client->damage_dealt` (hit markers) — is aggregated on the *attacker*, never keyed by victim.

## 2. The AQ2 wrinkle: most damage is delayed bleed damage

For MK23 / MP5 / M4 / Sniper / Knife / grenade-impact, `T_Damage` sets `instant_dam = 0`
(`g_combat.c:538`). Health is **not** reduced at hit time. The damage is converted into a
bleed pool (`client->bleeding += damage * BLEED_TIME`, `g_combat.c:923`) and drained by
`Do_Bleeding()` (`src/action/p_view.c:1185`).

When a bleed tick finishes someone off:

```c
meansOfDeath = ent->client->attacker_mod;
locOfDeath   = ent->client->attacker_loc;
Killed(ent, ent->client->attacker, ent->client->attacker, damage, ent->s.origin);
```
`p_view.c:1213-1217`

So the frag goes to **whoever landed the last hit**, even if that hit was 5 damage and someone
else did the other 95. This is a well-known AQ2 frag-attribution coin flip. It means an assist
system is not cosmetic here — it recovers credit the scoreboard is currently throwing away.

It also means the implementation is straightforward: bleed deaths route through the same
`Killed()` → `ClientObituary()` path as instant deaths, so one crediting call site covers both.

## 3. Where the state can live — and the free reset points

| Struct | Lifetime | Cleared at |
| --- | --- | --- |
| `gclient_s` (`g_local.h:2140`) | one life | `memset(client, 0, sizeof(*client))` in `PutClientInServer()` (`p_client.c:3118`) — everything except `pers`, `resp`, `cl_cvar` |
| `client_respawn_t` (`g_local.h:2012`) | whole match | `ClientBeginDeathmatch()` (`p_client.c:3472`); per-match loop at `a_team.c:1975-1995`; per-round streak reset at `a_team.c:2850-2857` |

That maps perfectly onto what assists need:

* the **per-life damage ledger** goes in `gclient_s` → it self-clears on every respawn, no new
  reset code required;
* the **cumulative assist counters** go in `client_respawn_t` → they survive respawn and are
  already zeroed by the existing match/round reset loops (one line each to add).

There is no savegame cost: `WriteGame`/`ReadGame`/`WriteLevel`/`ReadLevel` are empty stubs
(`src/action/g_save.c:838-849`), so adding struct members is free — no field descriptor tables
to maintain.

**Memory.** `MAX_CLIENTS` is 256 (`inc/shared/shared.h:100`). A full 256-slot ledger per client
would be ~2 KB each / 512 KB total — wasteful. A small ring of the N most recent *distinct*
attackers (N = 4 or 8) costs ~48-96 bytes per client and matches how every other shooter does
it. Recommended.

## 4. There is already precedent for exactly this shape of feature

CTF implements a time-windowed "you helped" award:

* `CTFCheckHurtCarrier()` (`a_ctf.c:683`) stamps `resp.ctf_lasthurtcarrier = level.framenum`
  when you damage the enemy flag carrier — called from inside `T_Damage` (`g_combat.c:849`);
* `CTFFragBonuses()` (`a_ctf.c:563`) reads it back and pays out if
  `level.framenum - ctf_lasthurtcarrier < CTF_CARRIER_DANGER_PROTECT_TIMEOUT * HZ`;
* capture assists at `a_ctf.c:825-855` use `CTF_FRAG_CARRIER_ASSIST_TIMEOUT` /
  `CTF_RETURN_FLAG_ASSIST_TIMEOUT`.

Espionage mode does the same thing with `esp_lasthurtleader` / `esp_leaderprotectcount`
(`g_local.h:2108-2117`).

A stamp-in-`T_Damage`, pay-out-on-death, timeout-gated assist is therefore *native* to this
codebase's idiom — it is a generalisation of code that already ships.

## 5. Delivery surfaces that already exist

| Surface | Hook | Notes |
| --- | --- | --- |
| Console / broadcast text | `gi.cprintf`, `gi.bprintf`, `PrintDeathMessage()` | zero-risk v1 |
| Spectator killfeed | `p_client.c:1059-1080` (`--KF <team> <name>, MOD <n>, ...`) | machine-parsable; an assist field slots in naturally |
| Reward centerprint + sound | `Announce_Reward()` `p_client.c:356`, `Awards` enum `g_local.h:1133` | add an `ASSIST` member; note it currently `CenterPrintAll`s, so an assist reward wants a per-player variant |
| AQtion ghud scoreboard | `resp.hud_items[128]` (`g_local.h:2098`), builders in `p_hud.c:1290-1500` | assists column is feasible for the AQtion client |
| Vanilla `svc_layout` scoreboard | `DeathmatchScoreboardMessage()` `p_hud.c:306` | capped at 1023 bytes (`p_hud.c:389`) and the built-in `client` layout op has fixed fields (score/ping/time) — an assists column here means hand-drawn strings, not extending `client` |
| Unicast event to capable clients | `TE_DAMAGE_DEALT` hit markers, `p_view.c:1698-1707` | already gated on `Client_GetProtocol()`/`Client_GetVersion()`; the same gate could carry an assist event |
| Stats API / log | `LogKill()` `tng_stats.c:980` | see §7 |

## 6. Proposed design

### 6.1 Per-life ledger (in `gclient_s`, `g_local.h`)

```c
#define MAX_ASSIST_TRACK 4      // distinct recent attackers remembered per life

typedef struct {
    edict_t *attacker;          // NULL = empty slot
    int      enterframe;        // attacker's resp.enterframe, guards against slot reuse
    int      damage;            // damage accumulated against us this life
    int      last_framenum;     // when they last hurt us
    int      mod;               // most recent means of death from them
} assist_track_t;

assist_track_t assist_track[MAX_ASSIST_TRACK];
```

Stored on the **victim**. Cleared for free by the `PutClientInServer` memset.

Guarding against client-slot reuse matters: an attacker can disconnect and a new player take
the slot before the victim dies. Pairing the `edict_t *` with `resp.enterframe` and validating
`inuse && client && pers.connected` on read-back is cheap. The codebase already stores raw
`edict_t *` and validates on use (`a_xgame.c:338-350`), so either convention is in keeping.

### 6.2 Recording

In `T_Damage`, at the two points where `client->attacker` is already assigned
(`g_combat.c:884-886` and `g_combat.c:954-956`), add a call:

```c
Assist_RecordDamage(targ, attacker, damage, mod);
```

Guards: `attacker->client && attacker != targ && !friendlyFire && !in_warmup`. Use `damage`
(the pre-armor value) for consistency with the neighbouring `resp.damage_dealt` and hit-marker
accounting, which both use `damage` rather than `take` — necessary anyway, since for bleeding
weapons `take` is not applied to health at hit time.

Slot policy: match existing attacker → accumulate; else take an empty slot; else evict the
entry with the oldest `last_framenum`.

### 6.3 Crediting

New `Assist_Award(edict_t *victim, edict_t *killer, int mod)`, called from `ClientObituary()`.
Cleanest is one call immediately after the frag is booked, covering both crediting sites
(`p_client.c:1187-1207` for the push/fall path, `p_client.c:1565-1583` for the normal path).

For each ledger entry, credit when **all** hold:

* entry is valid and is not the killer and not the victim;
* `entry->damage >= assist_min_damage`;
* `level.framenum - entry->last_framenum <= assist_timeout * HZ`;
* not on the victim's team (equivalently: on the killer's team, in team modes);
* `!in_warmup`, and in round-based modes `team_round_going` — mirroring `Add_Frag`'s gating.

Payout, in ledger order sorted by damage descending, capped at `assist_max` entries:

```c
ent->client->resp.assists++;
if (assist_score->value)
    ent->client->resp.score += (int)assist_score->value;
```

### 6.4 Cvars (register in `InitGame`, `g_save.c` around line 647 with the other gameplay cvars)

| Cvar | Default | Meaning |
| --- | --- | --- |
| `use_assists` | `0` | master switch — ships off, so existing servers are unchanged |
| `assist_timeout` | `10` | seconds since the assister's last hit |
| `assist_min_damage` | `25` | minimum accumulated damage to qualify |
| `assist_score` | `0` | score points per assist; `0` keeps scoring semantics identical |
| `assist_max` | `2` | max assists credited per kill |
| `assist_announce` | `1` | print "X assisted Y's kill of Z" |

Defaulting `use_assists` and `assist_score` to off means the feature is observable (counters,
stats) before it is allowed to move anybody's score — important for a mod with an established
competitive scene and matchmode configs (`a_match.c:1077` already force-sets gameplay cvars for
matches, so match configs get an explicit knob).

### 6.5 Counters (in `client_respawn_t`)

```c
int assists;            // this match
int roundAssists;       // this round
```

Add to the reset loop at `a_team.c:1975-1995` (match) and `a_team.c:2850-2857` (round), next to
`resp.kills` / `resp.roundStreakKills`. Optionally extend `gunStats_t` (`g_local.h:1931`) with
an `assists` member so `stats` output can break assists down per weapon; `ResetStats()`
(`tng_stats.c:80`) already memsets the whole array, so no extra reset work.

## 7. Telemetry

`LogKill()` (`tng_stats.c:980`) writes a single JSON line into a `char msg[1024]`:

```json
{"frag":{"sid":"…","v":"…","k":"…","w":12,"l":2,"ks":3,"ttk":184, …}}
```

Two options:

1. **Extend the `frag` record** with `"a":[{"s":"<steamid>","n":"<name>","d":142,"w":3}]`.
   Cheap for consumers to ignore, but the 1024-byte buffer is already fairly full — the assist
   array must be bounded (`assist_max`) and truncation-safe. Bump `msg` to 2048 while you are
   there.
2. **Emit a separate `{"assist":{…}}` record** per assist, reusing the same match/round/server
   identifiers. Keeps `frag` untouched, easier to version, more lines on the wire.

Recommendation: option 2 for the API, since the stats consumer is a separate service and a new
record type is a strictly additive schema change, whereas widening `frag` risks tripping strict
parsers.

Note `game.ai_ent_found` (`tng_stats.c:900`) disables stat logging entirely when bots are
present — assists should still function in-game under that condition, only logging is skipped.

## 8. Edge cases worth deciding up front

* **Bleed-out kills** — handled for free; `Do_Bleeding` routes through `Killed()`.
* **Healing.** Medkits restore health (`ent->health += medkit_value`, `p_view.c:1279`).
  Strictly, damage that got healed away should not earn an assist. The simplest defensible rule
  is to ignore healing and rely on `assist_timeout` to expire stale contributions; if you want
  it exact, subtract the healed amount from ledger entries oldest-first at `p_view.c:1279`.
* **Friendly fire / teamkills.** Do not credit assists on the FF path
  (`p_client.c:1571-1576`), and never credit a victim's own teammate.
* **World deaths** (`MOD_FALLING`, `MOD_TRIGGER_HURT`, lava…). `push_timeout`
  (`g_local.h:2317`) already gives an attacker credit for shoving someone off a ledge; whether a
  third party who softened the victim earns an assist is a design call. Recommend: v1 credits
  assists only when there is a real `attacker->client` killer, and revisit behind a cvar.
* **Telefrag.** `Add_Frag` is skipped for `MOD_TELEFRAG` in teamplay (`p_client.c:1578`);
  assists should follow the same rule.
* **Suicide.** Killer == victim: no frag, so no assists. Arguably a nearby damager "caused" it;
  out of scope for v1.
* **Warmup.** Gate on `in_warmup` exactly like `Add_Frag`/`Add_Death` do.
* **Bots.** `is_bot` clients have a `client` struct like anyone else — assists work unchanged;
  only the hit-marker style unicast paths need the existing `!ent->is_bot` guard.

## 9. Effort and risk

Roughly 150-250 lines, touching:

* `src/action/g_local.h` — ledger struct, `resp` counters, prototypes
* `src/action/g_combat.c` — two `Assist_RecordDamage()` calls in `T_Damage`
* `src/action/p_client.c` — `Assist_Award()` and its call sites in `ClientObituary()`
* `src/action/g_save.c` + `src/action/g_main.c` — cvar registration and externs
* `src/action/a_team.c` — two reset loops
* `src/action/tng_stats.c` — optional telemetry record
* `src/action/p_hud.c` — optional ghud assists column

Risk is low and contained: the whole feature is inert with `use_assists 0`, it adds no
allocation, no protocol change, and no savegame serialisation. The two genuinely external
pieces are the stats-API schema (a separate service) and any HUD column for the vanilla
scoreboard, which is constrained by the 1023-byte layout string and the fixed `client` layout
op.
