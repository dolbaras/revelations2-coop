/* test_combat3.c: offline checks of native/pupanim_logic.h (notes\combat3.md): which of the partner's action ids the puppet replays, and the replay guard.
 *   1. the mask: the knife (28), throws (14 / 18 aim, 22-27, 31), Moira's flashlight (15 / 16 / 19 / 20, 23), the dodge (5-12), crouching (4, 86-89), reload (32-33),
 *      the heal (40-41), hit reactions (158-216) are replayed; the ids that need a context object, belong to other mirrors or are not the human's are not;
 *   2. the guard: an action the puppet drops at once is switched to once per action of its owner (f0c66af: every pass, 30-60 ChangeAction calls a second);
 *   3. an action the puppet plays and finishes while its owner still is in it (the knife combo restarts id 28) is switched to again;
 *   4. idle / walk / crouched walk keep the plain rule (the puppet's own locomotion flips them);
 *   5. a timeline of the client's human (dodge, crouch, flashlight, knife, throw) with lost samples: every action is shown, the puppet ends in idle;
 *   6. the puppet's own copy of every newly replayed blow is still dropped by the host (P2, grapple_logic.h): one client blow = one damage on the host.
 * Built a second time with -DPA_MODEL_BEFORE_C3 (rev2-int f0c66af: the combat2 mask, the plain rule): that model must fail (tests/build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/grapple_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 40) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned g_rng = 4242;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

static int in_list(uint32_t id, const uint32_t *l, int n)
{
    for (int i = 0; i < n; i++)
        if (l[i] == id) return 1;
    return 0;
}

/* 1. the mask */
static void test_mask(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    static const uint32_t must[] = {
        2, 3, 4, 5, 6, 13, 17, 21, 22,                       /* combat2 and before */
        28,                                                  /* the knife / crowbar blow */
        29, 30,                                              /* melee variants */
        14, 18, 22, 23, 24, 25, 26, 27, 31,                  /* throws: the throwable aim, fire / throw from the aim, quick throw, the hand weapon thrown */
        15, 16, 19, 20, 23,                                  /* Moira's flashlight (special item 0x8020 / 0x8030, state 6 of 0x800) */
        7, 8, 9, 10, 11, 12,                                 /* the dodge (elude) */
        86, 87, 88, 89,                                      /* crouch (squat) */
        32, 33,                                              /* reload */
        40, 41,                                              /* the heal */
        39,                                                  /* lighter */
        43, 47, 54,                                          /* grapple (combat2) */
        90, 99,                                              /* gets_up */
        131, 145, 148, 152, 157,                             /* reactions */
        158, 161, 166, 172, 185, 200, 205, 207, 216,         /* damage reactions */
    };
    static const uint32_t never[] = {
        0, 1,                                                /* wait: the puppet settles by itself */
        34, 35, 36, 37,                                      /* weapon_change: the equipment sync owns the hand */
        38, 42,                                              /* Raid rocket launcher, the inventory UI */
        55, 56, 57, 58, 61, 62, 67, 68, 69, 75, 81, 82, 83, 84, 85,   /* move / ladder actions: the owner's environment target */
        100, 107,                                            /* crab walk */
        108, 113, 118, 119, 121,                             /* die / dying: the game-over coupling */
        122, 123, 124, 125,                                  /* the rescue chain: downed1 */
        126, 127, 128, 129, 130,                             /* world objects / story */
        149, 150, 151,                                       /* the dying reactions: downed1 */
        217, 220, 224, 225, 226, 237, 238, 239, 241,         /* Raid emotes, bottle mine, trials / event / test */
        242, 255, 256, 300, 0x80000000u, 0xFFFFu,            /* outside the table, the scripted INT_MIN */
    };
    for (unsigned i = 0; i < sizeof must / sizeof must[0]; i++) {
        CHECK(pa_anim_allowed(bits, must[i]));
        if (!pa_anim_allowed(bits, must[i])) printf("  id %u (%s) is not replayed\n", must[i], pa_family(must[i]));
    }
    for (unsigned i = 0; i < sizeof never / sizeof never[0]; i++) CHECK(!pa_anim_allowed(bits, never[i]));
    /* every id in the table has a decision: allowed exactly when it is one of these families */
    for (uint32_t id = 0; id < 242; id++) {
        int want = (id >= 2 && id <= 33) || id == 39 || id == 40 || id == 41 || (id >= 43 && id <= 54) || (id >= 86 && id <= 99) || (id >= 131 && id <= 148) || (id >= 152 && id <= 216);
        CHECK(pa_anim_allowed(bits, id) == want);
        CHECK(strcmp(pa_family(id), "?") != 0);
    }
    /* grapple_logic.h's defaults are the same words (coop_game.inc's `net anim 1`) */
    uint32_t g[8];
    gp_puppet_anim_defaults(g);
    CHECK(!memcmp(g, bits, sizeof g));
    (void)in_list;
}

