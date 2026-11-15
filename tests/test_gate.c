/* test_gate.c: offline model of the stage gate's timeout and a waiting Barry-half jump (native/warp_logic.h wl_gate_timeout_decide, 0.3.2; pair run g032r5n of 2026-11-14).
 * The situation (R1): the host is in Barry's half (stage 250) and repeats its jump every WL_SH_RESEND_MS; this PC (the guest) is still in Claire's first stage (201), its area
 * update waits for its own human since t = 0 (the stage gate, WL_GATE_MS = 25 s) while it cannot take the jump (the lab hold "not in play": the repeats are ignored and remembered).
 * At t = WL_GATE_MS the gate asks the rule: GO_AHEAD = the area switches to 250 under the human who stays at the 201 coordinates (the engine's death screen, game over for both:
 * "stranded"); APPLY_JUMP = the remembered jump is applied (the stock restart: the human moves with the stage, "jumped").
 * The three real runs are in the table below: f031h (0.3.1, jump 0.1 s before the timeout), g032r1 (3 s before), g032r5n (the repeat came after the timeout).
 * Built with -DWL_MODEL_BEFORE_G032 (the change always goes ahead) the suite must fail: tests/build.sh checks both. */
#include <stdio.h>
#include <string.h>
#include "../native/warp_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { int jumped, stranded; unsigned t; } res_t;
typedef struct { unsigned release_ms, phase_ms, last_repeat_ms; int jump_stage, gate_target, agame_state, hp, rc_on; } sit_t;

static sit_t base(unsigned release_ms, unsigned phase_ms)
{
    sit_t s = { release_ms, phase_ms, 0xFFFFFFFFu, 250, 250, 3, 700, 1 };
    return s;
}

/* the guest's side of the situation, step 100 ms; the repeats of the host arrive at phase + k * WL_SH_RESEND_MS (up to last_repeat_ms) */
static res_t run(const sit_t *s)
{
    res_t r = { 0, 0, 0 };
    int have_wait = 0;
    unsigned wait_t = 0;
    for (unsigned t = 0; t <= WL_GATE_MS; t += 100) {
        if (t < s->phase_ms || (t - s->phase_ms) % WL_SH_RESEND_MS != 0 || t > s->last_repeat_ms) continue;
        int held = t < s->release_ms;   /* the lab hold: aGame state 3 reads as 8 */
        const char *why = "";
        int g = wl_sh_guest_decide(201, !held, s->jump_stage, 0xFFFFFFFFu, &why);   /* this PC is in stage 201: not the jump's stage */
        if (g != WL_SH_G_RETRY) continue;
        int act = wl_retry_decide(s->rc_on, WL_RC_WARP, held ? 8 : s->agame_state, s->hp, &why);
        if (act == WL_RC_APPLY_WARP) { r.jumped = 1, r.t = t; return r; }   /* retry_on_event applies it at once */
        if (act == WL_RC_IGNORE) have_wait = 1, wait_t = t;                /* retry_on_event remembers it (0.3.2) */
    }
    const char *why = "";
    int d = wl_gate_timeout_decide(have_wait, have_wait ? WL_GATE_MS - wait_t : 0xFFFFFFFFu, s->jump_stage, s->gate_target, s->agame_state, s->hp, s->rc_on, &why);
    if (d == WL_GT_APPLY_JUMP) r.jumped = 1, r.t = WL_GATE_MS;
    else r.stranded = 1, r.t = WL_GATE_MS;
    return r;
}

/* the jump is applied by the repeat itself when it comes in time: unchanged by 0.3.2 */
static void t_in_time(void)
{
    sit_t a = base(8000, 0);        /* release at gate + 8 s (the lab pair g032r5e): the first repeat after it, at 9 s */
    res_t r = run(&a);
    CHECK(r.jumped && !r.stranded && r.t == 9000);
    sit_t b = base(21000, 0);       /* g032r1: the repeat right after the release, 3 s before the timeout */
    r = run(&b);
    CHECK(r.jumped && !r.stranded && r.t == 21000);
    sit_t c = base(24500, 900);     /* f031h: the repeat 0.1 s before the timeout (24.9 s; the real one was 24.89 s after the gate started) */
    r = run(&c);
    CHECK(r.jumped && !r.stranded && r.t == 24900);
}

