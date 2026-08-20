//-----------------------------------------------------------------------------
// assist_test.c
//
// Unit tests for the kill assist ledger in src/action/a_assist.c.
//
// a_assist.c is self-contained enough to compile directly against the real
// game headers, so these tests exercise the shipping code rather than a copy
// of it. Everything the module reaches for (cvars, level state, OnSameTeam,
// gi.cprintf, the stat logger) is stubbed below.
//
// Build and run from the repository root, against any configured build dir:
//
//   gcc -std=c11 -D_GNU_SOURCE -DHAVE_CONFIG_H -fms-extensions -fsigned-char \
//       -I inc -I src/action -I <builddir> \
//       -o /tmp/assist_test src/action/tests/assist_test.c src/action/a_assist.c
//   /tmp/assist_test
//
// Exits non-zero if any check fails.
//-----------------------------------------------------------------------------
#include "g_local.h"
#include <stdio.h>

// ---- stubs for what a_assist.c references -----------------------------------
game_import_t   gi;
game_locals_t   game;
level_locals_t  level;
team_t          teams[TEAM_TOP];
edict_t        *g_edicts;
int             in_warmup;

cvar_t *use_assists, *assist_timeout, *assist_min_damage;
cvar_t *assist_score, *assist_max, *assist_announce;
cvar_t *teamplay, *teamdm, *stat_logs, *server_id;

static cvar_t cv_use_assists, cv_timeout, cv_min_damage, cv_score, cv_max, cv_announce;
static cvar_t cv_teamplay, cv_teamdm, cv_stat_logs, cv_server_id;

static int printed;
static void stub_cprintf(edict_t *ent, int level_, const char *fmt, ...) { (void)ent; (void)level_; (void)fmt; printed++; }
void UpdateTeamScore(int team, int score) { (void)team; teams[team].score = score; }
void LogAssist(edict_t *e, edict_t *v, edict_t *k, int d, int m) { (void)e;(void)v;(void)k;(void)d;(void)m; }

qboolean OnSameTeam(edict_t *a, edict_t *b)
{
    if (!a->client || !b->client) return false;
    if (teamplay->value) return a->client->resp.team == b->client->resp.team;
    return false;
}

// ---- test scaffolding -------------------------------------------------------
#define MAXENTS 16
static edict_t   ents[MAXENTS];
static gclient_t clients[MAXENTS];

static int failures, checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void check_eq(int got, int want, const char *what)
{
    checks++;
    if (got != want) { failures++; printf("  FAIL: %s (got %d, want %d)\n", what, got, want); }
}

static edict_t *mkplayer(int i, int team)
{
    edict_t *e = &ents[i];
    memset(e, 0, sizeof(*e));
    memset(&clients[i], 0, sizeof(clients[i]));
    e->client = &clients[i];
    e->inuse = true;
    e->client->pers.connected = true;
    e->client->resp.team = team;
    e->client->resp.enterframe = 100 + i;
    snprintf(e->client->pers.netname, sizeof(e->client->pers.netname), "p%d", i);
    return e;
}

static void reset(void)
{
    int i;
    for (i = 0; i < MAXENTS; i++) mkplayer(i, 0);
    for (i = 0; i < TEAM_TOP; i++) teams[i].score = 0;
    level.framenum = 1000;
    in_warmup = 0;
    printed = 0;
}

static void setup_cvars(void)
{
    gi.cprintf = stub_cprintf;
    game.maxclients = MAXENTS;
    game.framerate = 10;

    use_assists = &cv_use_assists;         cv_use_assists.value = 1;
    assist_timeout = &cv_timeout;          cv_timeout.value = 10;
    assist_min_damage = &cv_min_damage;    cv_min_damage.value = 25;
    assist_score = &cv_score;              cv_score.value = 0;
    assist_max = &cv_max;                  cv_max.value = 2;
    assist_announce = &cv_announce;        cv_announce.value = 1;
    teamplay = &cv_teamplay;               cv_teamplay.value = 1;
    teamdm = &cv_teamdm;                   cv_teamdm.value = 0;
    stat_logs = &cv_stat_logs;             cv_stat_logs.value = 0;
    server_id = &cv_server_id;             cv_server_id.string = "test";
}

