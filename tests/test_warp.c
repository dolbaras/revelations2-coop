/* test_warp.c: offline checks of the lab stage warp (native/warp_logic.h). A small model of the game's two cGameInfo blocks (the current data and the restart copy
 * [aGame+0x608]) with the per-stage flag table (20 slots of {id, words[16]}), the game's flag getter (FUN_00975B50 + FUN_00A90510) and the restart copy-back
 * (FUN_00973080: the restart copy over the current data). The warp writes into the restart copy only; after the "restart" the current data must hold the target. */
#include <stdio.h>
#include <string.h>
#include "../native/warp_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define FREE_ID 0xFFFFFFFFu   /* the id of a free slot ([0x126087C]; the model works with any value) */
typedef struct {
    unsigned stage, startpos, sceno;
    unsigned tab[WL_SLOTS * (WL_SLOT_BYTES / 4)];
} gi_t;

static void gi_init(gi_t *g, unsigned stage)
{
    memset(g, 0, sizeof *g);
    for (int i = 0; i < WL_SLOTS; i++) g->tab[i * (WL_SLOT_BYTES / 4)] = FREE_ID;
    g->stage = stage;
}
/* the game's getter: FUN_00975B50 finds the slot of the current stage (FUN_00972390 claims a free one), FUN_00A90510 tests bit 0x80000000 >> (n & 31) of word n >> 5 */
static int game_flag(gi_t *g, int n)
{
    int s = wl_slot(g->tab, (int)g->stage, FREE_ID, 1);
    if (s < 0) return -1;
    unsigned w = g->tab[s * (WL_SLOT_BYTES / 4) + 1 + (n >> 5)];
    return (w & (0x80000000u >> ((char)n & 0x1F))) != 0;
}
static void game_set(gi_t *g, int n) { int s = wl_slot(g->tab, (int)g->stage, FREE_ID, 1); g->tab[s * (WL_SLOT_BYTES / 4) + 1 + (n >> 5)] |= 0x80000000u >> (n & 31); }
static void game_restart(gi_t *cur, const gi_t *rc) { *cur = *rc; }   /* FUN_00973080(restart copy -> current): the whole data block, stage / start pos / scene / flags */
static void game_checkpoint(gi_t *rc, const gi_t *cur) { *rc = *cur; } /* FUN_004364C0: current -> restart copy */

/* the warp as warp.inc does it, on the model */
static int do_warp(gi_t *rc, const char *line, int cur_stage, char *err)
{
    wl_plan_t p;
    if (!wl_parse(line, cur_stage, &p, err, 200)) return 0;
    int s = wl_slot(rc->tab, p.stage, FREE_ID, 0);
    if (s < 0) { snprintf(err, 200, "no slot"); return 0; }
    unsigned *e = &rc->tab[s * (WL_SLOT_BYTES / 4)];
    if (e[0] != (unsigned)p.stage) memset(e + 1, 0, WL_WORDS * 4);
    unsigned w[WL_WORDS];
    wl_words(&p, e + 1, w);
    e[0] = (unsigned)p.stage;
    memcpy(e + 1, w, sizeof w);
    rc->stage = (unsigned)p.stage, rc->startpos = (unsigned)p.startpos, rc->sceno = 0;
    return 1;
}

