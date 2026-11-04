/*
 * Regression tests for the retry hang of the pair run s6e2 (stage 6f) and the glue decisions around it (native/glue_logic.h), replayed on the timeline of those runs.
 *
 * THE CAUSE of the hang (proved on hardware in the first 6f run): after a retry the opening scene of Claire's half asks for the enemy (100,116,0) a second after it starts and
 * the story script WAITS for it, asking again every frame (50 Hz); the client refused every creation its own story asked for, so the scene never ended (6640 refusals in one run).
 * test_demand replays that request pattern against glue_demand_check (the 8th request within a second is let through; slow loops, single requests and many layouts are not).
 * The rest covers what was built around it: the scene gate (the partner stays the AI partner for a while after a barrier; my first guess for the cause, it did not cure the hang, so it is opt-in:
 * REV2_GATE=1), the watchdog that must not act during a scene / hold / death, the join guard (a human in the scripted action alone is NOT a scene: the AI partner Moira stands in INT_MIN from
 * the start), the 15 minute cap and the 32 bit tick wrap. The model test shows the old rule failing the timeline so the test can fail.
 */
#include <stdio.h>
#include <string.h>

#include "../native/glue_logic.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the retry timeline: ms after the barrier -> local human state */
static glue_local_t retry_state(unsigned t, unsigned scene_start, unsigned scene_end)
{
    glue_local_t l;
    memset(&l, 0, sizeof l);
    l.hp = 1000, l.act = 2;   /* idle, in control */
    if (t >= scene_start && t < scene_end) l.act = GLUE_SCRIPTED_ACT, l.demo_running = (t < scene_start + 6000);
    return l;
}

/* run the gate over a timeline; returns the time it opened (0 = never within `limit`) */
static unsigned run_gate(unsigned scene_start, unsigned scene_end, unsigned limit, unsigned *opened_during_scene)
{
    glue_gate_t g;
    unsigned base = 4294500000u;   /* the 32 bit wrap of GetTickCount falls inside the 15 minute cap test, on purpose */
    glue_gate_init(&g, base);
    if (opened_during_scene) *opened_during_scene = 0;
    for (unsigned t = 0; t < limit; t += 50) {
        glue_local_t l = retry_state(t, scene_start, scene_end);
        unsigned since, held;
        if (glue_gate_update(&g, base + t, &l, &since, &held)) {
            if (opened_during_scene && t >= scene_start && t < scene_end) *opened_during_scene = 1;
            return t ? t : 1;
        }
    }
    return 0;
}

static void test_retry_scene(void)
{
    printf("-- retry: the scene starts 2.2 s after the barrier and runs 24 s\n");
    unsigned during;
    unsigned t = run_gate(2200, 26200, 120000, &during);
    CHECK(t != 0 && !during);
    CHECK(t >= 26200 + GLUE_GATE_CONTROL_MS - 100 && t <= 26200 + GLUE_GATE_CONTROL_MS + 100);   /* 2.5 s after control returned */

    /* the old glue: the puppet at +0 s, i.e. a gate that is open before the scene starts = the failing behaviour */
    glue_local_t l = retry_state(100, 2200, 26200);
    CHECK(glue_in_control(&l));   /* the human has control in the first seconds: a control-only gate would open here ... */
    CHECK(!glue_gate_open(100, 100));   /* ... the settle time keeps it closed */
    CHECK(!glue_gate_open(GLUE_GATE_SETTLE_MS - 1, 10000));
    CHECK(glue_gate_open(GLUE_GATE_SETTLE_MS, GLUE_GATE_CONTROL_MS));

    printf("-- a retry without a scene (mid-level checkpoint): the gate opens at the settle time\n");
    t = run_gate(1000000, 1000001, 120000, &during);
    CHECK(t >= GLUE_GATE_SETTLE_MS && t <= GLUE_GATE_SETTLE_MS + 100);

    printf("-- a scene that starts at 5 s and ends at 9 s: control must be held 2.5 s after it\n");
    t = run_gate(5000, 9000, 120000, &during);
    CHECK(t >= 9000 + GLUE_GATE_CONTROL_MS - 100 && t <= 9000 + GLUE_GATE_CONTROL_MS + 100 && !during);

    printf("-- two scenes with a short gap: the 2.5 s hold restarts\n");
    {
        glue_gate_t g;
        glue_gate_init(&g, 1000);
        unsigned since, held, now = 1000, opened = 0;
        for (unsigned k = 0; k < 4000 && !opened; k++) {   /* every 10 ms */
            now = 1000 + k * 10;
            unsigned ts = k * 10;
            glue_local_t s;
            memset(&s, 0, sizeof s);
            s.hp = 800, s.act = 2;
            if ((ts >= 3000 && ts < 9000) || (ts >= 10500 && ts < 14000)) s.act = GLUE_SCRIPTED_ACT;   /* a 1.5 s gap between two scenes */
            opened = glue_gate_update(&g, now, &s, &since, &held);
        }
        CHECK(opened && since >= 14000 + GLUE_GATE_CONTROL_MS - 10);
    }
}