/* a model of the puppet on the host: ChangeAction(id) puts it in id; a refused id is left after `refuse_frames` frames (back to 0, the stock "end" flag); an id it plays
 * runs `len_ms` and then ends in 0 */
typedef struct { uint32_t act, since, changes; } pup_t;
static void pup_change(pup_t *p, uint32_t id, uint32_t now) { p->act = id, p->since = now, p->changes++; }
static void pup_tick(pup_t *p, uint32_t now, uint32_t refused_id, uint32_t refuse_ms, uint32_t len_ms)
{
    if (p->act == refused_id && now - p->since >= refuse_ms) p->act = 0, p->since = now;
    else if (p->act != 0 && p->act != 2 && p->act != 3 && now - p->since >= len_ms) p->act = 0, p->since = now;
}

/* 2. a refused action: switched to once per action of the owner */
static void test_refusal(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    static const uint32_t ids[] = {24, 31, 28, 15, 40, 32, 7, 86, 160};
    for (unsigned k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        uint32_t A = ids[k];
        pa_guard_t g;
        pa_guard_reset(&g);
        pup_t p = {2, 0, 0};
        uint32_t switches = 0, refusal_reports = 0;
        for (uint32_t t = 1000; t < 4000; t += 16) {   /* 60 passes a second; the owner: idle, A for 1 s, idle, A again for 1 s, idle */
            uint32_t own = (t >= 1500 && t < 2500) || (t >= 3000 && t < 3500) ? A : 2;
            pup_tick(&p, t, A, 16, 600);
            int d = pa_decide(&g, bits, own, p.act, t);
            if (d == 2) refusal_reports++;
            if (d == 1) {
                if (own == A) switches++;
                pup_change(&p, own, t);
            }
        }
        CHECK(switches == 2);              /* one per action of the owner (f0c66af: ~60 a second) */
        CHECK(refusal_reports == 2);
        CHECK(g.n_refused == 2);
        CHECK(p.act == 2 || p.act == 0);
    }
}

/* 3. the knife combo: the owner stays in 28 for 1.8 s (three slashes restart the id on its PC); the puppet's slash runs 600 ms and ends in 0 */
static void test_combo(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    pa_guard_t g;
    pa_guard_reset(&g);
    pup_t p = {2, 0, 0};
    uint32_t sw = 0, last = 0, min_gap = 0xFFFFFFFFu;
    for (uint32_t t = 1000; t < 4000; t += 16) {
        uint32_t own = t >= 1200 && t < 3000 ? 28 : 2;
        pup_tick(&p, t, PA_NONE, 0, 600);
        if (pa_decide(&g, bits, own, p.act, t) == 1) {
            if (own == 28) {
                if (sw && t - last < min_gap) min_gap = t - last;
                sw++, last = t;
            }
            pup_change(&p, own, t);
        }
    }
    CHECK(sw >= 3 && sw <= 4);   /* 1.8 s of the owner's combo = three 600 ms slashes on the puppet (a fourth may start in the last frames) */
    CHECK(min_gap >= 600);       /* never restarted while a slash still plays */
    CHECK(g.n_refused == 0);
    CHECK(p.act == 2 || p.act == 0);
}

/* 4. idle / walk / crouched walk: the plain rule, also when the puppet flips them at once */
static void test_loco(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    pa_guard_t g;
    pa_guard_reset(&g);
    uint32_t pup = 2, forced = 0;
    for (uint32_t t = 1000; t < 2000; t += 16) {
        uint32_t own = 3;
        if (pup == 3 && (t / 16) % 2) pup = 2;   /* the puppet's own locomotion goes back to idle every other pass */
        if (pa_decide(&g, bits, own, pup, t) == 1) pup = own, forced++;
    }
    CHECK(forced >= 25);    /* every pass the puppet left 3 (as before) */
    CHECK(g.n_refused == 0 && g.held == PA_NONE);
}

