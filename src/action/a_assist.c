//-----------------------------------------------------------------------------
// a_assist.c
//
// Kill assists.
//
// AQ2 hands the frag to whoever landed the last hit. Because bullet weapons
// deal their damage through the bleed pool rather than instantly (see
// T_Damage's instant_dam handling in g_combat.c and Do_Bleeding in p_view.c),
// "last hit" is frequently not "did the work" - doing 90 of the 100 damage and
// getting nothing on the scoreboard is routine. This module keeps a small
// per-life ledger of who hurt each player so that credit can be handed back.
//-----------------------------------------------------------------------------

#include "g_local.h"

/*
==================
Assist_RecordDamage

Remember that 'attacker' hurt 'targ' so that a third party who softened the
victim up can still be credited when somebody else lands the killing blow.
Called from T_Damage wherever client->attacker is latched, and reading the
same post-location, post-armor damage value the rest of the stats code uses.

The ledger lives on the victim in gclient_t, which PutClientInServer() wipes
on every respawn, so it is always scoped to a single life.
==================
*/
void Assist_RecordDamage(edict_t * targ, edict_t * attacker, int damage, int mod)
{
	assist_track_t *track = NULL, *oldest = NULL, *slot;
	int i;

	if (!use_assists->value || in_warmup)
		return;
	if (damage <= 0)
		return;
	if (!targ->client || !attacker || !attacker->client || targ == attacker)
		return;
	// teammates never build assist credit against each other
	if (OnSameTeam(targ, attacker))
		return;

	// already contributing to this life?
	for (i = 0; i < MAX_ASSIST_TRACK; i++) {
		slot = &targ->client->assist_track[i];
		if (slot->attacker == attacker && slot->enterframe == attacker->client->resp.enterframe) {
			track = slot;
			break;
		}
	}

	if (!track) {
		// take a free slot, else recycle the contributor who hit us longest ago
		for (i = 0; i < MAX_ASSIST_TRACK; i++) {
			slot = &targ->client->assist_track[i];
			if (!slot->attacker) {
				track = slot;
				break;
			}
			if (!oldest || slot->last_framenum < oldest->last_framenum)
				oldest = slot;
		}
		if (!track)
			track = oldest;

		track->attacker = attacker;
		track->enterframe = attacker->client->resp.enterframe;
		track->damage = 0;
	}

	track->damage += damage;
	track->last_framenum = level.framenum;
	track->mod = mod;
}

/*
==================
Assist_Award

Credit everyone who hurt the victim but did not land the killing blow.

This matters more in AQ2 than in most games: bullet weapons deal their damage
through the bleed pool rather than instantly, so Do_Bleeding() hands the frag
to whoever hit last (see p_view.c). Doing 90 of the 100 damage and getting
nothing on the scoreboard is routine. This is where that credit comes back.

Called from ClientObituary() inside the same guards as Add_Frag, so warmup and
round-state gating is inherited from the caller.
==================
*/
void Assist_Award(edict_t * victim, edict_t * killer)
{
	assist_track_t *sorted[MAX_ASSIST_TRACK], *tmp;
	int count = 0, awarded = 0, i, j;
	int timeout, minDamage, maxAssists, points;

	if (!use_assists->value || in_warmup)
		return;
	if (!victim || !victim->client)
		return;
	// only a genuine player-on-player frag produces assists
	if (!killer || !killer->client || killer == victim)
		return;

	timeout = (int)(assist_timeout->value * HZ);
	minDamage = (int)assist_min_damage->value;
	maxAssists = (int)assist_max->value;
	if (maxAssists <= 0)
		maxAssists = MAX_ASSIST_TRACK;

	for (i = 0; i < MAX_ASSIST_TRACK; i++) {
		assist_track_t *slot = &victim->client->assist_track[i];
		edict_t *ent = slot->attacker;

		if (!ent || ent == killer || ent == victim)
			continue;
		if (!ent->inuse || !ent->client || !ent->client->pers.connected)
			continue;
		// the client slot may have been recycled since they hit us
		if (ent->client->resp.enterframe != slot->enterframe)
			continue;
		if (slot->damage < minDamage)
			continue;
		if (timeout > 0 && level.framenum - slot->last_framenum > timeout)
			continue;
		// never hand the victim's own team credit for killing them
		if (OnSameTeam(victim, ent))
			continue;

		sorted[count++] = slot;
	}

	if (!count)
		return;

	// biggest contribution first, so a cap of 1 or 2 rewards whoever actually
	// did the work rather than whoever happens to sit lower in the array
	for (i = 1; i < count; i++) {
		tmp = sorted[i];
		for (j = i; j > 0 && sorted[j - 1]->damage < tmp->damage; j--)
			sorted[j] = sorted[j - 1];
		sorted[j] = tmp;
	}

	points = (int)assist_score->value;

	for (i = 0; i < count && awarded < maxAssists; i++) {
		edict_t *ent = sorted[i]->attacker;

		ent->client->resp.assists++;
		awarded++;

		if (points) {
			ent->client->resp.score += points;
			if (teamdm->value)
				UpdateTeamScore(ent->client->resp.team, teams[ent->client->resp.team].score + points);
		}

		if (assist_announce->value) {
			gi.cprintf(ent, PRINT_HIGH, "Assist: %d damage on %s before %s finished them\n",
				sorted[i]->damage, victim->client->pers.netname, killer->client->pers.netname);
			gi.cprintf(killer, PRINT_HIGH, "%s assisted your kill on %s\n",
				ent->client->pers.netname, victim->client->pers.netname);
		}

		LOG_ASSIST(ent, victim, killer, sorted[i]->damage, sorted[i]->mod);
	}
}
