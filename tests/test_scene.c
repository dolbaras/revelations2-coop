/* test_scene.c: scene032, the cutscene sync logic (native/scene_logic.h). Built with -DSC_MODEL_REL032 (the rel032 behaviour) it must FAIL (tests/build.sh checks that). */
#include <stdio.h>
#include "../native/scene_logic.h"

static int bad, n;
#define CHECK(c, ...) do { n++; if (!(c)) { bad++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_playing[4];
static int playing(uint32_t serial)
{
    for (int i = 0; i < 4; i++)
        if (g_playing[i] && g_playing[i] == serial) return 1;
    return 0;
}

static void barrier(void)
{
    sc_bar b;
    /* kind 7: the faster PC holds at the end of its load until the partner reaches the same point */
    sc_bar_reset(&b, 0);
    sc_bar_on_caps(&b, 1);
    sc_bar_load_begin(&b, 800);
    CHECK(sc_bar_arrive(&b, SC_KIND_LOAD, "a100", 1000) == 1, "first arrival is a new hold");
    CHECK(sc_bar_arrive(&b, SC_KIND_LOAD, "a100", 1016) == 0, "the next check of the same point is no new hold");
    CHECK(sc_bar_decide(&b, 1500, 1, 1, 0) == SC_HOLD, "kind 7 holds while the partner still loads (got %d)", sc_bar_decide(&b, 1500, 1, 1, 0));
    sc_bar_on_ready(&b, SC_KIND_LOAD, "a100", 18700);
    CHECK(sc_bar_decide(&b, 18716, 1, 1, 0) == SC_REL_MATCH, "kind 7 opens on the partner's READY of the same area (got %d)", sc_bar_decide(&b, 18716, 1, 1, 0));
    sc_bar_release(&b, 0);
    /* the used READY does not open the next point (a retry of the same stage) */
    sc_bar_load_begin(&b, 30000);
    sc_bar_arrive(&b, SC_KIND_LOAD, "a100", 40000);
    CHECK(sc_bar_decide(&b, 40100, 1, 1, 0) == SC_HOLD, "a used READY does not open the next hold (got %d)", sc_bar_decide(&b, 40100, 1, 1, 0));
    /* a stale READY (it came before this PC's own load began) does not open */
    sc_bar_on_ready(&b, SC_KIND_LOAD, "a100", 29000);
    CHECK(sc_bar_decide(&b, 40200, 1, 1, 0) == SC_HOLD, "a READY from before this load is stale (got %d)", sc_bar_decide(&b, 40200, 1, 1, 0));
    sc_bar_on_ready(&b, SC_KIND_LOAD, "a101", 41000);
    CHECK(sc_bar_decide(&b, 41100, 1, 1, 0) == SC_HOLD, "a READY of another area does not open (got %d)", sc_bar_decide(&b, 41100, 1, 1, 0));
    /* the partner is gone: at once */
    CHECK(sc_bar_decide(&b, 41200, 0, 1, 0) == SC_REL_ALONE, "partner lost releases at once (got %d)", sc_bar_decide(&b, 41200, 0, 1, 0));
    /* off: at once */
    CHECK(sc_bar_decide(&b, 41200, 1, 0, 0) == SC_REL_OFF, "scene sync off releases at once");
    /* the limit of a load: 300 s */
    CHECK(sc_bar_decide(&b, 40000 + SC_LOAD_LIMIT_MS - 1, 1, 1, 0) == SC_HOLD, "kind 7 holds until 300 s");
    CHECK(sc_bar_decide(&b, 40000 + SC_LOAD_LIMIT_MS, 1, 1, 0) == SC_REL_LIMIT, "kind 7 ends at 300 s (got %d)", sc_bar_decide(&b, 40000 + SC_LOAD_LIMIT_MS, 1, 1, 0));
    sc_bar_release(&b, 0);

    /* kind 8 inside a load (the glue is not running): the movie start waits for the partner's movie */
    sc_bar_reset(&b, 0);
    sc_bar_on_caps(&b, 1);
    sc_bar_load_begin(&b, 100);
    sc_bar_arrive(&b, SC_KIND_MOVIE, "d1000", 9000);
    CHECK(sc_bar_decide(&b, 9500, 1, 1, 0) == SC_HOLD, "a movie in a load holds (got %d)", sc_bar_decide(&b, 9500, 1, 1, 0));
    sc_bar_on_ready(&b, SC_KIND_MOVIE, "d1010", 9600);
    CHECK(sc_bar_decide(&b, 9700, 1, 1, 1) == SC_HOLD, "the partner's READY of ANOTHER movie does not open (got %d)", sc_bar_decide(&b, 9700, 1, 1, 1));
    CHECK(sc_bar_decide(&b, 9000 + SC_MOVIE_LIMIT_MS - 1, 1, 1, 1) == SC_HOLD, "a movie waits up to 30 s (a PC without this movie)");
    CHECK(sc_bar_decide(&b, 9000 + SC_MOVIE_LIMIT_MS, 1, 1, 1) == SC_REL_LIMIT, "a movie is let go at 30 s (got %d)", sc_bar_decide(&b, 9000 + SC_MOVIE_LIMIT_MS, 1, 1, 1));
    sc_bar_on_ready(&b, SC_KIND_MOVIE, "d1000", 12000);
    CHECK(sc_bar_decide(&b, 12016, 1, 1, 0) == SC_REL_MATCH, "the same movie opens (got %d)", sc_bar_decide(&b, 12016, 1, 1, 0));
    sc_bar_release(&b, 12016);
    /* a movie READY that came before this PC noticed its own load still counts (only one from before the last movie is stale) */
    sc_bar_on_ready(&b, SC_KIND_MOVIE, "d1010", 20000);
    sc_bar_load_begin(&b, 21000);
    sc_bar_arrive(&b, SC_KIND_MOVIE, "d1010", 23000);
    CHECK(sc_bar_decide(&b, 23016, 1, 1, 0) == SC_REL_MATCH, "a movie READY from before this load (after the last movie) opens (got %d)", sc_bar_decide(&b, 23016, 1, 1, 0));
    sc_bar_release(&b, 23016);
    sc_bar_on_ready(&b, SC_KIND_MOVIE, "d1100", 23000);
    sc_bar_arrive(&b, SC_KIND_MOVIE, "d1100", 40000);
    CHECK(sc_bar_decide(&b, 40016, 1, 1, 0) == SC_HOLD, "a movie READY from before the last movie is stale (got %d)", sc_bar_decide(&b, 40016, 1, 1, 0));
    sc_bar_release(&b, 40016);

    /* the partner does not load (in play): INPLAY releases, but only after 2 s at the hold and not after the partner said its load began */
    sc_bar_reset(&b, 0);
    sc_bar_on_caps(&b, 1);
    sc_bar_load_begin(&b, 100);
    sc_bar_arrive(&b, SC_KIND_LOAD, "a101", 5000);
    sc_bar_on_inplay(&b, "a101", 5050);
    CHECK(sc_bar_decide(&b, 5000 + SC_INPLAY_MIN_MS - 1, 1, 1, 0) == SC_HOLD, "INPLAY waits SC_INPLAY_MIN_MS");
    CHECK(sc_bar_decide(&b, 5000 + SC_INPLAY_MIN_MS, 1, 1, 0) == SC_REL_INPLAY, "INPLAY in the same area releases (got %d)", sc_bar_decide(&b, 5000 + SC_INPLAY_MIN_MS, 1, 1, 0));
    sc_bar_on_partner_loading(&b, 5100);
    CHECK(sc_bar_decide(&b, 8000, 1, 1, 0) == SC_HOLD, "an INPLAY followed by the partner's own load does not release (got %d)", sc_bar_decide(&b, 8000, 1, 1, 0));
    sc_bar_on_inplay(&b, "a201", 8100);
    CHECK(sc_bar_decide(&b, 8200, 1, 1, 0) == SC_REL_ELSEWHERE, "INPLAY elsewhere releases (got %d)", sc_bar_decide(&b, 8200, 1, 1, 0));
    sc_bar_release(&b, 0);
    CHECK(!sc_should_answer_inplay(1, 4999, 0) && sc_should_answer_inplay(1, 5000, 0) && !sc_should_answer_inplay(1, 9000, 1) && !sc_should_answer_inplay(0, 9000, 0), "INPLAY answer rule");

    /* a partner without scene sync: CAPS off at once, nothing heard after 10 s */
    sc_bar_reset(&b, 0);
    sc_bar_arrive(&b, SC_KIND_LOAD, "a100", 1000);
    CHECK(sc_bar_decide(&b, 1000 + SC_CAPS_WAIT_MS - 1, 1, 1, 0) == SC_HOLD, "no CAPS yet: hold up to 10 s");
    CHECK(sc_bar_decide(&b, 1000 + SC_CAPS_WAIT_MS, 1, 1, 0) == SC_REL_NOCAPS, "no CAPS after 10 s: released (got %d)", sc_bar_decide(&b, 1000 + SC_CAPS_WAIT_MS, 1, 1, 0));
    sc_bar_on_caps(&b, 0);
    CHECK(sc_bar_decide(&b, 1100, 1, 1, 0) == SC_REL_NOCAPS, "CAPS off: released at once (got %d)", sc_bar_decide(&b, 1100, 1, 1, 0));
}

