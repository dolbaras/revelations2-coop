/* scene_logic.h: scene032, the pure part of the cutscene sync between the two PCs (notes\cutscene-sync.md, private), so tests/test_scene.c can replay it without a game.
 *
 * Facts it builds on (decompiled, build 3658995):
 *  - the stock "wait for every peer" points of the game's own network code: sync point kind 7 = the end of the stage loader (aGame state 8 sub 7 / 8, before play),
 *    kind 8 = a pre-rendered movie is prepared (sCutscene state 2). Outside a game-network session (always in this mod) the check passes at once.
 *  - the stock skip of movies and mini demos is a vote of every peer (0x9F4DF0 my bit, 0x9F3200 all bits); without a game session the mask is this PC alone.
 *    Player demos (sDemo type 3) skip through sDemo+0x1960 on one PC.
 *  - a second sDemo type-3 submit for a player replaces (restarts) the running scene; the requests hold per-PC pointers, so a byte hash differs between the PCs.
 *
 * Parts:
 *  sc_bar   the start barrier at kind 7 / kind 8: hold until the partner reports the same point (READY with the same key, received after this PC's own load began),
 *           or is in play (INPLAY), or is gone / has no scene sync (CAPS), or the limit (300 s for a load: the session's own load limit; 30 s for a movie)
 *  sc_vote  the skip vote of one scene: both players must press skip; a partner who is not there does not count
 *  sc_dd    one start per in-engine scene and PC: the key (the camera director's LayoutID) of every start; a received SCENE is skipped while this PC's own copy of the
 *           same key plays (or started less than SC_DD_RECENT_MS ago), this PC's own duplicate submit is suppressed while the partner's copy plays. An empty key
 *           (no LayoutID) turns the dedupe off; a different key never matches; a scene is only ever dropped while another copy of it plays.
 *  sc_demo_sm_apply   the door mirror moves a scene set model (uSmPlayerDemo / uSmOtherDemo) only forward (a backward step restarted a playing scene: g032r2g)
 *  sc_clock / sc_gap  the partner's clock offset from ping / pong (the sample with the smallest round trip), the start gap of one scene between the PCs
 *
 * Build the model of rel032 (before scene032) with -DSC_MODEL_REL032: tests/test_scene.c must FAIL with it. */
#ifndef SCENE_LOGIC_H
#define SCENE_LOGIC_H

#include <stdint.h>
#include <string.h>

#define SC_KIND_LOAD 7u
#define SC_KIND_MOVIE 8u
#define SC_LOAD_LIMIT_MS 300000u   /* = the session's load barrier limit (coop_session.c BARRIER_TIMEOUT_MS): the slower PC's load is what is awaited */
#define SC_MOVIE_LIMIT_MS 30000u   /* both PCs start the movie inside the same load; a PC without this movie (another file / language pack) is let go here */
#define SC_OLD_BARRIER_MS 6000u    /* rel032: the kind-8 wait, only while the glue ran */
#define SC_CAPS_WAIT_MS 10000u     /* no CAPS from the partner after this long at a hold: the partner has no scene sync (an older build, `net scene 0`) */
#define SC_INPLAY_MIN_MS 2000u     /* an INPLAY answer releases only after this long at the hold (the partner may begin the same load a moment later) */
#define SC_DD_WINDOW_MS 180000u    /* how long a start is remembered */
#define SC_DD_RECENT_MS 3000u      /* a start this recent counts as playing even before its slot shows it */
#define SC_VOTE_PEND_MS 15000u     /* a partner's vote that came before this PC's own start of the same scene */
#define SC_KEY_MAX 24

enum sc_release {
    SC_HOLD = 0,
    SC_REL_OFF,        /* the scene sync is off here (`net scene 0`), or (rel032 model) no barrier at this point */
    SC_REL_ALONE,      /* no partner connected (left, lost, never there) */
    SC_REL_NOCAPS,     /* the partner announced no scene sync, or announced nothing */
    SC_REL_MATCH,      /* the partner reached the same point (READY, same key) */
    SC_REL_INPLAY,     /* the partner is in play in the same area (it does not load: nothing to wait for) */
    SC_REL_ELSEWHERE,  /* the partner is in play somewhere else */
    SC_REL_LIMIT       /* the time limit */
};
static const char *const sc_rel_names[] = {"holding", "scene sync off", "no partner connected", "the partner has no scene sync", "the partner reached the same point",
                                           "the partner is in play in this area", "the partner is in play elsewhere", "time limit"};

