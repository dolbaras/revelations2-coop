/* test_grapple.c: offline checks of native/grapple_logic.h (0.2.0: the client's physical attack on a staggered enemy replica; the replica's alerted bit
 * +0x37B8 bit 30 follows the host's enemy record). Built a second time with -DGP_MODEL_BEFORE_FIX (the record's +0x37B8 was sent but never applied): that model
 * must fail (tests/build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/grapple_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 40) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned g_rng = 4242;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }
static uint32_t rnd32(void) { return (uint32_t)(rnd() << 20) ^ (uint32_t)(rnd() << 8) ^ (uint32_t)rnd(); }

/* 1. the mirror touches bit 30 only and copies it exactly */
static void test_mirror_bits(void)
{
    for (int i = 0; i < 200000; i++) {
        uint32_t r = rnd32(), h = rnd32(), m = gp_alert_mirror(r, h);
        CHECK((m & ~GP_ALERT_BIT) == (r & ~GP_ALERT_BIT));
        CHECK((m & GP_ALERT_BIT) == (h & GP_ALERT_BIT));
        CHECK(gp_alert_mirror(m, h) == m);   /* idempotent: a repeated record changes nothing */
    }
    /* the live values of run M1 (2026-11-07): host 48000000, replica 08000000 */
    CHECK(gp_alert_mirror(0x08000000u, 0x48000000u) == 0x48000000u);
    CHECK(gp_alert_mirror(0x48000000u, 0x08000000u) == 0x08000000u);
    CHECK(gp_alert_mirror(0x08004001u, 0x40000000u) == 0x48004001u);   /* the replica's own low bits (+0x4000 grab, +1) stay */
}

/* 2. the stock class check (uEm1100_vf269) as decompiled */
static void test_class_check(void)
{
    gp_enemy_t e = {0x48000000u, GP_ACT_STAGGER_A, 1u << 12, 1, 6, 0, 0, 1};
    CHECK(gp_class_check(&e) == 1);              /* alerted + stagger window */
    e.motion_flags = 1u << 13;
    CHECK(gp_class_check(&e) == 1);
    e.motion_flags = 0;
    CHECK(gp_class_check(&e) == 0);              /* alerted, outside the window */
    e.motion_flags = 1u << 12, e.intel_500 = 9;
    CHECK(gp_class_check(&e) == 0);              /* Intel +0x500 = 9 forbids */
    e.intel_500 = 6, e.has_intel = 0;
    CHECK(gp_class_check(&e) == 0);              /* no Intel */
    e.has_intel = 1, e.ps = 0x08000000u, e.player_in_front = 1;
    CHECK(gp_class_check(&e) == 0);              /* NOT alerted: the stagger ids 0x97 / 0x98 are inside 0x77..0xEA */
    e.action = GP_ACT_STAGGER_B;
    CHECK(gp_class_check(&e) == 0);
    for (unsigned a = 0; a < 0x400; a++) {
        e.action = a, e.ps = 0x08000000u, e.player_in_front = 1, e.motion_flags = 0;
        int want = !((a >= 0x77 && a <= 0xEA) || a == 0x62 || a == 0x63);
        CHECK(gp_class_check(&e) == want);
        e.player_in_front = 0;
        CHECK(gp_class_check(&e) == 0);
        e.ps = 0x48000000u, e.motion_flags = GP_MOTION_WINDOW, e.player_in_front = 0;
        CHECK(gp_class_check(&e) == 1);          /* alerted: the action id does not matter, the window does */
    }
}

/* 3. the pair over time: the host's enemy (truth) notices a human, chases, is staggered by a head shot, recovers; the client's replica gets 15 Hz records
 * (with loss) and plays the host's action locally (its own motion: the window flags follow with a lag of up to 2 frames). Each frame where the host's enemy
 * offers the stagger attack to a human next to it, the replica must offer it too once a record of that state arrived; the replica must never offer the
 * surprise attack while the host's enemy is alerted. */
typedef struct { uint32_t ps, action, flags; uint8_t notice; } host_frame_t;

static host_frame_t host_at(int f, int notice_at, int stagger_at, int stagger_len, int window_from, int window_to)
{
    host_frame_t h;
    h.ps = 0x08000000u | (f >= notice_at ? GP_ALERT_BIT : 0u);
    h.action = f < notice_at ? 190u : 52u;                         /* idle (190), then chase / attack */
    h.flags = 0;
    h.notice = (f == notice_at || f == stagger_at) ? 1 : 0;   /* the host's notice byte: set by the hit / contact, cleared by its Intel the next frame */
    if (f >= stagger_at && f < stagger_at + stagger_len) {
        h.action = GP_ACT_STAGGER_A;                              /* one stagger of kind 0x97 */
        int k = f - stagger_at;
        if (k >= window_from && k < window_to) h.flags = 1u << 12;
    }
    return h;
}