static void vote(void)
{
    sc_vote v;
    memset(&v, 0, sizeof v);
    sc_vote_begin(&v, "d1000", 1000);
    CHECK(sc_vote_mine(&v) == 1 && sc_vote_mine(&v) == 0, "a vote is sent once");
    CHECK(!sc_vote_skip(&v, 1), "a lone skip changes nothing while the partner is there (the stock 'me alone' vote skips)");
    CHECK(sc_vote_strip(&v, 1) == SC_STRIP_WAIT, "the voter sees the waiting strip");
    sc_vote_theirs(&v, "d1000", 2000);
    CHECK(sc_vote_skip(&v, 1), "both voted: skip");
    CHECK(sc_vote_strip(&v, 1) == SC_STRIP_NONE, "no strip once both voted");
    sc_vote_end(&v);
    CHECK(!sc_vote_skip(&v, 1), "the vote ends with the scene");

    sc_vote_begin(&v, "L100,105,0", 3000);
    sc_vote_theirs(&v, "L100,105,0", 3100);
    CHECK(sc_vote_strip(&v, 1) == SC_STRIP_PARTNER && !sc_vote_skip(&v, 1), "the partner's vote alone: the strip asks this player, no skip");
    sc_vote_mine(&v);
    CHECK(sc_vote_skip(&v, 1), "then this player: skip");
    sc_vote_end(&v);

    sc_vote_begin(&v, "d1010", 4000);
    sc_vote_theirs(&v, "d1000", 4100);
    sc_vote_mine(&v);
    CHECK(!sc_vote_skip(&v, 1), "a vote for another scene does not count");
    CHECK(sc_vote_skip(&v, 0), "no partner: this player's vote alone skips (the stock game)");
    sc_vote_end(&v);

    sc_vote_theirs(&v, "d1100", 5000);   /* the partner started (and voted) a moment earlier */
    sc_vote_begin(&v, "d1100", 5200);
    CHECK(v.theirs == 1, "a vote that came before this PC's own start of the same scene counts");
    sc_vote_end(&v);
    sc_vote_theirs(&v, "d1110", 6000);
    sc_vote_begin(&v, "d1110", 6000 + SC_VOTE_PEND_MS);
    CHECK(v.theirs == 0, "an old early vote does not count");
}