static int sc_key_eq(const char *a, const char *b) { return a && b && a[0] && !strncmp(a, b, SC_KEY_MAX); }
static void sc_key_set(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n > SC_KEY_MAX - 1) n = SC_KEY_MAX - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---------------------------------------------------------------- the start barrier */
typedef struct {
    int have;
    char key[SC_KEY_MAX];
    uint32_t t;
} sc_report;

typedef struct {
    int caps;                    /* partner: 1 scene sync on, -1 off, 0 nothing heard */
    sc_report ready[2];          /* partner READY at kind 7 ([0]) / kind 8 ([1]) */
    sc_report inplay;            /* partner: "in play in area key" (an answer to a READY) */
    uint32_t p_loading_t;        /* partner: the last "my load began" */
    int holding;
    unsigned kind;
    char key[SC_KEY_MAX];
    uint32_t since;
    uint32_t load_t;             /* this PC: when its current load began */
    uint32_t movie_rel_t;        /* this PC: the release of its last movie start (or the session start) */
} sc_bar;

static void sc_bar_reset(sc_bar *b, uint32_t now)
{
    memset(b, 0, sizeof *b);
    b->load_t = b->movie_rel_t = now;
}
static int sc_kix(unsigned kind) { return kind == SC_KIND_MOVIE ? 1 : 0; }
static void sc_bar_load_begin(sc_bar *b, uint32_t now) { b->load_t = now; }
static void sc_bar_on_caps(sc_bar *b, int on) { b->caps = on ? 1 : -1; }
static void sc_bar_on_ready(sc_bar *b, unsigned kind, const char *key, uint32_t now)
{
    sc_report *r = &b->ready[sc_kix(kind)];
    r->have = 1, sc_key_set(r->key, key), r->t = now;
}
static void sc_bar_on_inplay(sc_bar *b, const char *key, uint32_t now) { b->inplay.have = 1, sc_key_set(b->inplay.key, key), b->inplay.t = now; }
static void sc_bar_on_partner_loading(sc_bar *b, uint32_t now) { b->p_loading_t = now, b->inplay.have = 0; }

/* this PC reached the point (kind, key): 1 = a new hold (send READY) */
static int sc_bar_arrive(sc_bar *b, unsigned kind, const char *key, uint32_t now)
{
    if (b->holding && b->kind == kind && sc_key_eq(b->key, key)) return 0;
    b->holding = 1, b->kind = kind, sc_key_set(b->key, key), b->since = now;
    return 1;
}

/* every check of the point: hold or the reason of the release. glue_running = the rel032 condition of its kind-8 wait (the model only) */
static int sc_bar_decide(const sc_bar *b, uint32_t now, int partner_connected, int scene_on, int glue_running)
{
    if (!b->holding) return SC_REL_OFF;
    uint32_t held = now - b->since;
#ifdef SC_MODEL_REL032
    (void)scene_on;
    if (b->kind != SC_KIND_MOVIE || !glue_running) return SC_REL_OFF;   /* rel032: kind 8 only, only while the glue ran */
    if (!partner_connected) return SC_REL_ALONE;
    if (b->ready[1].have) return SC_REL_MATCH;                            /* any arrival of the other PC, no key */
    return held >= SC_OLD_BARRIER_MS ? SC_REL_LIMIT : SC_HOLD;
#else
    (void)glue_running;
    if (!scene_on) return SC_REL_OFF;
    if (!partner_connected) return SC_REL_ALONE;
    if (b->caps < 0 || (b->caps == 0 && held >= SC_CAPS_WAIT_MS)) return SC_REL_NOCAPS;
    const sc_report *r = &b->ready[sc_kix(b->kind)];
    uint32_t fresh = b->kind == SC_KIND_LOAD ? b->load_t : b->movie_rel_t;   /* a loader-end READY from before this PC's own load began is stale (a retry of the same
                                                                             area); a movie READY only from before this PC's last movie (this PC may notice its load late) */
    if (r->have && sc_key_eq(r->key, b->key) && (int32_t)(r->t - fresh) >= 0) return SC_REL_MATCH;
    if (b->inplay.have && (int32_t)(b->inplay.t - b->since) >= 0 && (int32_t)(b->inplay.t - b->p_loading_t) >= 0 && held >= SC_INPLAY_MIN_MS)
        return b->kind == SC_KIND_LOAD && sc_key_eq(b->inplay.key, b->key) ? SC_REL_INPLAY : SC_REL_ELSEWHERE;
    return held >= (b->kind == SC_KIND_LOAD ? SC_LOAD_LIMIT_MS : SC_MOVIE_LIMIT_MS) ? SC_REL_LIMIT : SC_HOLD;
#endif
}