/* 5. the client's human in one fight, sampled at 20 Hz with lost samples (the puppet code runs once per received sample) */
static void test_timeline(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    static const struct { uint32_t id, ms; } seq[] = {
        {2, 500}, {7, 450}, {2, 300}, {86, 400}, {4, 800}, {87, 300}, {2, 300},   /* dodge, crouch, crouched walk, stand up */
        {15, 300}, {19, 900}, {22, 400}, {21, 200}, {2, 300},                     /* the flashlight: aim, hold, flash, lower */
        {28, 1800}, {2, 400},                                                     /* knife combo */
        {14, 300}, {18, 600}, {22, 700}, {21, 200}, {2, 400}, {24, 800}, {2, 500}, /* bottle: aim + throw, quick throw */
        {160, 500}, {2, 500},                                                     /* hit by a replica */
    };
    const int n = (int)(sizeof seq / sizeof seq[0]);
    for (int run = 0; run < 800; run++) {
        int loss = (int)(rnd() % 25);
        pa_guard_t g;
        pa_guard_reset(&g);
        pup_t p = {2, 0, 0};
        int shown[256];
        memset(shown, 0, sizeof shown);
        uint32_t t = 1000;
        for (int i = 0; i < n; i++) {
            for (uint32_t e = 0; e < seq[i].ms; e += 50, t += 50) {
                pup_tick(&p, t, PA_NONE, 0, 650);
                if ((int)(rnd() % 100) < loss) continue;
                if (pa_decide(&g, bits, seq[i].id, p.act, t) == 1) pup_change(&p, seq[i].id, t);
                if (p.act < 256) shown[p.act] = 1;
            }
        }
        for (int i = 0; i < n; i++)
            if (seq[i].ms >= 300) CHECK(shown[seq[i].id]);   /* 6+ samples, at most 24 % lost: one arrives */
        CHECK(p.act == 2 || p.act == 0);
        CHECK(g.n_refused == 0);
    }
}

/* 6. P2 still holds for the new ids: the client's blow counts once on the host (MHIT), the puppet's copy of it (its knife, its thrown bottle / brick, its flash) is dropped */
static void test_p2(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    const uint32_t pup = 0x2AAC11C0u, host = 0x2B000060u, bottle_of_pup = 0x31000000u, bottle_of_host = 0x31100000u;
    for (uint32_t id = 22; id <= 54; id++) {
        if (!pa_anim_allowed(bits, id) || (id > 31 && id < 43)) continue;   /* the blows: weapon_fire 22-31, grapple 43-54 */
        int hits_on_host = 1;                                                 /* the client's real blow: MHIT */
        int copy = 1;                                                         /* the puppet replays id: its own copy lands too */
        uint32_t att = id == 31 || id == 24 || id == 27 ? bottle_of_pup : pup, owner = att == bottle_of_pup ? pup : 0;
        if (copy && gp_puppet_blow_drop(1, att, owner, pup, 0)) copy = 0;    /* the host's guard before the stock update */
        hits_on_host += copy;
        CHECK(hits_on_host == 1);
    }
    CHECK(gp_puppet_blow_drop(1, host, 0, pup, 0) == 0);                     /* the host's own blows stay */
    CHECK(gp_puppet_blow_drop(1, bottle_of_host, host, pup, 0) == 0);
}

/* 7. a new puppet object resets the guard */
static void test_reset(void)
{
    uint32_t bits[8];
    pa_anim_defaults(bits);
    pa_guard_t g;
    pa_guard_reset(&g);
    CHECK(pa_decide(&g, bits, 28, 2, 1000) == 1);
    CHECK(pa_decide(&g, bits, 28, 0, 1010) == 2);   /* left at once: held */
    CHECK(pa_decide(&g, bits, 28, 0, 1030) == 0);
    pa_guard_reset(&g);                             /* the level was loaded again: a new puppet */
    CHECK(pa_decide(&g, bits, 28, 2, 1040) == 1);
}

int main(void)
{
    test_mask();
    test_refusal();
    test_combo();
    test_loco();
    test_timeline();
    test_p2();
    test_reset();
    printf("test_combat3: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
