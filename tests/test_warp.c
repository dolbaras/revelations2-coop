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

/* ---- 0.3.1 R1: the Barry-half start jump of the host and the partner, in steps of 500 ms. The host hops at t = 0 from 201 to 250 (its own load host_load ms);
 * the partner is in its initial load for guest_init ms (the barrier opened by its timeout), then in play in 201; a jump it takes costs it guest_load ms.
 * The session phase is PLAYING unless one of the two loads a jump (the barrier). end_ms: the session ends there (the host left). */
typedef struct { int jumps, gstage, hstage, acked, sends_after_end, host_active, stop_seen; } sh_res_t;
static sh_res_t sh_run(unsigned guest_init, unsigned host_load, unsigned guest_load, unsigned end_ms, unsigned total_ms)
{
    sh_res_t r;
    memset(&r, 0, sizeof r);
    wl_sh_host_t h;
    r.hstage = 201, r.gstage = 201;
    unsigned h_load_end = host_load, g_init_end = guest_init, g_jump_end = 0, applied_t = 0xFFFFFFFFu;
    int g_jumping = 0, pending = 0, session = 1, have_applied = 0;
    wl_sh_host_arm(&h, 250, 0);
    int jump_now = 1;   /* the hop itself goes out at t = 0 (warp_do) */
    for (unsigned t = 0; t <= total_ms; t += 500) {
        if (t >= end_ms) session = 0;
        int h_in_play = t >= h_load_end;
        if (t >= h_load_end) r.hstage = 250;
        int g_in_play = t >= g_init_end && !g_jumping;
        if (g_jumping && t >= g_jump_end) g_jumping = 0, r.gstage = 250, g_in_play = t >= g_init_end;
        if (jump_now) {   /* the partner receives the jump (reliable, same step) */
            const char *why = 0;
            unsigned ago = have_applied ? t - applied_t : 0xFFFFFFFFu;
            int d = wl_sh_guest_decide(r.gstage, g_in_play, 250, ago, &why);
            if (d == WL_SH_G_ACK) r.acked = 1;
            else if (d == WL_SH_G_RETRY && wl_retry_decide(1, WL_RC_WARP, g_in_play ? 3 : 8, 700, &why) == WL_RC_APPLY_WARP) {
                r.jumps++, applied_t = t, have_applied = 1, pending = 250, g_jumping = 1, g_jump_end = t + guest_load;
                if (r.gstage == 250) r.gstage = 250;   /* a second jump inside part 1 reloads the stage (counted in jumps) */
            }
            jump_now = 0;
        }
        g_in_play = t >= g_init_end && !g_jumping;
        if (wl_sh_guest_answer(pending, r.gstage, g_in_play)) r.acked = 1, pending = 0;
        int playing = session && !g_jumping && t >= h_load_end;
        const char *why = 0;
        int s = wl_sh_host_step(&h, session, playing, r.hstage, h_in_play, r.acked, t, &why);
        if (s == WL_SH_SEND) {
            jump_now = 1;
            if (!session) r.sends_after_end++;
        }
        if (s == WL_SH_STOP) r.stop_seen = 1;
    }
    r.host_active = h.active;
    return r;
}