/* the release happened: the partner's report is used up (it must not open the next point) */
static void sc_bar_release(sc_bar *b, uint32_t now)
{
    if (b->holding) b->ready[sc_kix(b->kind)].have = 0;
    if (b->holding && b->kind == SC_KIND_MOVIE) b->movie_rel_t = now;
    b->holding = 0, b->inplay.have = 0;
}

/* the partner sent READY (kind, key) and this PC does not hold: answer INPLAY when this PC has been in play (not loading) for a while */
static int sc_should_answer_inplay(int in_play, uint32_t in_play_ms, int holding) { return !holding && in_play && in_play_ms >= 5000u; }

/* ---------------------------------------------------------------- the skip vote */
enum { SC_STRIP_NONE = 0, SC_STRIP_WAIT = 1, SC_STRIP_PARTNER = 2 };

typedef struct {
    int active;
    char key[SC_KEY_MAX];
    int mine, theirs;
    int pend;                    /* the partner's vote for a scene that has not begun here yet */
    char pend_key[SC_KEY_MAX];
    uint32_t pend_t;
} sc_vote;

static void sc_vote_begin(sc_vote *v, const char *key, uint32_t now)
{
    if (v->active && sc_key_eq(v->key, key)) return;
    v->active = 1, sc_key_set(v->key, key), v->mine = v->theirs = 0;
    if (v->pend && sc_key_eq(v->pend_key, key) && now - v->pend_t < SC_VOTE_PEND_MS) v->theirs = 1;
    v->pend = 0;
}
static void sc_vote_end(sc_vote *v) { v->active = 0, v->mine = v->theirs = 0, v->key[0] = 0; }
/* this player pressed skip: 1 = a new vote (send it) */
static int sc_vote_mine(sc_vote *v)
{
    if (!v->active || v->mine) return 0;
    v->mine = 1;
    return 1;
}
static void sc_vote_theirs(sc_vote *v, const char *key, uint32_t now)
{
    if (v->active && sc_key_eq(v->key, key)) v->theirs = 1;
    else v->pend = 1, sc_key_set(v->pend_key, key), v->pend_t = now;
}
/* skip now? A partner who is not there (left, lost, no scene sync) does not count: then this player's vote alone skips, as in the stock game */
static int sc_vote_skip(const sc_vote *v, int partner_present)
{
#ifdef SC_MODEL_REL032
    (void)partner_present;
    return v->active && v->mine;   /* the stock vote without a game session: this PC alone */
#else
    if (!v->active || !v->mine) return 0;
    return partner_present ? v->theirs : 1;
#endif
}
static int sc_vote_strip(const sc_vote *v, int partner_present)
{
    if (!v->active || !partner_present) return SC_STRIP_NONE;
    if (v->mine && !v->theirs) return SC_STRIP_WAIT;
    if (v->theirs && !v->mine) return SC_STRIP_PARTNER;
    return SC_STRIP_NONE;
}

/* ---------------------------------------------------------------- one start per in-engine scene and PC */
enum { SC_FROM_OWN = 1, SC_FROM_PARTNER = 2 };
#define SC_DD_N 16
typedef struct {
    char key[SC_KEY_MAX];
    int origin;
    uint32_t serial;             /* the sDemo serial of that start on this PC */
    uint32_t t;
} sc_dd_ent;
typedef struct {
    sc_dd_ent e[SC_DD_N];
    unsigned next;
    unsigned suppressed, skipped;
} sc_dd;

