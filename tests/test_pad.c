/* test_pad.c: offline model of the real-pad logic (native/pad_logic.h, 6n, pad audit R2-1 / R2-2). */
#include <stdio.h>
#include <string.h>
#include "../native/pad_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int act(pad_act_t *a, unsigned short b, int lt, int rt, int lx, int ly, int rx, int ry) { return pad_act_update(a, b, (unsigned char)lt, (unsigned char)rt, (short)lx, (short)ly, (short)rx, (short)ry); }

int main(void)
{
    /* 1. activity is an event, not a level (R2-2) */
    {
        pad_act_t a;
        memset(&a, 0, sizeof a);
        CHECK(!act(&a, 0, 0, 0, 0, 0, 0, 0));                    /* the first sample is a baseline */
        CHECK(act(&a, 0x1000, 0, 0, 0, 0, 0, 0));               /* A pressed */
        CHECK(!act(&a, 0x1000, 0, 0, 0, 0, 0, 0));              /* held: no more events */
        CHECK(!act(&a, 0, 0, 0, 0, 0, 0, 0));                   /* released: no event */
        CHECK(act(&a, 0x1000, 0, 0, 0, 0, 0, 0));               /* pressed again */
        CHECK(act(&a, 0x1000 | 0x2000, 0, 0, 0, 0, 0, 0));      /* a second button while the first is held */
        CHECK(!act(&a, 0x2000, 0, 0, 0, 0, 0, 0));
        /* triggers: crossing the threshold only */
        CHECK(!act(&a, 0, 0, 0, 0, 0, 0, 0));
        CHECK(act(&a, 0, 0, 200, 0, 0, 0, 0));
        CHECK(!act(&a, 0, 0, 220, 0, 0, 0, 0));                 /* held and changing a little */
        CHECK(!act(&a, 0, 0, 0, 0, 0, 0, 0));
        CHECK(act(&a, 0, 100, 0, 0, 0, 0, 0));
        /* sticks: leaving the dead zone is an event, a drifting stick is not a stream of events */
        pad_act_t d;
        memset(&d, 0, sizeof d);
        CHECK(!act(&d, 0, 0, 0, 12000, 0, 0, 0));               /* plugged in with a drifting stick: baseline, no human */
        for (int i = 0; i < 50; i++) CHECK(!act(&d, 0, 0, 0, 12000 + (i % 3) * 700, -(i % 2) * 500, 0, 0));   /* noise of the drift: no event, ever */
        CHECK(act(&d, 0, 0, 0, 30000, 0, 0, 0));                /* the human pushes the stick */
        CHECK(!act(&d, 0, 0, 0, 30500, 0, 0, 0));
        CHECK(act(&d, 0, 0, 0, -20000, 0, 0, 0));               /* through the centre to the other side */
        CHECK(!act(&d, 0, 0, 0, 0, 0, 0, 0));                   /* back to rest */
        CHECK(act(&d, 0, 0, 0, 0, 25000, 0, 0));                /* leaving the dead zone on the other axis */
        pad_act_t s;
        memset(&s, 0, sizeof s);
        CHECK(!act(&s, 0x0001, 0, 0, 0, 0, 0, 0));              /* plugged in with a stuck button: baseline */
        for (int i = 0; i < 20; i++) CHECK(!act(&s, 0x0001, 0, 0, 0, 0, 0, 0));
        CHECK(act(&s, 0x0001 | 0x1000, 0, 0, 0, 0, 0, 0));      /* a real press on top of it still counts */
        pad_act_t r;
        memset(&r, 0, sizeof r);
        act(&r, 0, 0, 0, 0, 0, 0, 0);
        CHECK(!act(&r, 0, 0, 0, 8000, -8500, 8999, 0));         /* inside the dead zone */
        CHECK(act(&r, 0, 0, 0, 9100, 0, 0, 0));                 /* just beyond */
    }
    /* 2. the pick (R2-1) */
    {
        pad_pick_t p;
        pad_pick_init(&p);
        int c1[4] = {1, 0, 0, 0}, none[4] = {0, 0, 0, 0}, c2[4] = {1, 1, 0, 0}, e1[4] = {0, 1, 0, 0};
        CHECK(pad_pick_update(&p, 1000, none, none) == -1);
        CHECK(pad_pick_update(&p, 1200, c1, none) == 0);        /* one pad: it is the pad */
        CHECK(pad_pick_update(&p, 1400, c2, none) == 0);        /* a second one plugged in, nobody active: the choice stays */
        CHECK(pad_pick_update(&p, 2000, c2, e1) == 1);          /* pad 1 is used, pad 0 never was: the used one wins */
    }
    {
        pad_pick_t p;
        pad_pick_init(&p);
        int c2[4] = {1, 1, 0, 0}, none[4] = {0, 0, 0, 0}, e0[4] = {1, 0, 0, 0}, e1[4] = {0, 1, 0, 0};
        /* a duplicate pad 0 (a remapper's virtual one, idle) and the real pad 1 */
        CHECK(pad_pick_update(&p, 1000, c2, none) == 0);        /* before any activity: the lowest index */
        CHECK(pad_pick_update(&p, 1200, c2, e1) == 1);          /* the pad that is used wins over the idle one that never was */
        CHECK(pad_pick_update(&p, 1400, c2, e1) == 1);
        CHECK(pad_pick_update(&p, 2000, c2, e0) == 1);          /* pad 0 twitches, pad 1 was used 600 ms ago: stays */
        CHECK(pad_pick_update(&p, 3000, c2, none) == 1);
        CHECK(pad_pick_update(&p, 5500, c2, e0) == 0);          /* pad 1 idle for 4 s (since 1400), pad 0 active: the choice moves */
        CHECK(pad_pick_update(&p, 6000, c2, e1) == 0);          /* pad 1 shows up again but pad 0 was used 500 ms ago: stays */
        CHECK(pad_pick_update(&p, 9500, c2, e1) == 1);          /* pad 0 idle for 4 s, pad 1 active: back */
    }
    {
        pad_pick_t p;
        pad_pick_init(&p);
        int c2[4] = {1, 1, 0, 0}, c1b[4] = {0, 1, 0, 0}, c3[4] = {1, 1, 1, 0}, none[4] = {0, 0, 0, 0}, e1[4] = {0, 1, 0, 0}, e2[4] = {0, 0, 1, 0}, e0[4] = {1, 0, 0, 0};
        pad_pick_update(&p, 100, c2, e1);
        CHECK(p.cur == 1);
        CHECK(pad_pick_update(&p, 200, c1b, none) == 1);        /* pad 0 unplugged: no change */
        CHECK(pad_pick_update(&p, 300, none, none) == -1);      /* all gone */
        CHECK(pad_pick_update(&p, 400, c3, none) == 1);         /* back: the pad with the newest activity (1) wins over the lowest index */
        pad_pick_update(&p, 500, c3, e2);
        CHECK(p.cur == 1);                                      /* pad 1 is only 400 ms idle: stays */
        CHECK(pad_pick_update(&p, 4000, c3, e2) == 2);          /* 1 idle, 2 active */
        /* the tick counter wraps (GetTickCount after 49 days) */
        pad_pick_t w;
        pad_pick_init(&w);
        CHECK(pad_pick_update(&w, 0xFFFFFC00u, c2, e1) == 1);
        CHECK(pad_pick_update(&w, 0x00000400u, c2, e0) == 1);   /* pad 1 used 2.0 s ago across the wrap: stays */
        CHECK(pad_pick_update(&w, 0x00001800u, c2, e0) == 0);   /* idle 6 s across the wrap: moves */
    }
    /* R2-6 (flashlive, notes\flashlive.md section 1): the seats after a load. A model of the game (GameMan types / seats, sPlayer_SpawnAll's swap, the player's
     * +0x7920 / +0x7924 copies, the cPad pad number re-read from the seat at a slot-type change) and of the glue (the partner to type 1 at the load begin, the role
     * set-up after it, the running game's tick). The human must read the logical pad its PC's real pad is passed through as (game user = its slot; sBioPad's
     * tables are straight: logical n = physical n = XInput game user n), after any number of loads, in both roles, and the seats stay the stock join's 0 / 1. */
    for (int my = 0; my < 2; my++) {
        struct { int type[2], seat[2], ptype[2], pseat[2], cpad[2]; } g;
        int rem = 1 - my, bad_pad = 0, bad_seat = 0;
#define R26_REFRESH(k) do { int t_ = g.type[k]; if (g.ptype[k] != t_) g.cpad[k] = g.seat[k]; g.ptype[k] = t_, g.pseat[k] = g.seat[k]; } while (0)   /* set_slot_type: SetSlotType + RefreshSlotFromGameMan; a type change re-reads the seat into the cPad (vf19 -> vf44 -> vf51) */
#define R26_FIX() do { int m_ = pad_seat_writes(g.seat); for (int k_ = 0; k_ < 2; k_++) if (m_ & (1 << k_)) g.seat[k_] = k_; if (m_) for (int k_ = 0; k_ < 2; k_++) g.pseat[k_] = g.seat[k_]; \
                         int f_ = pad_cpad_fix(my, g.cpad[my]); if (f_ >= 0) g.cpad[my] = f_; } while (0)
        /* the session start: single player (slot 0 human seat 0, slot 1 AI seat -1), the stock join (the other slot human, seat 1), the glue's types */
        g.type[0] = 0, g.type[1] = 1, g.seat[0] = 0, g.seat[1] = -1;
        for (int k = 0; k < 2; k++) g.ptype[k] = g.type[k], g.pseat[k] = g.seat[k], g.cpad[k] = g.seat[k];
        g.type[1] = 0, g.seat[1] = 1; R26_REFRESH(1);                          /* Coop_ApplyJoinLeave(join) */
        R26_FIX();
        g.type[my] = 0; R26_REFRESH(my);
        g.type[rem] = 2; R26_REFRESH(rem);
        CHECK(g.cpad[my] == my);
        for (int load = 1; load <= 5; load++) {
            g.type[rem] = 1; R26_REFRESH(rem);                                 /* the glue at the load begin: "a type-2 slot would stall the load" */
            if (g.type[0] == 1 && g.type[1] == 0) {                            /* sPlayer_SpawnAll: types and seats swapped */
                int t = g.seat[0];
                g.type[0] = 0, g.type[1] = 1, g.seat[0] = g.seat[1], g.seat[1] = t;
            }
            for (int k = 0; k < 2; k++) g.ptype[k] = g.type[k], g.pseat[k] = g.seat[k], g.cpad[k] = g.type[k] == 0 ? g.seat[k] : -1;   /* new players (sPlayer_SpawnForSlot) */
            R26_FIX();                                                         /* setup_roles_step: padseat_apply("after the load") */
            for (int k = 0; k < 2; k++) { g.type[k] = k == my ? 0 : 2; R26_REFRESH(k); }
            R26_FIX();                                                         /* the running game's tick */
            if (g.cpad[my] != my) bad_pad++;
            if (g.seat[0] != 0 || g.seat[1] != 1 || g.pseat[my] != my) bad_seat++;
        }
        CHECK(bad_pad == 0);    /* 6b40a90: the slot-1 human read pad 0 after loads 1, 3, 5 */
        CHECK(bad_seat == 0);
#undef R26_REFRESH
#undef R26_FIX
    }
    {   /* the pure parts */
        int s01[2] = {0, 1}, s10[2] = {1, 0}, sm[2] = {0, -1};
        CHECK(pad_seat_writes(s01) == 0);
        CHECK(pad_seat_writes(s10) == 3);
        CHECK(pad_seat_writes(sm) == 2);
        CHECK(pad_cpad_fix(1, 1) == -1);
        CHECK(pad_cpad_fix(1, 0) == 1);
        CHECK(pad_cpad_fix(0, 0) == -1);
        CHECK(pad_cpad_fix(-1, 0) == -1);
    }
    printf("test_pad: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