/* the race that was lost in g032r5n: the hold is released at 24.2 s, the next repeat would come at 25.1 s, after the timeout */
static void t_race_lost(void)
{
    sit_t a = base(24200, 1100);    /* repeats at 1.1, 4.1, ..., 22.1 (held), 25.1 (too late) */
    res_t r = run(&a);
    CHECK(r.jumped);                /* 0.3.1: stranded, 0.3.2: the waiting jump is applied at the timeout */
    CHECK(!r.stranded);
    CHECK(r.t == WL_GATE_MS);
    sit_t b = base(27000, 0);       /* the lab pair g032r5f: the release 27 s after the gate started, the whole window ignored */
    r = run(&b);
    CHECK(r.jumped && !r.stranded && r.t == WL_GATE_MS);
    sit_t c = base(600000, 700);    /* a hold that never ends within the window (the lab's 10 min): the jump is still applied at the timeout */
    r = run(&c);
    CHECK(r.jumped && !r.stranded);
}

/* nothing waiting / nothing to apply: the old behaviour (the change goes ahead), no infinite wait */
static void t_old_behaviour_stays(void)
{
    sit_t a = base(27000, 0);
    a.last_repeat_ms = 3000;        /* the host stopped repeating long ago (the last repeat 22 s before the timeout): stale */
    res_t r = run(&a);
    CHECK(r.stranded && !r.jumped);
    sit_t b = base(27000, 0);
    b.gate_target = 351;            /* the area change goes to another stage than the jump: not this jump's business */
    r = run(&b);
    CHECK(r.stranded && !r.jumped);
    sit_t c = base(27000, 0);
    c.agame_state = 9;              /* a scene state: no restart now (the retry coupling's own rule) */
    r = run(&c);
    CHECK(r.stranded && !r.jumped);
    sit_t d = base(27000, 0);
    d.agame_state = 11;             /* the death screen is up already */
    r = run(&d);
    CHECK(r.stranded && !r.jumped);
    sit_t e = base(27000, 0);
    e.hp = 0;                       /* my human is dead (the death retry follows) */
    r = run(&e);
    CHECK(r.stranded && !r.jumped);
    sit_t f = base(27000, 0);
    f.rc_on = 0;                    /* the retry coupling is switched off */
    r = run(&f);
    CHECK(r.stranded && !r.jumped);
    const char *why = "";
    CHECK(wl_gate_timeout_decide(0, 0xFFFFFFFFu, 0, 250, 3, 700, 1, &why) == WL_GT_GO_AHEAD);   /* a natural stage change: no jump was ever received */
    CHECK(wl_gate_timeout_decide(1, 100, 0, 250, 3, 700, 1, &why) == WL_GT_GO_AHEAD);           /* no stage in the line */
}

/* the constants and the hold window */
static void t_constants(void)
{
    CHECK(WL_GATE_MS == 25000u);
    CHECK(WL_GT_PENDING_MS >= 2 * WL_SH_RESEND_MS);   /* one lost repeat must not drop the waiting jump */
    CHECK(WL_GT_PENDING_MS < 5 * WL_SH_RESEND_MS);    /* ...and a long-dead host must */
    CHECK(WL_GT_KEEP_MS == 5000u);                    /* no infinite wait: the area is held 5 s after the jump */
    CHECK(!wl_gate_keeping(1000, 0));                 /* not armed */
    CHECK(wl_gate_keeping(1000, 1000 + WL_GT_KEEP_MS));
    CHECK(wl_gate_keeping(1000 + WL_GT_KEEP_MS - 1, 1000 + WL_GT_KEEP_MS));
    CHECK(!wl_gate_keeping(1000 + WL_GT_KEEP_MS, 1000 + WL_GT_KEEP_MS));   /* the keep window ends exactly */
    CHECK(!wl_gate_keeping(1000 + WL_GT_KEEP_MS + 100000, 1000 + WL_GT_KEEP_MS));
    CHECK(wl_gate_keeping(0xFFFFFF00u, 0xFFFFFF00u + WL_GT_KEEP_MS));   /* the tick counter wraps */
}

int main(void)
{
    t_in_time();
    t_race_lost();
    t_old_behaviour_stays();
    t_constants();
#ifdef WL_MODEL_BEFORE_G032
    printf("gate (model of 0.3.1): %d checks, %d failed\n", g_checks, g_fail);
#else
    printf("gate: %d checks, %d failed\n", g_checks, g_fail);
#endif
    return g_fail ? 1 : 0;
}