static void sc_dd_note(sc_dd *d, const char *key, int origin, uint32_t serial, uint32_t now)
{
    if (!key || !key[0]) return;
    sc_dd_ent *x = &d->e[d->next++ % SC_DD_N];
    sc_key_set(x->key, key), x->origin = origin, x->serial = serial, x->t = now ? now : 1u;
}
/* the newest start of this key from origin, -1 when none (in the window) */
static int sc_dd_find(const sc_dd *d, const char *key, int origin, uint32_t now)
{
    int best = -1;
    for (int i = 0; i < SC_DD_N; i++) {
        const sc_dd_ent *x = &d->e[i];
        if (!x->t || x->origin != origin || !sc_key_eq(x->key, key) || now - x->t > SC_DD_WINDOW_MS) continue;
        if (best < 0 || (int32_t)(x->t - d->e[best].t) > 0) best = i;
    }
    return best;
}
/* this PC's own submit of key: suppress it (the partner's copy of the same scene plays here: no restart)? playing = the slot of that start is still running */
static int sc_dd_own_suppress(const sc_dd *d, const char *key, uint32_t now, int (*playing)(uint32_t serial))
{
#ifdef SC_MODEL_REL032
    (void)d, (void)key, (void)now, (void)playing;
    return 0;
#else
    if (!key || !key[0]) return 0;
    int i = sc_dd_find(d, key, SC_FROM_PARTNER, now);
    return i >= 0 && playing && playing(d->e[i].serial);
#endif
}
/* the partner's SCENE of key: skip it (this PC's own copy of the same scene plays, or just started)? */
static int sc_dd_partner_skip(const sc_dd *d, const char *key, uint32_t now, int (*playing)(uint32_t serial))
{
#ifdef SC_MODEL_REL032
    (void)d, (void)key, (void)now, (void)playing;
    return 0;   /* rel032: the byte hash of the other PC's request never equals this PC's (per-PC pointers) */
#else
    if (!key || !key[0]) return 0;
    int i = sc_dd_find(d, key, SC_FROM_OWN, now);
    if (i < 0) return 0;
    return now - d->e[i].t < SC_DD_RECENT_MS || (playing && playing(d->e[i].serial));
#endif
}

/* the PC whose own submit started an in-engine scene holds the scene's slot start (sDemo 0xA9D530 "not ready yet") until the partner's copy has started (its STAMP),
 * at most SC_SCENE_HOLD_MS: right after a load the slower PC took ~0.44 s to apply the SCENE (live sc032b 02:44:26); with the hold both start within the event latency */
#define SC_SCENE_HOLD_MS 1000u
static int sc_scene_hold(int coop, int key_valid, int own_recent, int partner_started, uint32_t held_ms)
{
#ifdef SC_MODEL_REL032
    (void)coop, (void)key_valid, (void)own_recent, (void)partner_started, (void)held_ms;
    return 0;
#else
    if (!coop || !key_valid || !own_recent || partner_started) return 0;
    return held_ms < SC_SCENE_HOLD_MS;
#endif
}

/* the gate of the first in-engine scene after a load (live sc032c: each PC's story starts it after its own work past the synced loader end, 3.5-6.6 s apart): the camera
 * director's submit (FUN_0075D3A0, called every frame by its action FUN_0075CFB0 in state 2 until it returns true) waits until the partner's story reached the same
 * scene (its SREADY of the key, re-sent every second while it waits), at most SC_GATE_LIMIT_MS; only a director that submits at its frame 0 (+0x4204 == 0: the
 * other call ignores the result), only the first scene after a load, never without a partner that has the gate */