static void test_gate_caps_and_states(void)
{
    printf("-- the gate never waits for ever; dead and cutscene-view states\n");
    unsigned during;
    unsigned t = run_gate(0, 0xFFFFFFF0u, GLUE_GATE_MAX_MS + 1000, &during);   /* the human is scripted for ever */
    CHECK(t >= GLUE_GATE_MAX_MS && t <= GLUE_GATE_MAX_MS + 100);

    glue_local_t l;
    memset(&l, 0, sizeof l);
    l.hp = 500, l.act = 2, l.view_mode = 2;
    CHECK(!glue_in_control(&l));   /* the engine's cutscene view */
    l.view_mode = 3;
    CHECK(!glue_in_control(&l));
    l.view_mode = 0, l.demo_running = 1;
    CHECK(!glue_in_control(&l));
    l.demo_running = 0, l.act = GLUE_SCRIPTED_ACT;
    CHECK(!glue_in_control(&l));   /* a hold by an enemy counts as scripted: the gate only applies after a barrier, a hold during play never reaches it */
    l.hp = 0;
    CHECK(glue_in_control(&l));    /* dead: the death screen waits for the player */
    l.hp = -5;
    CHECK(glue_in_control(&l));
}

static void test_watchdog(void)
{
    printf("-- the coop-flag watchdog\n");
    glue_local_t ctl, scripted, dead;
    memset(&ctl, 0, sizeof ctl), memset(&scripted, 0, sizeof scripted), memset(&dead, 0, sizeof dead);
    ctl.hp = 700, ctl.act = 2;
    scripted = ctl, scripted.act = GLUE_SCRIPTED_ACT;
    dead = ctl, dead.hp = 0;
    /* the s6e2 case: the scene is over for the 40 s memory of the scene hook, coop flag off, view mode 0, but the human is still scripted */
    CHECK(!glue_coop_lost_counts(0, 0, 1, 0, 0, &scripted));
    /* the same during the gate */
    CHECK(!glue_coop_lost_counts(0, 0, 1, 0, 1, &ctl));
    /* a scene the engine reports */
    CHECK(!glue_coop_lost_counts(0, 2, 1, 0, 0, &ctl));
    CHECK(!glue_coop_lost_counts(0, 3, 1, 0, 0, &ctl));
    CHECK(!glue_coop_lost_counts(0, 0, 1, 1, 0, &ctl));
    /* the coop flag is on, or no episode is loaded */
    CHECK(!glue_coop_lost_counts(1, 0, 1, 0, 0, &ctl));
    CHECK(!glue_coop_lost_counts(0, 0, 0, 0, 0, &ctl));
    /* the real purpose: a human in control, no scene, the second human gone from the game */
    CHECK(glue_coop_lost_counts(0, 0, 1, 0, 0, &ctl));
    CHECK(glue_coop_lost_counts(0, 0, 1, 0, 0, &dead));   /* dead humans count as controlled (the original behaviour) */
    /* the role set-up must not ask for a join during a scene (cutscene view / running demo) ... */
    glue_local_t scene = ctl;
    scene.demo_running = 1;
    CHECK(!glue_may_join(&scene));
    scene.demo_running = 0, scene.view_mode = 2;
    CHECK(!glue_may_join(&scene));
    CHECK(glue_may_join(&ctl));
    /* ... but a human that merely stands in the scripted action must be allowed to join: the AI partner Moira of Claire's half is in INT_MIN from the start (first 6f run: the host
     * waited 2 minutes for a join that the guard refused) */
    CHECK(glue_may_join(&scripted));
    CHECK(!glue_in_control(&scripted));   /* the gate still waits for her */
}