static void dedupe(void)
{
    sc_dd d;
    memset(&d, 0, sizeof d);
    memset(g_playing, 0, sizeof g_playing);
    /* this PC started the scene itself; the partner's SCENE of the same scene comes later: no restart */
    sc_dd_note(&d, "L100,105,0", SC_FROM_OWN, 5, 1000);
    g_playing[0] = 5;
    CHECK(sc_dd_partner_skip(&d, "L100,105,0", 18000, playing), "the partner's SCENE of a scene that plays here is skipped (rel032 restarted it)");
    /* the partner's SCENE came first; this PC's own submit of the same scene is suppressed while that copy plays */
    sc_dd_note(&d, "L400,120,0", SC_FROM_PARTNER, 7, 20000);
    g_playing[1] = 7;
    CHECK(sc_dd_own_suppress(&d, "L400,120,0", 21000, playing), "this PC's duplicate submit is suppressed while the partner's copy plays (rel032 restarted it)");
    /* never block a scene: once the partner's copy ended, this PC's own submit goes through */
    g_playing[1] = 0;
    CHECK(!sc_dd_own_suppress(&d, "L400,120,0", 60000, playing), "a submit after the copy ended is not suppressed");
    /* a wrong key only turns the dedupe off: empty, or not equal on the two PCs */
    CHECK(!sc_dd_own_suppress(&d, "", 21000, playing) && !sc_dd_partner_skip(&d, "", 1500, playing), "an empty key never drops a scene");
    CHECK(!sc_dd_partner_skip(&d, "L100,105,1", 1500, playing) && !sc_dd_own_suppress(&d, "L400,120,1", 21000, playing), "a different key never drops a scene");
    /* just started here (the slot does not show it yet) */
    memset(&d, 0, sizeof d);
    memset(g_playing, 0, sizeof g_playing);
    sc_dd_note(&d, "L102,115,23", SC_FROM_OWN, 9, 50000);
    CHECK(sc_dd_partner_skip(&d, "L102,115,23", 50000 + SC_DD_RECENT_MS - 1, playing), "a start this recent counts as playing");
    CHECK(!sc_dd_partner_skip(&d, "L102,115,23", 50000 + SC_DD_RECENT_MS, playing), "an ended start does not block the partner's scene");
    g_playing[0] = 9;
    CHECK(!sc_dd_partner_skip(&d, "L102,115,23", 50000 + SC_DD_WINDOW_MS + 1, playing), "outside the window nothing matches");
    CHECK(d.e[0].t != 0, "note");
}