#define SC_GATE_LIMIT_MS 15000u
#define SC_GATE_FRESH_MS 3000u    /* a waiting PC re-sends its SREADY every second */
#define SC_GATE_WINDOW_MS 30000u  /* only a scene submitted within 30 s after the load (the loader end, or the glue's set-up of a retry) is gated (lab: 7.8-20.4 s) */
enum { SC_GATE_WAIT = 0, SC_GATE_PASS, SC_GATE_MATCH, SC_GATE_STARTED, SC_GATE_ALONE, SC_GATE_NOGATE, SC_GATE_LIMIT, SC_GATE_N };
typedef struct {
    int armed, waiting, phave;
    char key[SC_KEY_MAX], pkey[SC_KEY_MAX];
    uint32_t t0, pt;
} sc_gate;
static void sc_gate_arm(sc_gate *g) { g->armed = 1, g->waiting = 0, g->phave = 0; }   /* a load began: the partner's old SREADY is void (it re-sends while it waits) */
static void sc_gate_disarm(sc_gate *g) { g->armed = 0, g->waiting = 0; }
static void sc_gate_on_partner(sc_gate *g, const char *key, uint32_t now) { g->phave = 1, sc_key_set(g->pkey, key), g->pt = now; }
/* SC_GATE_WAIT = no submit this frame; SC_GATE_PASS = not gated (submit as the stock does); the other values release the gate (the caller disarms it and submits) */
static int sc_gate_decide(sc_gate *g, const char *key, uint32_t now, int submit_at_start, int partner_present, int partner_has_gate, int partner_copy_started)
{
#ifdef SC_MODEL_REL032
    (void)g, (void)key, (void)now, (void)submit_at_start, (void)partner_present, (void)partner_has_gate, (void)partner_copy_started;
    return SC_GATE_PASS;
#else
    if (!g->armed || !key || !key[0] || !submit_at_start) return SC_GATE_PASS;
    if (!partner_present) return SC_GATE_ALONE;
    if (!partner_has_gate) return SC_GATE_NOGATE;
    if (partner_copy_started) return SC_GATE_STARTED;
    if (!g->waiting || !sc_key_eq(g->key, key)) g->waiting = 1, sc_key_set(g->key, key), g->t0 = now;
    if (g->phave && sc_key_eq(g->pkey, key) && now - g->pt < SC_GATE_FRESH_MS) return SC_GATE_MATCH;
    if (now - g->t0 >= SC_GATE_LIMIT_MS) return SC_GATE_LIMIT;
    return SC_GATE_WAIT;
#endif
}
/* a received SCENE (complete, with the key its SKEY named): live sc032f, the request carries the sender's director pointers (+0x18, +0x20, +0x70), its copy ran
 * without a director here (camera frame -1) and the guest kept playing; the guest's own director of that scene never started (the scene set model's play action
 * FUN_007750E0 waits in state 0 for the interaction's flag, its state 2 starts the directors through FUN_00773EA0). So: a scene set model of that key on this PC
 * starts its own directors (FUN_00773EA0, local objects only) unless it runs already or this is the first scene after a load (both stories start that one) */
enum { SC_RX_APPLY = 0, SC_RX_KEEP_OUT, SC_RX_START_LOCAL };
static int sc_rx_decide(int coop, int has_local_sm, int local_started, int first_scene_window, int trig_pending)
{
#ifdef SC_MODEL_REL032
    (void)coop, (void)has_local_sm, (void)local_started, (void)first_scene_window, (void)trig_pending;
    return SC_RX_APPLY;
#else
    if (!coop || !has_local_sm) return SC_RX_APPLY;   /* no scene set model of that key here: the bytes as before */
    if (local_started || first_scene_window) return SC_RX_KEEP_OUT;
    if (trig_pending) return SC_RX_KEEP_OUT;   /* the partner's TRIG waits for this PC's own sequence (sc_trig_pending_step): it starts the scene */
    return SC_RX_START_LOCAL;
#endif
}

/* the partner's TRIG: its scene set model of that key was triggered there (the stock trigger FUN_00773F40: a press, a zone or a flag; the player who triggered it).
 * Live sc032g: the guest's directors started from the received SCENE (FUN_00773EA0 alone) ran with the camera frame at 0.0 for 4 s while the host's advanced: the stock
 * sequence FUN_00778690 of the scene set model (fade, HUD, scheduler frame reset, +0x45AD) and the Move action's state 2 (FUN_007781D0 before FUN_00773EA0) never ran
 * there. So this PC triggers its OWN scene set model of that key through the same stock trigger with the partner's puppet slot as the player, at the moment the
 * partner's was triggered: both stock sequences run from there (local objects only). Not when: no scene set model of that key here, this PC's own one was triggered
 * already (or ran), the first scene after a load (each PC's own story starts it), no player object of that slot here, or its sequence is not waiting for a trigger
 * (state 3); the received SCENE then decides (sc_rx_decide) */