static void sh_tests(void)
{
    /* guest loading when the hop goes out (allepP27-like: its 201 load runs 40 s past the barrier): 0.3.0 left it in 201 for good */
    sh_res_t a = sh_run(40000, 8000, 15000, 0xFFFFFFFFu, 180000);
    CHECK(a.gstage == 250 && a.jumps == 1 && a.acked && !a.host_active);
    /* guest in play when the hop goes out (the normal path of P2 / P4 / P10): exactly one jump, then the answer stops the repeats */
    sh_res_t b = sh_run(0, 8000, 15000, 0xFFFFFFFFu, 120000);
    CHECK(b.gstage == 250 && b.jumps == 1 && b.acked && !b.host_active);
    /* a slower guest load than the resend period: the repeats during its load do not jump it twice */
    sh_res_t b2 = sh_run(0, 4000, 60000, 0xFFFFFFFFu, 180000);
    CHECK(b2.gstage == 250 && b2.jumps == 1 && b2.acked);
    /* a repeat after the jump: a partner already in a stage of part 1 answers and does not jump again */
    {
        const char *why = 0;
        CHECK(wl_sh_guest_decide(250, 1, 250, 0xFFFFFFFFu, &why) == WL_SH_G_ACK);
        CHECK(wl_sh_guest_decide(351, 1, 351, 100000, &why) == WL_SH_G_ACK);
        CHECK(wl_sh_guest_decide(451, 0, 451, 0xFFFFFFFFu, &why) == WL_SH_G_IGNORE);   /* in the target stage, still loading: neither a jump nor an answer */
        CHECK(wl_sh_guest_decide(201, 1, 250, 2000, &why) == WL_SH_G_IGNORE);          /* the same jump is being loaded */
        CHECK(wl_sh_guest_decide(201, 1, 250, WL_SH_LOADING_MS + 1000, &why) == WL_SH_G_RETRY);   /* that jump failed long ago: decide again */
        CHECK(wl_sh_guest_decide(306, 1, 351, 0xFFFFFFFFu, &why) == WL_SH_G_RETRY);
        /* the lab's hops inside part 1 (live slot F 2026-11-12: 452 -> 455 was refused by the first rule "any part-1 stage = done") */
        CHECK(wl_sh_guest_decide(452, 1, 455, 0xFFFFFFFFu, &why) == WL_SH_G_RETRY);
        CHECK(wl_sh_guest_decide(455, 1, 453, 0xFFFFFFFFu, &why) == WL_SH_G_RETRY);
        CHECK(!wl_sh_guest_answer(250, 201, 1) && !wl_sh_guest_answer(250, 250, 0) && wl_sh_guest_answer(250, 250, 1) && !wl_sh_guest_answer(0, 250, 1) && !wl_sh_guest_answer(455, 452, 1));
    }
    /* the host leaves 5 s after the hop (its own load done at 4 s, the partner still loading): the repeats stop with the session, nothing is sent after it */
    sh_res_t d = sh_run(60000, 4000, 15000, 5000, 120000);
    CHECK(!d.host_active && d.sends_after_end == 0 && d.stop_seen && d.jumps == 0);
    /* the host step itself: a section change and the end of the session stop it, a load only waits */
    {
        wl_sh_host_t h;
        const char *why = 0;
        wl_sh_host_arm(&h, 250, 0);
        CHECK(wl_sh_host_step(&h, 1, 0, 201, 0, 0, 1000, &why) == WL_SH_WAIT && h.active);   /* the host's own load */
        CHECK(wl_sh_host_step(&h, 1, 1, 250, 1, 0, 5000, &why) == WL_SH_SEND);               /* in play at 250, no answer: again */
        CHECK(wl_sh_host_step(&h, 1, 1, 250, 1, 0, 6000, &why) == WL_SH_WAIT);               /* not before the resend period */
        CHECK(wl_sh_host_step(&h, 1, 1, 251, 1, 0, 9000, &why) == WL_SH_STOP && !h.active && strstr(why, "section"));
        wl_sh_host_arm(&h, 351, 0);
        CHECK(wl_sh_host_step(&h, 0, 0, 306, 1, 0, 1000, &why) == WL_SH_STOP && strstr(why, "session"));
        wl_sh_host_arm(&h, 451, 0);
        CHECK(wl_sh_host_step(&h, 1, 1, 451, 1, 1, 4000, &why) == WL_SH_STOP && strstr(why, "Barry"));
        wl_sh_host_arm(&h, 451, 0);
        CHECK(wl_sh_host_step(&h, 1, 1, 451, 1, 0, WL_SH_GIVEUP_MS, &why) == WL_SH_STOP);
        CHECK(wl_sh_host_step(&h, 1, 1, 451, 1, 0, WL_SH_GIVEUP_MS + 5000, &why) == WL_SH_WAIT);   /* stopped stays stopped */
    }
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

    sh_tests();
    printf("test_warp: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