/* a model of the glue's retry: puppet type over time with the old and the new rule, against a "scene needs the partner as an AI partner" world */
static void test_model(void)
{
    printf("-- model: the partner's type while the scene plays (the hang is a type-2 puppet during the scene)\n");
    unsigned scene_start = 2200, scene_end = 26200;
    int old_hang = 0, new_hang = 0;
    glue_gate_t g;
    unsigned base = 5000;
    glue_gate_init(&g, base);
    int new_type = 1, old_type = 2;   /* old rule: type 2 from the barrier on */
    for (unsigned t = 0; t < 60000; t += 50) {
        glue_local_t l = retry_state(t, scene_start, scene_end);
        unsigned s, h;
        if (new_type == 1 && glue_gate_update(&g, base + t, &l, &s, &h)) new_type = 2;
        if (t >= scene_start && t < scene_end) {
            if (old_type == 2) old_hang = 1;
            if (new_type == 2) new_hang = 1;
        }
    }
    CHECK(old_hang);    /* the failing behaviour is visible in the model */
    CHECK(!new_hang);   /* and gone with the gate */
    CHECK(new_type == 2);   /* the puppet is there after the scene */
}

/* the real cause: the client refused the enemy that the opening scene's script waits for, asking every frame */
static void test_demand(void)
{
    printf("-- a story that keeps asking for the same enemy gets it\n");
    glue_demand_t tab[GLUE_DEMAND_SLOTS];
    memset(tab, 0, sizeof tab);
    unsigned t = 4294960000u;   /* the tick count wraps inside the test */
    int allowed_at = -1;
    for (int k = 0; k < 100; k++, t += 20) {   /* 50 Hz, the pattern of the s6e2 / s6f1 logs */
        if (glue_demand_check(tab, 100, 116u, t) && allowed_at < 0) allowed_at = k;
    }
    CHECK(allowed_at == GLUE_DEMAND_N - 1);   /* refused 7 times, the 8th request (140 ms) is let through */
    /* a single request, or one now and one later, is never let through (the host's SPAWN creates it) */
    memset(tab, 0, sizeof tab);
    int any = 0;
    any |= glue_demand_check(tab, 100, 116u, 1000);
    any |= glue_demand_check(tab, 100, 116u, 3000);
    any |= glue_demand_check(tab, 100, 116u, 9000);
    CHECK(!any);
    /* seven requests per second never escalate (a slow story loop) */
    memset(tab, 0, sizeof tab);
    any = 0;
    for (int k = 0; k < 40; k++) any |= glue_demand_check(tab, 100, 116u, 1000 + (unsigned)k * 150);
    CHECK(!any);
    /* different layouts do not add up, and more layouts than slots do not break the table */
    memset(tab, 0, sizeof tab);
    any = 0;
    for (int k = 0; k < 40; k++) any |= glue_demand_check(tab, 100 + k % 20, (unsigned)(k % 20), 1000 + (unsigned)k * 20);
    CHECK(!any);
    /* two enemies asked for at the same time both get through */
    memset(tab, 0, sizeof tab);
    int a1 = 0, a2 = 0;
    for (int k = 0; k < 20; k++) {
        a1 += glue_demand_check(tab, 100, 116u, 5000 + (unsigned)k * 20);
        a2 += glue_demand_check(tab, 100, 3u, 5000 + (unsigned)k * 20);
    }
    CHECK(a1 > 0 && a2 > 0);
}

/* ---- stage 6g, task b: the engine's own game-over state ---- */
static glue_dead_t alive(void)
{
    glue_dead_t d;
    memset(&d, 0, sizeof d);
    d.hp = 1000, d.action = 2, d.agame_state = 3;   /* idle, aGame in its main gameplay state */
    return d;
}