enum { SC_TRIG_APPLY = 0, SC_TRIG_NONE, SC_TRIG_HAVE, SC_TRIG_WINDOW, SC_TRIG_NOPLAYER, SC_TRIG_STATE, SC_TRIG_WAIT, SC_TRIG_N };
enum { SC_SEQ_WAIT_TRIGGER = 3 };
#define SC_TRIG_WAIT_MS 2000u
static int sc_trig_decide(int coop, int has_local_sm, int triggered_here, int first_scene_window, int player_here, int seq_state)
{
#ifdef SC_MODEL_REL032
    (void)coop, (void)has_local_sm, (void)triggered_here, (void)first_scene_window, (void)player_here, (void)seq_state;
    return SC_TRIG_NONE;   /* rel032 has no TRIG: the guest learns of the scene only from the SCENE bytes */
#else
    if (!coop || !has_local_sm) return SC_TRIG_NONE;
    if (triggered_here) return SC_TRIG_HAVE;
    if (first_scene_window) return SC_TRIG_WINDOW;
    if (!player_here) return SC_TRIG_NOPLAYER;
    if (seq_state >= 0 && seq_state < SC_SEQ_WAIT_TRIGGER) return SC_TRIG_WAIT;   /* still preparing (live sc032h: the knife table boots on SF[2], which reached the guest
                                                                                       60 ms after the TRIG; its sequence was in state 2): wait for state 3 */
    if (seq_state != SC_SEQ_WAIT_TRIGGER) return SC_TRIG_STATE;
    return SC_TRIG_APPLY;
#endif
}
/* a waiting TRIG, every frame: its own story triggered the set model meanwhile (drop), the sequence reached state 3 (apply), or SC_TRIG_WAIT_MS passed (expire: the
 * partner's SCENE, kept out meanwhile, starts it locally as before) */
enum { SC_TP_WAIT = 0, SC_TP_APPLY, SC_TP_DROP, SC_TP_EXPIRE };
static int sc_trig_pending_step(int triggered_here, int seq_state, uint32_t elapsed)
{
#ifdef SC_MODEL_REL032
    (void)triggered_here, (void)seq_state, (void)elapsed;
    return SC_TP_DROP;
#else
    if (triggered_here) return SC_TP_DROP;
    if (seq_state == SC_SEQ_WAIT_TRIGGER) return SC_TP_APPLY;
    if (elapsed >= SC_TRIG_WAIT_MS) return SC_TP_EXPIRE;
    return SC_TP_WAIT;
#endif
}
/* an in-engine scene plays on this PC (a type-3 sDemo slot in state 1 / 2): its director shows the scene through the viewport of the scene's player (FUN_0075D8D0 /
 * GameMan_SetSlot, the slot start hands that viewport the scene camera). Live sc032m: on the guest the mod's view guard put viewport 0 back on its own human 1 s into the
 * host's knife table scene (the guest then saw its own human's gameplay camera: another angle), and the role set-up after the load (join, types, `vp 0 1`) did the same in
 * the middle of the guest's own WakeUp. While a scene plays the view guard and the role set-up leave the view alone (the guard restores it after the scene) */
static int sc_view_owned(int on, int scene_playing)
{
#ifdef SC_MODEL_REL032
    (void)on, (void)scene_playing;
    return 0;
#else
    return on && scene_playing;
#endif
}

/* a client's own story asks for an enemy while this PC's own in-engine scene waits for its units: the scene's actor. Live sc032h: the guest's own WakeUp asked for uEm1100
 * (100,116,0) while its sequence waited for its units (state 7); the client guard refused it (the host's SPAWN had come while the guest's glue was still setting up), the
 * scene's units waited 7 s for it and the scene's camera (its scheduler) stood still until the replica came from a host record. Created here through the stock path; the
 * host's record / SPAWN finds it by its layout id (world.inc w_find_active).
 * 0.3.2 final regression (r032fa): the first form of the rule let EVERY creation through while a scene was being started (sequence states 4..8) or any scene played. At The
 * Struggle's opening (S600,115,0, a scene without an enemy unit) the client made the stage's wildlife itself, and a snake (600,30,0) that the host's PC never made (state 8)
 * lived on the guest's PC until the orphan rule killed it 10 s after the scene. Now only the scene's own actor: a layout that one of the watched scene set model's enemy units
 * names (uDemoControllerEm*, Control::Enemy +0x42B0 = 3; its target getter vf90 0x76A460 looks the enemy up by LayoutID (+0x42BC, +0x42C0, +0x42C4) through FUN_00A94550 with
 * the key (c << 16 | b) & 0xFFFF00FF, nothing when b or c is -1), and only while the sequence waits for its units (state 7: all 7 live actor requests came there, the snake in 8).
 * -DSC_MODEL_ACTOR_ANY = the first form (tests/test_scene.c must fail with it). */