#define TEST(name) do { printf("%s\n", name); reset(); } while (0)

int main(void)
{
    setup_cvars();

    // --- the core case: bleed damage means the last hit takes the frag ------
    TEST("assist goes to the player who did the damage, not the finisher");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *worker = mkplayer(1, 2);   // did 90 damage
        edict_t *finisher = mkplayer(2, 2); // landed the 10 damage killing blow

        Assist_RecordDamage(victim, worker, 90, MOD_MP5);
        Assist_RecordDamage(victim, finisher, 10, MOD_MK23);
        Assist_Award(victim, finisher);

        check_eq(worker->client->resp.assists, 1, "worker earns the assist");
        check_eq(finisher->client->resp.assists, 0, "finisher earns no assist for their own kill");
    }

    // --- thresholds ---------------------------------------------------------
    TEST("damage below assist_min_damage earns nothing");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *grazer = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        Assist_RecordDamage(victim, grazer, 24, MOD_MP5);   // one under the 25 threshold
        Assist_Award(victim, killer);
        check_eq(grazer->client->resp.assists, 0, "24 damage is below the threshold");

        reset();
        victim = mkplayer(0, 1); grazer = mkplayer(1, 2); killer = mkplayer(2, 2);
        Assist_RecordDamage(victim, grazer, 25, MOD_MP5);   // exactly at it
        Assist_Award(victim, killer);
        check_eq(grazer->client->resp.assists, 1, "25 damage meets the threshold");
    }

    TEST("damage older than assist_timeout is stale");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *early = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        Assist_RecordDamage(victim, early, 90, MOD_MP5);
        level.framenum += 10 * HZ + 1;            // just past the 10s window
        Assist_Award(victim, killer);
        check_eq(early->client->resp.assists, 0, "hit outside the window does not count");

        reset();
        victim = mkplayer(0, 1); early = mkplayer(1, 2); killer = mkplayer(2, 2);
        Assist_RecordDamage(victim, early, 90, MOD_MP5);
        level.framenum += 10 * HZ;                // exactly on the boundary
        Assist_Award(victim, killer);
        check_eq(early->client->resp.assists, 1, "hit on the window boundary counts");
    }

    // --- teams --------------------------------------------------------------
    TEST("teammates of the victim never earn assists");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *mate = mkplayer(1, 1);     // same team as victim
        edict_t *killer = mkplayer(2, 2);

        Assist_RecordDamage(victim, mate, 90, MOD_MP5);   // friendly fire
        Assist_Award(victim, killer);
        check_eq(mate->client->resp.assists, 0, "friendly fire builds no assist credit");
    }

    // --- cap and ordering ---------------------------------------------------
    TEST("assist_max caps awards, biggest contributors win");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *small = mkplayer(1, 2);
        edict_t *big = mkplayer(2, 2);
        edict_t *mid = mkplayer(3, 2);
        edict_t *killer = mkplayer(4, 2);

        Assist_RecordDamage(victim, small, 30, MOD_MP5);
        Assist_RecordDamage(victim, big, 80, MOD_MP5);
        Assist_RecordDamage(victim, mid, 50, MOD_MP5);
        Assist_Award(victim, killer);   // assist_max is 2

        check_eq(big->client->resp.assists, 1, "biggest contributor awarded");
        check_eq(mid->client->resp.assists, 1, "second biggest awarded");
        check_eq(small->client->resp.assists, 0, "smallest dropped by the cap");
    }

    // --- accumulation -------------------------------------------------------
    TEST("repeat hits from one attacker accumulate into one slot");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *shooter = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);
        int i;

        for (i = 0; i < 3; i++)
            Assist_RecordDamage(victim, shooter, 10, MOD_MP5);  // 3x10 = 30 >= 25
        Assist_Award(victim, killer);
        check_eq(shooter->client->resp.assists, 1, "three small hits add up to an assist");
    }

    // --- slot reuse ---------------------------------------------------------
    TEST("a recycled client slot does not inherit the old player's credit");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *attacker = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        Assist_RecordDamage(victim, attacker, 90, MOD_MP5);
        // same edict, new player: reconnect stamps a fresh enterframe
        attacker->client->resp.enterframe = 9999;
        attacker->client->resp.assists = 0;
        Assist_Award(victim, killer);
        check_eq(attacker->client->resp.assists, 0, "stale ledger entry is discarded");
    }

    TEST("a disconnected assister is skipped");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *attacker = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        Assist_RecordDamage(victim, attacker, 90, MOD_MP5);
        attacker->client->pers.connected = false;
        Assist_Award(victim, killer);
        check_eq(attacker->client->resp.assists, 0, "disconnected player earns nothing");
    }

    // --- ledger capacity ----------------------------------------------------
    TEST("more attackers than MAX_ASSIST_TRACK evicts the stalest");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *killer = mkplayer(15, 2);
        edict_t *first;
        int i;

        first = mkplayer(1, 2);
        Assist_RecordDamage(victim, first, 90, MOD_MP5);     // oldest hit
        for (i = 2; i <= MAX_ASSIST_TRACK + 1; i++) {
            level.framenum++;
            Assist_RecordDamage(victim, mkplayer(i, 2), 90, MOD_MP5);
        }
        cv_max.value = MAX_ASSIST_TRACK;   // lift the cap so only eviction is tested
        Assist_Award(victim, killer);
        cv_max.value = 2;

        check_eq(first->client->resp.assists, 0, "stalest contributor was evicted");
        check_eq(ents[MAX_ASSIST_TRACK + 1].client->resp.assists, 1, "newest contributor kept");
    }

    // --- master switch and warmup -------------------------------------------
    TEST("use_assists 0 records and awards nothing");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *helper = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        cv_use_assists.value = 0;
        Assist_RecordDamage(victim, helper, 90, MOD_MP5);
        Assist_Award(victim, killer);
        cv_use_assists.value = 1;
        check_eq(helper->client->resp.assists, 0, "feature is inert when disabled");
    }

    TEST("warmup earns nothing");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *helper = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        in_warmup = 1;
        Assist_RecordDamage(victim, helper, 90, MOD_MP5);
        Assist_Award(victim, killer);
        in_warmup = 0;
        check_eq(helper->client->resp.assists, 0, "no assists during warmup");
    }

    // --- self damage / world kills ------------------------------------------
    TEST("self damage and world deaths award nothing");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *helper = mkplayer(1, 2);

        Assist_RecordDamage(victim, victim, 90, MOD_HG_SPLASH);
        check_eq(victim->client->assist_track[0].attacker == NULL, 1, "self damage is not tracked");

        Assist_RecordDamage(victim, helper, 90, MOD_MP5);
        Assist_Award(victim, NULL);              // died to the world
        check_eq(helper->client->resp.assists, 0, "world death produces no assists");
        Assist_Award(victim, victim);            // suicide
        check_eq(helper->client->resp.assists, 0, "suicide produces no assists");
    }

    // --- scoring ------------------------------------------------------------
    TEST("assist_score adds to score only when configured");
    {
        edict_t *victim = mkplayer(0, 1);
        edict_t *helper = mkplayer(1, 2);
        edict_t *killer = mkplayer(2, 2);

        Assist_RecordDamage(victim, helper, 90, MOD_MP5);
        Assist_Award(victim, killer);
        check_eq(helper->client->resp.score, 0, "default assist_score 0 leaves score untouched");

        reset();
        victim = mkplayer(0, 1); helper = mkplayer(1, 2); killer = mkplayer(2, 2);
        cv_score.value = 1;
        Assist_RecordDamage(victim, helper, 90, MOD_MP5);
        Assist_Award(victim, killer);
        cv_score.value = 0;
        check_eq(helper->client->resp.score, 1, "assist_score 1 grants a point");
    }

    // --- deathmatch (no teams) ----------------------------------------------
    TEST("assists work in plain deathmatch");
    {
        edict_t *victim, *helper, *killer;
        cv_teamplay.value = 0;
        victim = mkplayer(0, 0); helper = mkplayer(1, 0); killer = mkplayer(2, 0);
        Assist_RecordDamage(victim, helper, 90, MOD_MP5);
        Assist_Award(victim, killer);
        cv_teamplay.value = 1;
        check_eq(helper->client->resp.assists, 1, "teamless players still earn assists");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