static void test_dead(void)
{
    printf("-- dead: the shapes seen on hardware and the engine's own state\n");
    glue_dead_t d = alive();
    CHECK(glue_dead_reasons(&d) == 0);
    d.action = 3, d.ctrl = 0x00092008u, d.pstate = 0, d.f7904 = 0x300;   /* walking / knife attack, restrictions word: alive */
    CHECK(glue_dead_reasons(&d) == 0);
    d.action = 0x80000000u, d.pstate = 0x00000000, d.ctrl = 0x00000008u, d.hp = 706;   /* held by an enemy or in a scene, healthy: NOT dead */
    CHECK(glue_dead_reasons(&d) == 0);
    d.pstate = 0x80000000u;   /* a scene's pstate: alive */
    d.hp = 1;
    CHECK(glue_dead_reasons(&d) == 0);

    /* hp 0 (act 108) */
    d = alive(); d.hp = 0, d.action = 108;
    CHECK(glue_dead_reasons(&d) & GLUE_DR_HP);
    d.hp = -20;
    CHECK(glue_dead_reasons(&d) & GLUE_DR_HP);
    /* the 32 bit hp: a value whose low 16 bits are positive is still dead when negative (the old sample cast to int16) */
    d = alive(); d.hp = (int)0xFFFF8000u;
    CHECK(glue_dead_reasons(&d) & GLUE_DR_HP);

    /* a fall into the void, 6f1: hp > 0, act 118, pstate 80000000, ctrl 00001002 */
    d = alive(); d.hp = 1000, d.action = 118, d.pstate = 0x80000000u, d.ctrl = 0x00001002u;
    CHECK(glue_dead_reasons(&d) & GLUE_DR_ENGINE_PRED);
    CHECK(glue_dead_reasons(&d) & GLUE_DR_OLD_VOID);
    /* the same without the old shape (ctrl bit 1 alone is what the engine reads) */
    d.action = 2, d.pstate = 0;
    CHECK(glue_dead_reasons(&d) == GLUE_DR_ENGINE_PRED);

    /* the case the old rules missed (6f3): a HELD human fell into the void: hp 96, act INT_MIN, pstate 4000, ctrl 80800400 */
    d = alive(); d.hp = 96, d.action = 0x80000000u, d.pstate = 0x00004000u, d.ctrl = 0x80800400u;
    CHECK(glue_dead_reasons(&d) == GLUE_DR_ENGINE_PRED);
    /* the old rule alone: neither hp <= 0 nor (act 118 + pstate 80000000) - the model of the bug */
    CHECK(!(d.hp <= 0 || (d.action == 118 && d.pstate == 0x80000000u)));
    /* the predicate counts only in the gameplay state: a scene (aGame state 9) with the same words is not a death, nor is the title / a load (state 0) */
    d = alive(); d.ctrl = 0x00001002u, d.agame_state = 9;
    CHECK(glue_dead_reasons(&d) == 0);
    d.agame_state = 0;
    CHECK(glue_dead_reasons(&d) == 0);
    d = alive(); d.hp = 96, d.action = 0x80000000u, d.pstate = 0x00004000u, d.agame_state = 9;
    CHECK(glue_dead_reasons(&d) == 0);
    /* flags7904 bit 28 */
    d = alive(); d.f7904 = 0x10000000u;
    CHECK(glue_dead_reasons(&d) == GLUE_DR_ENGINE_PRED);

    /* the engine's record: the mask bit of the slot, then the game-over state with the screen up (aGame+4 == 0x0B), whatever the body shows */
    d = alive(); d.mask_bit = 1;
    CHECK(glue_dead_reasons(&d) == GLUE_DR_MASK);
    d = alive(); d.agame_state = 0x0B;
    CHECK(glue_dead_reasons(&d) == GLUE_DR_SCREEN);
    d.agame_state = 0x0B | 0x300;   /* only the low byte is the state */
    CHECK(glue_dead_reasons(&d) == GLUE_DR_SCREEN);
    d = alive(); d.agame_state = 0x0C;   /* the state after the choice: not "the screen is up" */
    CHECK(glue_dead_reasons(&d) == 0);
    /* retry: the mask is cleared and the state goes back to 3: no death left over */
    d = alive();
    CHECK(glue_dead_reasons(&d) == 0);
}

/* ---- stage 6g, task a: holds ---- */
static glue_hold_in_t hin(int held, int found, int hp, int chain)
{
    glue_hold_in_t i;
    i.held = held, i.holder_found = found, i.holder_hp = hp, i.holder_in_chain = chain, i.max_ms = 0, i.holder_seen = 1;
    return i;
}