#define SC_SEQ_WAIT_UNITS 7
typedef struct { int a, b, c; } sc_lay;   /* the target LayoutID an enemy unit names */
/* the CreateEnemy layout (a, bc = b | c << 16) is one that a unit names, with the getter's key */
static int sc_lay_named(const sc_lay *t, int n, int a, unsigned bc)
{
    for (int i = 0; i < n; i++) {
        if (t[i].b == -1 || t[i].c == -1) continue;
        unsigned key = ((unsigned)(t[i].c & 0xFFFF) << 16 | (unsigned)(t[i].b & 0xFFFF)) & 0xFFFF00FFu;
        if (t[i].a == a && (bc & 0xFFFF00FFu) == key) return 1;
    }
    return 0;
}
/* coop_client: a co-op client whose guard would refuse the creation; seq: the sequence state of this PC's own watched scene set model (-1 none); scene_playing: a type-3 scene
 * plays here; named: the layout is one of that set model's enemy units' targets */
static int sc_actor_allow(int coop_client, int seq, int scene_playing, int named)
{
#ifdef SC_MODEL_REL032
    (void)coop_client, (void)seq, (void)scene_playing, (void)named;
    return 0;
#elif defined(SC_MODEL_ACTOR_ANY)
    (void)named;
    return coop_client && ((seq >= 4 && seq <= 8) || scene_playing);
#else
    (void)scene_playing;
    return coop_client && seq == SC_SEQ_WAIT_UNITS && named;
#endif
}
/* the local start from a received SCENE (no TRIG took here): the Move action's state 2 calls FUN_007781D0 (scheduler +0xC |= 0x800 | 0x400) and THEN FUN_00773EA0;
 * fix C called only the second (live sc032g: camera frame 0.0 for 4 s). 1 = the scheduler call comes first */
static int sc_local_start_runs_scheduler(void)
{
#ifdef SC_MODEL_REL032
    return 0;
#else
    return 1;
#endif
}

/* the partner's SCENE while the gate is armed is not submitted here: this PC's own story starts the first scene after the load (live sc032e: the received copy ran
 * without a director here, camera frame -1 and the players outside the scene, and this PC's own submit 3.5 s later started the scene a second time) */
static int sc_gate_keeps_own(const sc_gate *g, uint32_t now, uint32_t arm_t, int partner_has_gate)
{
#ifdef SC_MODEL_REL032
    (void)g, (void)now, (void)arm_t, (void)partner_has_gate;
    return 0;
#else
    return g->armed && partner_has_gate && now - arm_t < SC_GATE_WINDOW_MS;
#endif
}

/* ---------------------------------------------------------------- the door mirror and scene set models */
/* apply the partner's action `asked` to a scene set model whose action here is `local`? Forward only (0 wait, 1 play, 2 done); a negative local id (no action) as before.
 * Not Wait -> Move while this PC's own set model is not triggered yet: its own sequence makes that step after the trigger, with the trigger's player (live sc032n: the
 * partner's DOOR 0 -> 1 came 17 ms before this PC's TRIG took, the Move action started with player -1, FUN_00773EA0(-1), and both characters stood in their bind pose
 * through the whole scene on the guest) */
static int sc_demo_sm_apply(int is_scene_sm, int local, int asked, int triggered_here)
{
#ifdef SC_MODEL_REL032
    (void)is_scene_sm, (void)local, (void)asked, (void)triggered_here;
    return 1;
#else
    if (!is_scene_sm || local < 0 || asked < 0) return 1;
    if (local == 0 && asked == 1 && !triggered_here) return 0;
    return asked > local;
#endif
}

/* ---------------------------------------------------------------- clock offset and start gaps */
typedef struct {
    int have;
    int32_t offset;              /* partner clock - my clock (ms) */
    uint32_t rtt;
    uint32_t t;                  /* when the kept sample was taken (my clock) */
    unsigned samples;
} sc_clock;

/* a PONG: t1 = my send time, t2 = the partner's clock when it answered, t3 = my receive time */
static void sc_clock_sample(sc_clock *c, uint32_t t1, uint32_t t2, uint32_t t3)
{
    uint32_t rtt = t3 - t1;
    if (rtt > 10000u) return;
    c->samples++;
    if (c->have && rtt > c->rtt && t3 - c->t < 60000u) return;   /* keep the smallest round trip of the last minute */
    c->have = 1, c->rtt = rtt, c->t = t3;
    c->offset = (int32_t)(t2 - (t1 + rtt / 2u));
}
/* the start gap: + = this PC started later than the partner (ms) */
static int32_t sc_gap(const sc_clock *c, uint32_t my_t, uint32_t partner_t) { return (int32_t)(my_t - (partner_t - (uint32_t)c->offset)); }

#endif