static int same_set(const int *a, int na, const int *b, int nb)
{
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(void)
{
    char err[200];
    wl_plan_t p;

    /* ---- the table: one entry per (stage, start pos), CHP flags unique per stage, every Episode 1 stage present, positions plausible */
    for (int i = 0; i < WL_CHP_N; i++) {
        for (int j = i + 1; j < WL_CHP_N; j++) {
            CHECK(!(g_wl_chp[i].stage == g_wl_chp[j].stage && g_wl_chp[i].startpos == g_wl_chp[j].startpos));
            CHECK(!(g_wl_chp[i].stage == g_wl_chp[j].stage && g_wl_chp[i].flag >= 0 && g_wl_chp[i].flag == g_wl_chp[j].flag));
        }
        CHECK(wl_part_of(g_wl_chp[i].stage) >= 0);
        CHECK(g_wl_chp[i].flag < WL_FLAGS_N);
        CHECK(g_wl_chp[i].y > 0 && g_wl_chp[i].y < 5000);
    }
    /* all-episodes (lab): the table of Episodes 2-4: one entry per (stage, start pos), stage ids of that episode, flags in range, the stage's part known */
    CHECK(wl_bonus_episode(700) == 9 && wl_bonus_episode(701) == 10 && wl_bonus_episode(711) == 11 && wl_bonus_episode(630) == 8 && wl_bonus_episode(201) == 0);
    CHECK(WL_CHP2_N >= 97 && wl_find(711, 0) && wl_find(711, 0)->x == 35724 && wl_part_of(700) == 0 && wl_part_of(630) == 0 && wl_part_of(500) == -1);
    for (int i = 0; i < WL_CHP2_N; i++) {
        for (int j = i + 1; j < WL_CHP2_N; j++) CHECK(!(g_wl_chp2[i].stage == g_wl_chp2[j].stage && g_wl_chp2[i].startpos == g_wl_chp2[j].startpos));
        CHECK(wl_part_of(g_wl_chp2[i].stage) >= 0 && g_wl_chp2[i].flag < WL_FLAGS_N && g_wl_chp2[i].stage >= 200 && g_wl_chp2[i].stage <= 711);
        CHECK(wl_find(g_wl_chp2[i].stage, g_wl_chp2[i].startpos) == &g_wl_chp2[i]);
    }
    CHECK(wl_part_of(201) == 0 && wl_part_of(203) == 0 && wl_part_of(250) == 1 && wl_part_of(254) == 1 && wl_part_of(306) == 0 && wl_part_of(351) == 1 && wl_part_of(455) == 1 && wl_part_of(500) == -1 && wl_part_of(9000) == -1);
    CHECK(wl_find(201, 6) && wl_find(201, 6)->flag == 51 && wl_find(201, 6)->x == 41729);   /* tavern 1F (Pedro infected) */
    CHECK(wl_find(203, 0) && wl_find(203, 0)->flag == 0 && wl_find(250, 0) && wl_find(250, 0)->flag == -1);
    {
        int fl[8];
        CHECK(wl_chp_flags(201, 6, fl, 8) == 1 && fl[0] == 51);   /* only the target's own flag in Episodes 2-4 */
        CHECK(wl_chp_flags(201, 99, fl, 8) == -1);
        CHECK(wl_parse("201 6", 201, &p, err, sizeof err) && p.chp && p.stage == 201);
        CHECK(!wl_parse("250 0", 201, &p, err, sizeof err) && strstr(err, "other half"));
    }
    CHECK(wl_parse("250 0 half", 201, &p, err, sizeof err) && p.cross && p.stage == 250 && p.startpos == 0);   /* the lobby's second-half hop (warp.inc second_half_frame) */
    CHECK(wl_parse("351 1 half", 306, &p, err, sizeof err) && p.cross && p.chp && p.chp->flag == 14);
    CHECK(wl_parse("201 0", 250, &p, err, sizeof err) == 0 && wl_parse("201 0 half", 250, &p, err, sizeof err) && p.cross);
    CHECK(wl_part_start(201, 0) && wl_part_start(250, 0) && wl_part_start(351, 0) && !wl_part_start(201, 1));
    static const int stages[] = {100, 101, 102, 151, 152, 153, 154};
    for (int i = 0; i < 7; i++) CHECK(wl_find(stages[i], 0) != 0);
    CHECK(wl_find(152, 1) && wl_find(152, 1)->flag == 30 && wl_find(152, 1)->x == 24336);   /* s0152.stp entry 1 "back from the forest" */
    CHECK(wl_find(154, 5) && wl_find(154, 5)->flag == 230);                                  /* s0154_chp: start pos 5 <- SF 230 (generator) */
    CHECK(wl_find(151, 2) && wl_find(151, 2)->flag == 60 && wl_find(151, 2)->x == 2842);     /* the north door checkpoint (S6 50.4: the retry came back at (2841.8,2350.1,-13618)) */
    CHECK(wl_find(100, 0) && wl_find(100, 0)->flag == -1);
    CHECK(wl_find(154, 7) == 0 && wl_find(103, 0) == 0);

    /* ---- the CHP flags of a stage up to the target, in walk order */
    int f[32], n;
    n = wl_chp_flags(154, 0, f, 32); { int e[] = {10}; CHECK(same_set(f, n, e, 1)); }
    n = wl_chp_flags(154, 4, f, 32); { int e[] = {10, 41, 110, 165, 200}; CHECK(same_set(f, n, e, 5)); }
    n = wl_chp_flags(151, 3, f, 32); { int e[] = {5, 30, 60, 53, 82}; CHECK(same_set(f, n, e, 5)); }   /* sp5 (cell block west) comes before the power lever */
    n = wl_chp_flags(101, 2, f, 32); { int e[] = {0, 1, 4, 2}; CHECK(same_set(f, n, e, 4)); }
    n = wl_chp_flags(152, 1, f, 32); { int e[] = {0, 30}; CHECK(same_set(f, n, e, 2)); }
    n = wl_chp_flags(100, 0, f, 32); CHECK(n == 0);
    CHECK(wl_chp_flags(152, 2, f, 32) == -1);

    /* ---- the bit order is the game's */
    unsigned w[WL_WORDS];
    memset(w, 0, sizeof w);
    wl_bit_set(w, 0, 1); CHECK(w[0] == 0x80000000u);
    wl_bit_set(w, 31, 1); CHECK(w[0] == 0x80000001u);
    wl_bit_set(w, 32, 1); CHECK(w[1] == 0x80000000u);
    wl_bit_set(w, 511, 1); CHECK(w[15] == 1u);
    wl_bit_set(w, 512, 1); CHECK(w[15] == 1u);                      /* out of range: ignored */
    wl_bit_set(w, 0, 0); CHECK(w[0] == 1u && !wl_bit_get(w, 0) && wl_bit_get(w, 31));

    /* ---- parsing */
    CHECK(wl_parse("154", 153, &p, err, sizeof err) && p.stage == 154 && p.startpos == 0 && p.go == 1 && p.keep == 0);
    CHECK(wl_parse("154 3 set keep +180 +181 -165", 151, &p, err, sizeof err) && p.startpos == 3 && p.go == 0 && p.keep == 1 && p.nplus == 2 && p.plus[1] == 181 && p.nminus == 1 && p.minus[0] == 165);
    CHECK(wl_parse("101 4 go", 100, &p, err, sizeof err) && p.go == 1);
    CHECK(!wl_parse("154 0", 100, &p, err, sizeof err) && strstr(err, "other half"));   /* Claire's half loaded: no warp into Barry's */
    CHECK(!wl_parse("101 0", 152, &p, err, sizeof err) && strstr(err, "other half"));
    CHECK(!wl_parse("154 0", 9000, &p, err, sizeof err) && strstr(err, "not in a story stage"));
    CHECK(!wl_parse("154 9", 153, &p, err, sizeof err) && strstr(err, "no start position"));
    CHECK(!wl_parse("154 0 jump", 153, &p, err, sizeof err) && strstr(err, "unknown word"));
    CHECK(!wl_parse("154 0 +600", 153, &p, err, sizeof err) && strstr(err, "out of"));
    CHECK(!wl_parse("", 153, &p, err, sizeof err) && strstr(err, "usage"));

    /* ---- slots: found, claimed (words cleared), full table */
    gi_t cur, rc;
    gi_init(&cur, 153);
    game_set(&cur, 2), game_set(&cur, 4);                            /* stage 153 played a bit: SF 2, 4 */
    game_checkpoint(&rc, &cur);
    CHECK(wl_slot(rc.tab, 153, FREE_ID, 0) == 0);
    CHECK(wl_slot(rc.tab, 154, FREE_ID, 0) == 1);                    /* the first free one (not claimed yet) */
    CHECK(rc.tab[1 * (WL_SLOT_BYTES / 4)] == FREE_ID);
    gi_t full;
    gi_init(&full, 153);
    for (int i = 0; i < WL_SLOTS; i++) full.tab[i * (WL_SLOT_BYTES / 4)] = 200u + (unsigned)i;
    CHECK(wl_slot(full.tab, 154, FREE_ID, 1) == -1);
    CHECK(!do_warp(&full, "154 0", 153, err));

    /* ---- warp 153 -> 154 start: after the restart the current data is at 154 / 0 with only SF[10] in 154, and 153's flags are untouched */
    CHECK(do_warp(&rc, "154 0", 153, err));
    CHECK(cur.stage == 153);                                         /* the warp never writes the current data */
    game_restart(&cur, &rc);
    CHECK(cur.stage == 154 && cur.startpos == 0 && cur.sceno == 0);
    CHECK(game_flag(&cur, 10) == 1 && game_flag(&cur, 41) == 0 && game_flag(&cur, 2) == 0);
    { unsigned keep_stage = cur.stage; cur.stage = 153; CHECK(game_flag(&cur, 2) == 1 && game_flag(&cur, 4) == 1); cur.stage = keep_stage; }

    /* ---- the same stage again: the default replaces the stage's words (a clean re-test), keep merges */
    game_set(&cur, 19), game_set(&cur, 41);                          /* 154 played: SF 19, 41 */
    game_checkpoint(&rc, &cur);
    CHECK(do_warp(&rc, "154 0", 154, err));
    game_restart(&cur, &rc);
    CHECK(game_flag(&cur, 10) == 1 && game_flag(&cur, 19) == 0 && game_flag(&cur, 41) == 0);
    game_set(&cur, 19), game_checkpoint(&rc, &cur);
    CHECK(do_warp(&rc, "154 4 keep +229 -10", 154, err));
    game_restart(&cur, &rc);
    CHECK(cur.startpos == 4 && game_flag(&cur, 19) == 1 && game_flag(&cur, 229) == 1 && game_flag(&cur, 10) == 0 && game_flag(&cur, 200) == 1 && game_flag(&cur, 165) == 1);

    /* ---- back to a part start (153 / 0): the opening's flags of 153 are kept (run w1 13:10: replacing them left no player), `clean` replaces them */
    gi_init(&cur, 153);
    game_set(&cur, 2), game_set(&cur, 3), game_set(&cur, 4), game_set(&cur, 13), game_set(&cur, 23);
    game_checkpoint(&rc, &cur);
    CHECK(do_warp(&rc, "154 0", 153, err));
    game_restart(&cur, &rc);
    game_checkpoint(&rc, &cur);
    CHECK(do_warp(&rc, "153 0", 154, err));
    game_restart(&cur, &rc);
    CHECK(cur.stage == 153 && game_flag(&cur, 2) == 1 && game_flag(&cur, 3) == 1 && game_flag(&cur, 4) == 1 && game_flag(&cur, 13) == 1 && game_flag(&cur, 23) == 1);
    game_checkpoint(&rc, &cur);
    CHECK(do_warp(&rc, "153 0 clean", 153, err));
    game_restart(&cur, &rc);
    CHECK(game_flag(&cur, 2) == 1 && game_flag(&cur, 3) == 0 && game_flag(&cur, 23) == 0);
    CHECK(wl_part_start(153, 0) && wl_part_start(100, 0) && !wl_part_start(151, 0) && !wl_part_start(153, 1));
    CHECK(wl_parse("100 0 clean", 101, &p, err, sizeof err) && p.clean == 1 && p.keep == 0);

    /* ---- a stage-entry warp of the other part start: 100 start pos 0 sets no flag (with `clean`; the default keeps the part start's flags) */
    gi_init(&cur, 101);
    game_set(&cur, 1), game_checkpoint(&rc, &cur);
    CHECK(do_warp(&rc, "100 0 clean", 101, err));
    game_restart(&cur, &rc);
    CHECK(cur.stage == 100 && cur.startpos == 0);
    for (int i = 0; i < WL_FLAGS_N; i++) if (game_flag(&cur, i)) { CHECK(!"no flag of 100 after a warp to its part start"); break; }

    /* ---- the retry coupling: the receiver's decision */
    const char *why = 0;
    CHECK(wl_retry_decide(1, WL_RC_PAUSE, 3, 700, &why) == WL_RC_RESTART && why && strstr(why, "retries too"));
    CHECK(wl_retry_decide(1, WL_RC_WARP, 3, 700, &why) == WL_RC_APPLY_WARP);
    CHECK(wl_retry_decide(1, WL_RC_PAUSE, 11, 0, &why) == WL_RC_IGNORE && strstr(why, "death screen"));
    CHECK(wl_retry_decide(1, WL_RC_PAUSE, 3, 0, &why) == WL_RC_IGNORE);
    CHECK(wl_retry_decide(1, WL_RC_PAUSE, 12, 700, &why) == WL_RC_IGNORE);   /* already restarting (state 0xC) */
    CHECK(wl_retry_decide(0, WL_RC_PAUSE, 3, 700, &why) == WL_RC_IGNORE);
    CHECK(wl_retry_decide(1, 7, 3, 700, &why) == WL_RC_IGNORE);

    /* ---- two PCs, the pause Retry of PC A (stock: FUN_004362E0 on A only). Both walked on from checkpoint 151/2 into 152; A retries: A reloads 151/2;
     *      B (in play, alive) must reload ITS restart copy too, else B stays in 152 while A is back in 151 (the pair splits) */
    {
        gi_t a_cur, a_rc, b_cur, b_rc;
        gi_init(&a_cur, 151); a_cur.startpos = 2; game_set(&a_cur, 60); game_checkpoint(&a_rc, &a_cur);
        b_cur = a_cur; b_rc = a_rc;
        a_cur.stage = 152, b_cur.stage = 152;
        game_restart(&a_cur, &a_rc);                                   /* A: the Retry row */
        if (wl_retry_decide(1, WL_RC_PAUSE, 3, 700, &why) == WL_RC_RESTART) game_restart(&b_cur, &b_rc);   /* B gets RETRY */
        CHECK(a_cur.stage == 151 && b_cur.stage == 151 && a_cur.startpos == b_cur.startpos);
        /* the lab warp on A rides on the same event: B applies the same line to its own restart copy */
        gi_t a2 = a_cur, a2rc, b2 = b_cur, b2rc;
        game_checkpoint(&a2rc, &a2), game_checkpoint(&b2rc, &b2);
        CHECK(do_warp(&a2rc, "154 3", 151, err));
        game_restart(&a2, &a2rc);
        if (wl_retry_decide(1, WL_RC_WARP, 3, 700, &why) == WL_RC_APPLY_WARP) { CHECK(do_warp(&b2rc, "154 3", 151, err)); game_restart(&b2, &b2rc); }
        CHECK(a2.stage == 154 && b2.stage == 154 && a2.startpos == 3 && b2.startpos == 3 && game_flag(&b2, 165) == 1);
    }

    printf("test_warp: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