static void scene_hold(void)
{
    CHECK(sc_scene_hold(1, 1, 1, 0, 0) && sc_scene_hold(1, 1, 1, 0, SC_SCENE_HOLD_MS - 1), "the first PC holds its own scene's start until the partner's copy started");
    CHECK(!sc_scene_hold(1, 1, 1, 0, SC_SCENE_HOLD_MS), "at most 1 s: never a hang");
    CHECK(!sc_scene_hold(1, 1, 1, 1, 10), "released when the partner's copy started");
    CHECK(!sc_scene_hold(1, 1, 0, 0, 10), "a copy started from the partner's SCENE is not held");
    CHECK(!sc_scene_hold(1, 0, 1, 0, 10), "no key: no hold (a wrong key never blocks a scene)");
    CHECK(!sc_scene_hold(0, 1, 1, 0, 10), "no partner with scene sync: no hold");
}

static void door(void)
{
    CHECK(!sc_demo_sm_apply(1, 1, 0, 1), "a playing scene set model is not set back (the door snapshot 1 -> 0 restarted it, g032r2g)");
    CHECK(!sc_demo_sm_apply(1, 2, 1, 1), "a finished scene set model is not started again");
    CHECK(!sc_demo_sm_apply(1, 1, 1, 1), "same action: nothing");
    CHECK(sc_demo_sm_apply(1, 0, 1, 1) && sc_demo_sm_apply(1, 1, 2, 1) && sc_demo_sm_apply(1, 0, 2, 0), "forward steps are applied");
    CHECK(!sc_demo_sm_apply(1, 0, 1, 0), "Wait -> Move before this PC's own trigger: not applied (live sc032n: the Move action started with player -1, bind pose)");
    CHECK(sc_demo_sm_apply(0, 1, 0, 0) && sc_demo_sm_apply(1, -2147483647 - 1, 1, 0), "other classes and an unknown local action as before");
}