static void test_pair_timeline(void)
{
    int stagger_offers_host = 0, stagger_offers_both = 0, stagger_missed = 0, surprise_wrong = 0;
    for (int round = 0; round < 400; round++) {
        int loss = (int)(rnd() % 31);                 /* 0..30 % of the records lost */
        int notice_at = 30 + (int)(rnd() % 200), stagger_at = notice_at + 60 + (int)(rnd() % 300);
        int stagger_len = 90 + (int)(rnd() % 120), window_from = 6 + (int)(rnd() % 10), window_to = window_from + 40 + (int)(rnd() % 40);
        uint32_t rep_ps = 0x08000000u, rep_act = 190u, rec_alert = 0;
        uint8_t rep_notice = 0;
        int last_rec = -1000, last_local = -1000, rec_notice = 0;
        for (int f = 0; f < stagger_at + stagger_len + 120; f++) {
            host_frame_t h = host_at(f, notice_at, stagger_at, stagger_len, window_from, window_to);
            if (f % 4 == 0 && (int)(rnd() % 100) >= loss) {   /* a record every 4 frames (15 Hz at 60 fps) */
                rep_ps = gp_alert_mirror(rep_ps, h.ps);
                rep_notice = gp_notice_mirror(rep_notice, gp_rec_flags(h.notice));
                rec_notice = h.notice;
                rep_act = h.action;                            /* ChangeAction on the replica */
                rec_alert = h.ps & GP_ALERT_BIT;               /* what the record said */
                last_rec = f;
            }
            if ((int)(rnd() % 400) == 0) rep_notice = 1, last_local = f;   /* the replica's own update: a body contact of a human (FUN_00605CF0), never cleared without an Intel */
            host_frame_t hl = host_at(f - 2, notice_at, stagger_at, stagger_len, window_from, window_to);   /* the replica's own motion, 2 frames behind */
            gp_enemy_t he = {h.ps, h.action, h.flags, 1, h.action == GP_ACT_STAGGER_A ? 6u : 3u, 1, h.notice, 0};
            gp_enemy_t re = {rep_ps, rep_act, rep_act == GP_ACT_STAGGER_A ? hl.flags : 0u, 1, rep_act == GP_ACT_STAGGER_A ? 6u : 3u, 1, rep_notice, 0};
            int host_offer = gp_stagger_offer(&he), rep_offer = gp_stagger_offer(&re);
            /* the replica can only know what a record told it: count the frames where its last record already showed the stagger and the alerted host */
            int informed = last_rec >= stagger_at && last_rec > last_local && !rec_notice && rep_act == GP_ACT_STAGGER_A && rec_alert;   /* (a local contact after the last record: the next one clears it) */
            if (host_offer) stagger_offers_host++;
            if (host_offer && informed && (hl.flags & GP_MOTION_WINDOW)) {
                if (rep_offer) stagger_offers_both++;
                else stagger_missed++;
            }
            if (rec_alert && gp_surprise_offer(&re)) surprise_wrong++;   /* the last record said alerted */
        }
    }
    printf("pair timeline: host offers %d frames, replica offers with it %d, missed %d, surprise attack offered on an alerted enemy %d\n",
           stagger_offers_host, stagger_offers_both, stagger_missed, surprise_wrong);
    CHECK(stagger_offers_host > 1000);
    CHECK(stagger_offers_both > 1000);
    CHECK(stagger_missed == 0);
    CHECK(surprise_wrong == 0);
}

/* 4. the live pair run m1 (2026-11-08 04:33): the downed (151,0,2), alerted on both, layer-0 flags 0x2000 on both, Intel+0x500 5 on both, Area_IsEpisodeArea(-1) 0
 * on both; host +0x6F8B 0 -> class 1, client +0x6F8B 1 (its own update set it) -> class 0. After the record mirror the client's check is the host's. */
static void test_live_m1(void)
{
    gp_enemy_t host = {0x48000000u, 100, 0x2000, 1, 5, 0, 0, 0};
    gp_enemy_t cli = {0x48000000u, 100, 0x2000, 1, 5, 0, 1, 0};
    CHECK(gp_class_check(&host) == 1);
    CHECK(gp_class_check(&cli) == 0);   /* the input of the run (true for both models) */
    cli.notice = gp_notice_mirror(cli.notice, gp_rec_flags(host.notice));
    CHECK(cli.notice == 0);
    CHECK(gp_class_check(&cli) == gp_class_check(&host));
    /* the record byte: only bit 0, the host's byte maps to 0/1 */
    for (unsigned v = 0; v < 256; v++) {
        CHECK(gp_rec_flags((uint8_t)v) == (v ? GP_REC_NOTICE : 0u));
        CHECK(gp_notice_mirror(1, gp_rec_flags((uint8_t)v)) == (v ? 1 : 0));
        CHECK(gp_notice_mirror(0, gp_rec_flags((uint8_t)v)) == (v ? 1 : 0));
    }
    /* bVar7: with the Episode check true the notice byte does not matter (the stock path of other areas) */
    gp_enemy_t ep = {0x48000000u, 100, 0x2000, 1, 5, 0, 1, 1};
    CHECK(gp_class_check(&ep) == 1);
}

int main(void)
{
    test_mirror_bits();
    test_class_check();
    test_pair_timeline();
    test_live_m1();
#if defined(GP_MODEL_BEFORE_NOTICE)
    printf("test_grapple (model before the notice mirror): %d checks, %d failed\n", g_checks, g_fail);
#elif defined(GP_MODEL_BEFORE_FIX)
    printf("test_grapple (model before the fix): %d checks, %d failed\n", g_checks, g_fail);
#else
    printf("test_grapple: %d checks, %d failed\n", g_checks, g_fail);
#endif
    return g_fail ? 1 : 0;
}