static void test_hold(void)
{
    printf("-- hold: the victim is freed when the holder is gone / dead / out of the chain, or after the cap\n");
    CHECK(glue_is_held(GLUE_SCRIPTED_ACT, 0x2300));
    CHECK(!glue_is_held(GLUE_SCRIPTED_ACT, 0x0300));   /* a scene / the restrictions word alone */
    CHECK(!glue_is_held(2, 0x2000));
    CHECK(glue_grab_family(77) && glue_grab_family(0x4D) && glue_grab_family(0x58) && !glue_grab_family(0x4C) && !glue_grab_family(0x59) && !glue_grab_family(2) && !glue_grab_family(16));

    /* a normal 5 s hold with a living holder in the chain: never released */
    glue_hold_t h;
    memset(&h, 0, sizeof h);
    unsigned base = 4294960000u;   /* the 32 bit tick wraps inside this test */
    int rel = 0;
    for (unsigned t = 0; t < 5000; t += 50) {
        glue_hold_in_t i = hin(1, 1, 400, 1);
        rel |= glue_hold_step(&h, base + t, &i);
    }
    CHECK(rel == 0);
    glue_hold_in_t free_i = hin(0, 0, 0, 0);
    CHECK(glue_hold_step(&h, base + 5100, &free_i) == GLUE_HOLD_NONE && h.held_since == 0);

    /* the holder died (hp 0) but the victim stays held (6f3 wedge): released 1.5 s after the death, once, then again every 2 s while still held */
    memset(&h, 0, sizeof h);
    unsigned first = 0, count = 0, last = 0;
    for (unsigned t = 0; t < 8000; t += 50) {
        glue_hold_in_t i = hin(1, 1, t < 2000 ? 400 : 0, 1);
        int r = glue_hold_step(&h, base + t, &i);
        if (r) {
            CHECK(r == GLUE_HOLD_REL_ORPHAN);
            if (!first) first = t;
            if (last) CHECK(t - last >= GLUE_HOLD_RETRY_MS - 50);
            last = t, count++;
        }
    }
    CHECK(first >= 2000 + GLUE_HOLD_ORPHAN_MS - 50 && first <= 2000 + GLUE_HOLD_ORPHAN_MS + 100);
    CHECK(count >= 3 && count <= 4);

    /* the holder left the chain (the stock code would release in one frame; if it did not, we free the victim after 1.5 s) - but a flicker shorter than that never does */
    memset(&h, 0, sizeof h);
    rel = 0;
    for (unsigned t = 0; t < 6000; t += 50) {
        int chain = !(t >= 1000 && t < 1000 + GLUE_HOLD_ORPHAN_MS - 200);
        glue_hold_in_t i = hin(1, 1, 400, chain);
        rel |= glue_hold_step(&h, base + t, &i);
    }
    CHECK(rel == 0);
    memset(&h, 0, sizeof h);
    rel = 0;
    for (unsigned t = 0; t < 6000; t += 50) {
        glue_hold_in_t i = hin(1, 1, 400, t < 1000);
        rel |= glue_hold_step(&h, base + t, &i);
    }
    CHECK(rel == GLUE_HOLD_REL_ORPHAN);

    /* holder not found at all (an object that is gone): freed after the grace time */
    memset(&h, 0, sizeof h);
    first = 0;
    for (unsigned t = 0; t < 8000 && !first; t += 50) {
        glue_hold_in_t i = hin(1, 0, 0, 0);
        if (glue_hold_step(&h, base + t, &i)) first = t ? t : 1;
    }
    CHECK(first >= GLUE_HOLD_LOST_MS - 50 && first <= GLUE_HOLD_LOST_MS + 100);

    /* a wedged LIVE holder in the chain (host enemy stuck in 77): the hard cap frees the victim at 15 s */
    memset(&h, 0, sizeof h);
    first = 0;
    for (unsigned t = 0; t < 30000 && !first; t += 50) {
        glue_hold_in_t i = hin(1, 1, 400, 1);
        int r = glue_hold_step(&h, base + t, &i);
        if (r) {
            CHECK(r == GLUE_HOLD_REL_MAX);
            first = t;
        }
    }
    CHECK(first >= GLUE_HOLD_MAX_MS - 50 && first <= GLUE_HOLD_MAX_MS + 100);

    /* 6g hardware: the opening scene of a retry looks like a hold (act INT_MIN, flags 0x2000) with no holder at all: the watcher must not touch it (it cut the scene 4 s in) */
    memset(&h, 0, sizeof h);
    rel = 0;
    for (unsigned t = 0; t < 60000; t += 50) {
        glue_hold_in_t i = hin(1, 0, 0, 0);
        i.holder_seen = 0;
        rel |= glue_hold_step(&h, base + t, &i);
    }
    CHECK(rel == 0);
    /* ... and an enemy hold whose holder was seen and then lost IS freed (the knob and the real wedge) */
    memset(&h, 0, sizeof h);
    first = 0;
    for (unsigned t = 0; t < 10000 && !first; t += 50) {
        glue_hold_in_t i = hin(1, t < 1000, 400, 1);
        if (glue_hold_step(&h, base + t, &i)) first = t;
    }
    CHECK(first >= 1000 + GLUE_HOLD_LOST_MS - 100 && first <= 1000 + GLUE_HOLD_LOST_MS + 100);

    /* a holder of another class (a boss with a long scripted hold): the longer cap */
    memset(&h, 0, sizeof h);
    first = 0;
    for (unsigned t = 0; t < 120000 && !first; t += 100) {
        glue_hold_in_t i = hin(1, 1, 4000, 1);
        i.max_ms = GLUE_HOLD_MAX_OTHER_MS;
        if (glue_hold_step(&h, base + t, &i)) first = t;
    }
    CHECK(first >= GLUE_HOLD_MAX_OTHER_MS - 100 && first <= GLUE_HOLD_MAX_OTHER_MS + 200);

    /* a new hold after a release starts a new clock */
    memset(&h, 0, sizeof h);
    glue_hold_in_t w = hin(1, 1, 0, 1);
    glue_hold_step(&h, base, &w);
    glue_hold_step(&h, base + 2000, &w);   /* released at 2 s (orphan) */
    CHECK(h.releases == 1);
    glue_hold_in_t f = hin(0, 0, 0, 0);
    glue_hold_step(&h, base + 2100, &f);
    CHECK(h.held_since == 0 && h.orphan_since == 0 && h.last_release == 0);
    glue_hold_in_t ok = hin(1, 1, 400, 1);
    CHECK(glue_hold_step(&h, base + 9000, &ok) == GLUE_HOLD_NONE && h.held_since == base + 9000);
}