/* the gate of the first in-engine scene after a load: the earlier PC's story waits (no submit) until the partner's story reached the same scene, at most 15 s */
static void gate(void)
{
    sc_gate g;
    memset(&g, 0, sizeof g);
    const char *k = "S100,105,0";
    sc_gate_arm(&g);
    CHECK(sc_gate_decide(&g, k, 1000, 1, 1, 1, 0) == SC_GATE_WAIT, "the earlier PC does not submit the first scene while the partner's story has not reached it (got %d)", sc_gate_decide(&g, k, 1000, 1, 1, 1, 0));
    CHECK(sc_gate_decide(&g, k, 7600, 1, 1, 1, 0) == SC_GATE_WAIT, "still waiting 6.6 s later (the sc032c spread)");
    sc_gate_on_partner(&g, "S100,116,2", 7700);
    CHECK(sc_gate_decide(&g, k, 7716, 1, 1, 1, 0) == SC_GATE_WAIT, "the partner's SREADY of another scene does not release");
    sc_gate_on_partner(&g, k, 7800);
    CHECK(sc_gate_decide(&g, k, 7816, 1, 1, 1, 0) == SC_GATE_MATCH, "the partner's SREADY of the same scene releases (got %d)", sc_gate_decide(&g, k, 7816, 1, 1, 1, 0));
    sc_gate_disarm(&g);
    CHECK(sc_gate_decide(&g, "S100,116,2", 9000, 1, 1, 1, 0) == SC_GATE_PASS, "only the first scene after a load is gated");
    /* the later PC: the partner's SREADY is already there when its own story reaches the scene: no wait at all */
    sc_gate_arm(&g);
    sc_gate_on_partner(&g, k, 20000);
    CHECK(sc_gate_decide(&g, k, 20500, 1, 1, 1, 0) == SC_GATE_MATCH, "the later PC passes at once");
    /* a stale SREADY (the partner stopped waiting) does not release */
    sc_gate_arm(&g);
    sc_gate_on_partner(&g, k, 30000);
    CHECK(sc_gate_decide(&g, k, 30000 + SC_GATE_FRESH_MS, 1, 1, 1, 0) == SC_GATE_WAIT, "an SREADY older than 3 s is stale");
    /* a load voids the partner's SREADY */
    sc_gate_on_partner(&g, k, 40000);
    sc_gate_arm(&g);
    CHECK(sc_gate_decide(&g, k, 40100, 1, 1, 1, 0) == SC_GATE_WAIT, "an SREADY from before this load does not release");
    /* never a hang */
    CHECK(sc_gate_decide(&g, k, 40100 + SC_GATE_LIMIT_MS - 1, 1, 1, 1, 0) == SC_GATE_WAIT && sc_gate_decide(&g, k, 40100 + SC_GATE_LIMIT_MS, 1, 1, 1, 0) == SC_GATE_LIMIT,
          "the 15 s limit releases");
    sc_gate_arm(&g);
    CHECK(sc_gate_decide(&g, k, 50000, 1, 0, 1, 0) == SC_GATE_ALONE, "no partner: at once");
    CHECK(sc_gate_decide(&g, k, 50000, 1, 1, 0, 0) == SC_GATE_NOGATE, "a partner without the gate: at once");
    CHECK(sc_gate_decide(&g, k, 50000, 1, 1, 1, 1) == SC_GATE_STARTED, "the partner's copy of this scene started already: at once");
    CHECK(sc_gate_decide(&g, k, 50000, 0, 1, 1, 0) == SC_GATE_PASS, "a director that submits later than its frame 0 is never deferred (that call ignores the result)");
    CHECK(sc_gate_decide(&g, "", 50000, 1, 1, 1, 0) == SC_GATE_PASS, "no key: no gate (a wrong key never blocks a scene)");
    /* the partner's SCENE while the gate is armed: this PC's own story starts the first scene (no copy without a director, no second start) */
    sc_gate_arm(&g);
    CHECK(sc_gate_keeps_own(&g, 60000 + 8000, 60000, 1), "a received SCENE 8 s after the load is not submitted while the gate is armed");
    CHECK(!sc_gate_keeps_own(&g, 60000 + SC_GATE_WINDOW_MS, 60000, 1), "after the window the received SCENE is submitted as before");
    CHECK(!sc_gate_keeps_own(&g, 60000 + 8000, 60000, 0), "a partner without the gate: submitted as before");
    sc_gate_disarm(&g);
    CHECK(!sc_gate_keeps_own(&g, 60000 + 8000, 60000, 1), "after the first scene: submitted as before");
}

