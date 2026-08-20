# Kill Assists

**Question:** can the AQtion/TNG game server be extended to award *assists* on a kill?

**Answer:** yes — and it now does. Sections 1-5 record the investigation that led here;
sections 6-9 document what shipped, and are the part to read if you just want to know how the
feature works or how to turn it on.

The game DLL already had every hook
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

## 6. What shipped

Implemented in `src/action/a_assist.c` (new module, following the `a_ctf.c` / `a_dom.c`
convention for self-contained features), wired into the existing damage and death paths.

### 6.1 Per-life ledger

`assist_track_t` in `g_local.h`, an 8-entry array on `gclient_s`:

```c
typedef struct assist_track_s
{
	edict_t	*attacker;		// who hurt us, NULL for an empty slot
	int		enterframe;		// attacker's resp.enterframe, guards against client slot reuse
	int		damage;			// effective damage they have dealt us this life
	int		last_framenum;	// level.framenum of their most recent hit
	int		mod;			// their most recent means of death
} assist_track_t;
```

Held on the **victim**, and wiped for free by the `PutClientInServer()` memset on every
respawn, so it is always scoped to a single life. Eight slots covers the 8v8 upper bound; a
5v5 round can fill at most five, so the eviction path is unreachable in normal play.

Pairing the `edict_t *` with the attacker's `resp.enterframe` is what stops a disconnecting
player's contribution from being credited to whoever next takes that client slot.

### 6.2 Recording

`Assist_RecordDamage(targ, attacker, damage, mod)` is called from `T_Damage` at both points
where `client->attacker` is latched (`g_combat.c`). It reads `damage` after the location and
armor multipliers have been applied, which is the same value the surrounding `resp.damage_dealt`
and hit-marker accounting uses - and the only correct choice, since for bleeding weapons
`take` is not applied to health at hit time.

Teammates never accumulate credit against each other, and self-damage is ignored.

### 6.3 Crediting

`Assist_Award(victim, killer)` is called from `ClientObituary()` immediately after `Add_Frag`,
at both crediting sites - so warmup and round-state gating is inherited from the caller rather
than duplicated. Bleed-out deaths need no special handling: `Do_Bleeding()` routes through
`Killed()` into the same obituary path.

A ledger entry is credited when it is not the killer or the victim, the attacker is still
connected in the same client slot, `damage >= assist_min_damage`, the last hit was within
`assist_timeout`, and they are not on the victim's team. Survivors are sorted by damage
descending and the top `assist_max` are awarded, so a cap of 2 rewards whoever actually did
the work.

### 6.4 Cvars