/* ---- stage 6g, task a: a replica inside the grab chain is not re-forced ---- */
static void test_replica_force(void)
{
    printf("-- replica: the grab chain is not restarted\n");
    CHECK(!glue_replica_force_ok(77, 77));
    CHECK(glue_replica_force_ok(77, 3));      /* start of the grab: the replica was running */
    CHECK(glue_replica_force_ok(77, 2));
    CHECK(!glue_replica_force_ok(77, 79));    /* the replica's own chain moved on: do NOT force 77 again (the 77 <-> 79 cycling of 6f3) */
    CHECK(!glue_replica_force_ok(0x4F, 0x51));
    CHECK(glue_replica_force_ok(16, 79));     /* the host left the chain: forced */
    CHECK(glue_replica_force_ok(2, 77));
    CHECK(glue_replica_force_ok(74, 0x58));
    CHECK(glue_replica_force_ok(0x59, 0x4D));
    /* replay of the 6f3 timeline: the host holds 77 for 5 s; the replica's own chain walks 77 -> 79 -> 77. The old rule forced 77 at every mismatch (cycling), the new rule once (the start) */
    for (int rule = 0; rule < 2; rule++) {
        unsigned replica = 3, host = 77, forced = 0;
        for (unsigned t = 0; t < 5000; t += 66) {
            int force = rule == 0 ? host != replica : glue_replica_force_ok(host, replica);
            if (force) forced++, replica = host;
            else if (replica == 77 && t > 1000 && t < 4000) replica = 79;   /* the replica's own chain moves on */
            else if (replica == 79 && t >= 4000) replica = 77;
        }
        if (rule == 0) CHECK(forced >= 2);   /* the old rule restarts the grab at least once more after the start */
        else CHECK(forced == 1);
    }
}

int main(void)
{
    test_dead();
    test_hold();
    test_replica_force();
    test_demand();
    test_retry_scene();
    test_gate_caps_and_states();
    test_watchdog();
    test_model();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