/* a received SCENE: this PC's own scene set model starts its directors (the received bytes point at the sender's director) */
static void receive(void)
{
    CHECK(sc_rx_decide(1, 1, 0, 0, 0) == SC_RX_START_LOCAL, "a host-triggered scene (the knife table) starts the guest's own directors, not the sender's request bytes");
    CHECK(sc_rx_decide(1, 1, 1, 0, 0) == SC_RX_KEEP_OUT, "the guest's own set model runs the scene already: neither the bytes nor a second start");
    CHECK(sc_rx_decide(1, 1, 0, 1, 0) == SC_RX_KEEP_OUT, "the first scene after a load: each PC's own story starts it, the bytes stay out");
    CHECK(sc_rx_decide(1, 1, 0, 0, 1) == SC_RX_KEEP_OUT, "the partner's TRIG waits here for this PC's own sequence: the SCENE neither submitted nor started (no second start)");
    CHECK(sc_rx_decide(1, 0, 0, 0, 0) == SC_RX_APPLY, "no scene set model of that key here: the bytes as before");
    CHECK(sc_rx_decide(0, 1, 0, 0, 0) == SC_RX_APPLY, "no co-op partner with scene sync: as before");
    CHECK(sc_local_start_runs_scheduler(), "the local start from a SCENE runs the Move action's scheduler call before the directors (camera frame stayed 0.0 without it)");
}

static void trigger(void)
{
    CHECK(sc_trig_decide(1, 1, 0, 0, 1, SC_SEQ_WAIT_TRIGGER) == SC_TRIG_APPLY, "the host's knife table trigger: the guest triggers its own scene set model (its stock sequence runs)");
    CHECK(sc_trig_decide(1, 1, 1, 0, 1, SC_SEQ_WAIT_TRIGGER) == SC_TRIG_HAVE, "triggered here already (both humans in the zone, or it ran): no second trigger");
    CHECK(sc_trig_decide(1, 1, 0, 1, 1, SC_SEQ_WAIT_TRIGGER) == SC_TRIG_WINDOW, "the first scene after a load: each PC's own story triggers it");
    CHECK(sc_trig_decide(1, 1, 0, 0, 0, SC_SEQ_WAIT_TRIGGER) == SC_TRIG_NOPLAYER, "no player object of the trigger's slot here: never a trigger with a null player");
    CHECK(sc_trig_decide(1, 1, 0, 0, 1, 2) == SC_TRIG_WAIT, "its sequence still prepares (live sc032h: the knife table's boot flag SF[2] came 60 ms after the TRIG): the TRIG waits");
    CHECK(sc_trig_pending_step(0, 2, 500) == SC_TP_WAIT, "a waiting TRIG: the flag has not come yet, 0.5 s in");
    CHECK(sc_trig_pending_step(0, SC_SEQ_WAIT_TRIGGER, 560) == SC_TP_APPLY, "a waiting TRIG: the sequence reached state 3, the trigger goes through the stock function");
    CHECK(sc_trig_pending_step(1, 2, 300) == SC_TP_DROP, "a waiting TRIG: this PC's own story triggered it meanwhile, nothing more");
    CHECK(sc_trig_pending_step(0, 2, SC_TRIG_WAIT_MS) == SC_TP_EXPIRE, "a waiting TRIG after 2 s: the kept-out SCENE starts it locally as before");
    CHECK(sc_actor_allow(1, SC_SEQ_WAIT_UNITS, 1, 1), "the guest's own scene waits for its units and asks for the actor its enemy unit names (WakeUp: uEm1100 (100,116,0)): created here (its units waited 7 s without it)");
    CHECK(!sc_actor_allow(1, -1, 0, 0), "no scene here: the client guard refuses as before (the host's SPAWN creates it)");
    CHECK(!sc_actor_allow(0, SC_SEQ_WAIT_UNITS, 1, 1), "no co-op client: the guard is not involved");
    CHECK(sc_view_owned(1, 1), "a scene plays here and its director shows it through its player's viewport: the view guard and the role set-up leave the view alone (live sc032m)");
    CHECK(!sc_view_owned(1, 0), "no scene: the view guard keeps this PC's own human on screen as before");
    CHECK(!sc_view_owned(0, 1), "LAB `net scene view 0`: as before");
    CHECK(sc_trig_decide(1, 1, 0, 0, 1, 0x10) == SC_TRIG_STATE, "its sequence has ended (a one-shot scene that ran): no trigger");
    CHECK(sc_trig_decide(1, 0, 0, 0, 1, SC_SEQ_WAIT_TRIGGER) == SC_TRIG_NONE, "no scene set model of that key here: nothing");
    CHECK(sc_trig_decide(0, 1, 0, 0, 1, SC_SEQ_WAIT_TRIGGER) == SC_TRIG_NONE, "no co-op partner with scene sync: nothing");
}