| Cvar | Default | Meaning |
| --- | --- | --- |
| `use_assists` | `0` | master switch - ships off, so existing servers are unchanged |
| `assist_timeout` | `10` | seconds since the assister's last hit |
| `assist_min_damage` | `25` | minimum accumulated damage to qualify (~a quarter of a player's health) |
| `assist_score` | `0` | score points per assist; `0` keeps scoring semantics identical |
| `assist_max` | `2` | max assists credited per kill |
| `assist_announce` | `1` | console line to the assister and the killer |

Registered in `InitGame` (`g_save.c`) alongside the other gameplay cvars, and documented in
`doc/action.md` and `action/doc/tngcvar.txt`.

Defaulting both `use_assists` and `assist_score` to off means the feature is observable -
counters, scoreboard column, telemetry - before it is allowed to move anybody's score. That
matters for a mod with an established competitive scene; a server can collect the stat for a
few weeks and then decide whether it should count.

### 6.5 Display

An `Ast` column next to `Frg` on both teamplay scoreboards that carry per-player frags:
`A_NewScoreboardMessage()` and the `showExtra` rows of `A_ScoreboardMessage()`. The column
only appears when `use_assists` is set, so servers with the feature off see a byte-for-byte
identical scoreboard.

Note for server admins: the default matchmode scoreboard lists **names only** - no per-player
frag column at all - so `use_newscore` must be `1` or higher for the `Frg`/`Ast` columns to
exist in the first place.

**Why not the live top-right frag counter?** It cannot be done without a protocol change. That
number is `STAT_FRAGS`, an index into `player_state_old_t.stats[]`, and that array is full:
`MAX_STATS_OLD` is 32 and the enum in `inc/shared/shared.h` already defines exactly 32 entries
(0-31, with `STAT_TEAM1_HEADER`/`STAT_TEAM2_HEADER` aliasing indices 30 and 31). Adding a
sibling number would mean burning a stat slot or moving the game to `player_state_new_t`
(`MAX_STATS_NEW` 64), which the codebase deliberately does not use. The available route to the
live counter is `assist_score` - with it set, assists fold into `resp.score` and therefore into
the top-right number and every other place score is shown.

### 6.6 Layout string budget

`MAX_SCOREBOARD_SIZE` is 1024 and the assist column widens every row by 4 characters, which is
enough to push a full 8v8 board over the limit. `A_NewScoreboardMessage()` previously had no
byte budgeting at all - it relied on the numbers happening to fit - so a guard was added that
drops a row rather than letting `Q_strncatz` cut one in half, and reports the shortfall through
the existing "..and N more" line.

The guard measures the actual row it is about to append, so with `use_assists 0` the output is
identical to before for every team size, and with assists on a full 8v8 (16 rows, 1014 bytes)
still fits. Above 8 players per team the board was already truncated by `MAX_PLAYERS_PER_TEAM`.

`A_ScoreboardMessage()`'s `showExtra` path already budgets properly via `rowChars`, so it just
needed widened metrics (`TEAM_ROW_CHARS2_AST`, `TEAM_ROW_WIDTH2_AST`).

### 6.7 Telemetry

`LogAssist()` in `tng_stats.c` emits a separate `{"assist":{...}}` record rather than widening
the `frag` line - the stats consumer is a separate service, and a new record type is a purely
additive schema change where growing `frag` (already close to its `char msg[1024]` buffer)
risks tripping strict parsers.

```json
{"assist":{"sid":"…","mid":"…","a":"…","an":"…","ad":"…","at":2,
           "v":"…","vn":"…","vt":1,"k":"…","kn":"…","kt":2,
           "w":2,"d":62,"gm":0,"gmf":0,"t":…,"gt":…,"m":"…","r":3}}
```

`a`/`an`/`ad`/`at` are the assister, `v`/`vn`/`vt` the victim, `k`/`kn`/`kt` the killer, `w` the
means of death the assister last used, `d` the damage they contributed. Consumer-side support
is external to this repo; until it is added the records land in `action/logs/<logfile_name>.stats`
like everything else.

## 7. Edge cases and how they are handled

* **Bleed-out kills** - handled for free; `Do_Bleeding` routes through `Killed()`.
* **Friendly fire / teamkills** - `Assist_Award` is not called on the FF branch, and a victim's
  own teammate can never accumulate credit.
* **World deaths, suicide, telefrag** - assists require a real `attacker->client` killer that is
  not the victim, and the call sits inside the existing `MOD_TELEFRAG` guard.
* **Warmup and round state** - inherited from the `Add_Frag` call sites.
* **Disconnect and client slot reuse** - the `enterframe` stamp plus an
  `inuse && client && pers.connected` check.
* **Healing.** Medkits restore health (`p_view.c`), so some tracked damage can be undone. The
  shipped behaviour ignores this and relies on `assist_timeout` to expire stale contributions.
  Making it exact would mean subtracting healed amounts from ledger entries oldest-first at the
  medkit site; that is a deliberate simplification, not an oversight.
* **Deathmatch** - `OnSameTeam` returns false without teams, so assists work in plain DM too.
* **Bots** - unaffected in-game; only stat logging is skipped when `game.ai_ent_found`.

## 8. Tests

`a_assist.c` is a standalone module, so it can be compiled directly against the real headers
with stubs for the handful of game globals it touches. The harness covers 23 assertions:

* the core case - the player who did 90 damage gets the assist, the finisher does not
* `assist_min_damage` boundary (24 vs 25)
* `assist_timeout` boundary (exactly on the window vs one frame past)
* teammates of the victim never earn assists
* `assist_max` cap, and that it keeps the biggest contributors
* repeated small hits accumulating into one ledger slot
* a recycled client slot not inheriting the previous player's credit
* disconnected assisters skipped
* eviction of the stalest contributor past `MAX_ASSIST_TRACK`
* `use_assists 0` and warmup both inert
* self-damage, world deaths and suicide awarding nothing
* `assist_score` only touching score when configured
* assists working in plain deathmatch

The scoreboard byte budget was verified separately by reproducing the layout-string
construction and confirming (a) byte-for-byte parity with the previous output for every team
size when `use_assists` is 0, and (b) that no configuration exceeds 1023 bytes with it on.

## 9. Files touched

| File | Change |
| --- | --- |
| `src/action/a_assist.c` | new - `Assist_RecordDamage()`, `Assist_Award()` |
| `src/action/g_local.h` | `assist_track_t`, `resp.assists`, cvar externs, prototypes, `LOG_ASSIST` |
| `src/action/g_combat.c` | two `Assist_RecordDamage()` calls in `T_Damage` |
| `src/action/p_client.c` | two `Assist_Award()` calls in `ClientObituary()` |
| `src/action/g_main.c`, `g_save.c` | cvar definitions and registration |
| `src/action/a_team.c` | `Ast` scoreboard column, match reset, layout budget guard |
| `src/action/tng_stats.c` | `LogAssist()` |
| `meson.build`, `src/action/Makefile` | build the new module |
| `doc/action.md`, `action/doc/tngcvar.txt` | admin-facing docs |