/* 0.3.2 final regression r032fa: only the scene's own actor (the layout an enemy unit names, while the sequence waits for its units) */
static void actor032(void)
{
    sc_lay wake[1] = {{100, 116, 0}};   /* S100,105,0 (Episode 1 WakeUp): uDemoControllerEm00 names uEm1100 (100,116,0) */
    CHECK(sc_lay_named(wake, 1, 100, 116u | 0u << 16), "the WakeUp's enemy unit names (100,116,0)");
    CHECK(sc_actor_allow(1, SC_SEQ_WAIT_UNITS, 1, sc_lay_named(wake, 1, 100, 116u)), "WakeUp: its actor (100,116,0) in state 7 is created on the guest (live r032fs 15:56:07.476)");
    CHECK(!sc_lay_named(wake, 1, 100, 117u), "another b is not the actor");
    CHECK(!sc_lay_named(wake, 1, 101, 116u), "another stage is not the actor");
    CHECK(!sc_lay_named(wake, 1, 100, 116u | 1u << 16), "another c is not the actor");
    sc_lay none_b[1] = {{100, -1, 0}}, none_c[1] = {{100, 116, -1}};
    CHECK(!sc_lay_named(none_b, 1, 100, 0xFFFFu) && !sc_lay_named(none_c, 1, 100, 116u | 0xFFFFu << 16), "a unit without a target (b or c -1, the getter returns nothing) names no actor");
    sc_lay hi[1] = {{100, 0x0174, 0}};   /* the getter's key keeps only the low byte of b */
    CHECK(sc_lay_named(hi, 1, 100, 0x74u), "b is compared with the getter's key (low byte of b, c in the high half)");
    /* S600,115,0 (The Struggle's opening): no enemy unit at all; the stage's wildlife is made during its state 7, a snake (600,30,0) in state 8 */
    int snake = sc_lay_named(NULL, 0, 600, 30u | 0u << 16);
    CHECK(!snake, "S600,115,0 names no enemy");
    CHECK(!sc_actor_allow(1, 8, 1, snake), "the snake (600,30,0) asked in state 8 of S600,115,0 is not created on the guest (live r032fa: it lived there until the orphan rule killed it)");
    CHECK(!sc_actor_allow(1, SC_SEQ_WAIT_UNITS, 1, sc_lay_named(NULL, 0, 600, 1u | 20u << 16)), "a rabbit (600,1,20) asked in state 7 of a scene without an enemy unit: left to the guard (the host's SPAWN)");
    CHECK(!sc_actor_allow(1, -1, 1, 0), "a scene plays here without a watched scene set model: left to the guard (the demand rule stays)");
    CHECK(!sc_actor_allow(1, 6, 1, 1), "the actor's layout before the sequence waits for its units: not yet");
    CHECK(!sc_actor_allow(1, 8, 1, 1), "the actor's layout after all units are ready: not again");
}

static void clock_(void)
{
    sc_clock c;
    memset(&c, 0, sizeof c);
    sc_clock_sample(&c, 1000, 5050, 1100);
    CHECK(c.have && c.rtt == 100 && c.offset == 4000, "first sample: rtt %u offset %d", c.rtt, c.offset);
    sc_clock_sample(&c, 2000, 6200, 2300);
    CHECK(c.rtt == 100 && c.offset == 4000, "a slower sample is not kept");
    sc_clock_sample(&c, 3000, 7020, 3050);
    CHECK(c.rtt == 50 && c.offset == 3995, "a faster sample is kept: rtt %u offset %d", c.rtt, c.offset);
    CHECK(sc_gap(&c, 10000, 13995 + 5) == -5, "gap: this PC started 5 ms earlier (%d)", sc_gap(&c, 10000, 14000));
    CHECK(sc_gap(&c, 10500, 14000) == 495, "gap: this PC started 495 ms later (%d)", sc_gap(&c, 10500, 14000));
    sc_clock_sample(&c, 3000, 9000, 3000 + 20000);
    CHECK(c.rtt == 50, "an absurd round trip is ignored");
}

int main(void)
{
    barrier();
    vote();
    dedupe();
    scene_hold();
    door();
    gate();
    receive();
    trigger();
    actor032();
    clock_();
    printf("scene: %d checks, %d failed\n", n, bad);
    return bad != 0;
}
