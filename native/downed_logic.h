/* downed_logic.h: the pure state machine of the "downed / revive" rule (RE5 / RE6 "agony") for the co-op mod. No engine calls, no Windows, no network: tests/test_downed.c replays it, native/downed.inc
 * (engine glue) feeds it and executes what it asks for. Design, wire events and live checks: docs/downed-revive.md.
 *
 * The rule (switchable, session flag CP_SESS_DOWNED): when a human's health would reach 0 and at least one OTHER human is still up (ALIVE), he goes DOWN instead of dying: hp stays 1, he lies on the floor
 * (the game's own wounded chain: dying_start -> wait_rescue), cannot act, and BLEEDS OUT on a timer (default 60 s). A partner who is up and holds the interact button next to him for the revive time raises him
 * (hp restored by the game's own completion). Game over (the death screen, retry from the checkpoint) when a human DIES (the timer ran out, a void fall, a finisher the rule cannot soften) or when no human
 * is up any more (everybody DOWN).
 *
 * Authority (network): every PC is the authority for ITS OWN human: it decides DOWN / DEAD / bleed-out for him, and it is the authority for the hold of ITS human on the partner (the progress is computed where
 * the input is). Everything else is a MIRROR driven by reliable events (DOWN, DEAD, BEGIN, KEEPALIVE, CANCEL, DONE) with the unreliable STATE flags as a debounced second opinion. Each PC evaluates the game-over
 * condition on its own mirror; the condition is stable (nobody can get up from "all down"), so both PCs reach it without arbitration. The one race that exists - both humans take their lethal hit within one
 * round trip - ends in "both DOWN" on both PCs, i.e. a game over on both, which is the right answer.
 * All times are milliseconds of a monotonic local clock (wrap-safe differences). */
#ifndef DOWNED_LOGIC_H
#define DOWNED_LOGIC_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DL_MAX 4                       /* the protocol knows four players, the game two */
#define DL_BLEED_DEFAULT_MS 60000u     /* agony before the bleed-out (0 in the config = never) */
#define DL_REVIVE_DEFAULT_MS 3000u     /* hold time of the mod-driven revive (the engine's own is g_sPlayer+0xCE4, read live) */
#define DL_KEEPALIVE_MS 500u           /* the reviver repeats "still holding" this often */
#define DL_WATCHDOG_MS 2500u           /* the downed side: no BEGIN / KEEPALIVE from the reviver for this long = the revive was cancelled (reviver gone, link stalled) */
#define DL_REMOTE_GRACE_MS 1500u       /* a remote human's predicted bleed-out is believed this long after the timer ran out without the DEAD event */
#define DL_TRUST_MS 600u               /* the unreliable STATE disagrees with the mirror for this long: it wins */
#define DL_QMAX 24

enum dl_state { DL_ALIVE = 0, DL_DOWN = 1, DL_DEAD = 2, DL_GONE = 3 /* not in the session (left / never was) */ };

/* what the glue must do (outputs) */
enum dl_out_kind {
    DL_O_NONE = 0,
    DL_O_GO_DOWN,        /* who = me: clamp hp to 1 and start the engine's dying chain (ChangeAction 0x79); bleed timer runs */
    DL_O_DIE,            /* who = me: let the human die now (hp := 0): the engine's game over, the existing partner coupling does the rest */
    DL_O_GET_UP,         /* who = me: revived (or auto-revived): the engine's completion (hp restore, get-up motion) */
    DL_O_REVIVE_PLAY,    /* who = me: somebody holds the button on me: the be_rescued pose (0x7D) */
    DL_O_REVIVE_STOP,    /* who = me: the hold ended without success: back to lying (0x7A) */
    DL_O_PUPPET_DOWN,    /* who = remote: show him lying (puppet ChangeAction 0x7A) so the stock rescue prompt can appear */
    DL_O_PUPPET_UP,      /* who = remote: he stood up again (puppet back to the normal drive) */
    DL_O_SEND_DOWN,      /* events to the partner: */
    DL_O_SEND_DEAD,
    DL_O_SEND_BEGIN,     /* arg = target human */
    DL_O_SEND_KEEPALIVE, /* arg = target human */
    DL_O_SEND_CANCEL,    /* arg = target human */
    DL_O_SEND_DONE,      /* arg = target human: I raised him */
    DL_O_GAME_OVER,      /* informational: the rule decided the run is over (followed by DL_O_DIE for me when I am still down or up) */
    DL_O_HOLD_RESET      /* who = me: my own hold ended (the glue clears its input latch / the engine's gauge) */
};

typedef struct { int kind, who, arg; } dl_out;

/* inputs from the other PC (events) */
enum dl_event_kind { DL_E_DOWN = 1, DL_E_DEAD, DL_E_BEGIN, DL_E_KEEPALIVE, DL_E_CANCEL, DL_E_DONE };

typedef struct {
    int enabled;               /* the session switch */
    uint32_t bleed_ms;         /* 0 = never bleeds out */
    uint32_t revive_ms;        /* mod-driven hold time (dl_local_hold) */
    int get_up_on_leave;       /* the partner left while I lie: stand up (otherwise I would lie for ever) */
} dl_cfg;

typedef struct {
    int state;
    uint32_t down_at, last_seen;
    int32_t bleed_left;        /* ms; frozen while somebody holds the button */
    int reviver;               /* human that holds the button on this one (as far as I know), -1 */
    uint32_t revive_last;      /* time of the last BEGIN / KEEPALIVE */
    int hold_target;           /* me only: the human whose revive I am holding, -1 */
    uint32_t hold_ms, hold_ka, hold_t; /* progress of my hold, time of my last keep-alive, time of the last hold sample */
    uint32_t mismatch_since;   /* STATE (unreliable) disagrees since */
    int mismatch_state;
    uint32_t up_at;            /* me only: when a revive / get-up made me UP (0 = never): no new engine down for DL_UP_GRACE_MS (dl_follow) */
} dl_human;

typedef struct {
    dl_cfg cfg;
    int me, n;
    dl_human h[DL_MAX];
    dl_out q[DL_QMAX];
    int nq;
    uint32_t last_tick;
    int over;                  /* the game-over decision was made (once per run) */
    uint32_t n_down, n_revive, n_bleed, n_over, n_dropped, n_stale_fix;
} dl_t;

static inline int32_t dl_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static inline void dl_push(dl_t *d, int kind, int who, int arg)
{
    if (d->nq >= DL_QMAX) {
        d->n_dropped++;
        return;
    }
    d->q[d->nq].kind = kind, d->q[d->nq].who = who, d->q[d->nq].arg = arg, d->nq++;
}

/* the glue empties the queue every pass; returns 1 and fills *o while something is left */
static inline int dl_take(dl_t *d, dl_out *o)
{
    if (!d->nq) return 0;
    *o = d->q[0];
    d->nq--;
    memmove(d->q, d->q + 1, (size_t)d->nq * sizeof d->q[0]);
    return 1;
}

static inline void dl_human_reset(dl_human *h, int state, uint32_t now)
{
    memset(h, 0, sizeof *h);
    h->state = state, h->reviver = -1, h->hold_target = -1, h->last_seen = now;
}

/* n humans in the session (2 today), me = my index (the slot of my character) */
static inline void dl_init(dl_t *d, const dl_cfg *cfg, int me, int n, uint32_t now)
{
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    if (n < 1) n = 1;
    if (n > DL_MAX) n = DL_MAX;
    d->me = me, d->n = n, d->last_tick = now;
    for (int i = 0; i < DL_MAX; i++) dl_human_reset(&d->h[i], i < n ? DL_ALIVE : DL_GONE, now);
}

/* a level load / retry: everybody is up again */
static inline void dl_reset(dl_t *d, uint32_t now)
{
    for (int i = 0; i < DL_MAX; i++)
        if (d->h[i].state != DL_GONE) dl_human_reset(&d->h[i], DL_ALIVE, now);
    d->nq = 0, d->over = 0, d->last_tick = now;
}

/* ---- queries ---- */
static inline int dl_state(const dl_t *d, int i) { return i >= 0 && i < DL_MAX ? d->h[i].state : DL_GONE; }
static inline int dl_present(const dl_t *d, int i) { return dl_state(d, i) != DL_GONE; }
static inline int dl_count_up(const dl_t *d)
{
    int c = 0;
    for (int i = 0; i < DL_MAX; i++) c += d->h[i].state == DL_ALIVE;
    return c;
}
static inline int dl_count_present(const dl_t *d)
{
    int c = 0;
    for (int i = 0; i < DL_MAX; i++) c += d->h[i].state != DL_GONE;
    return c;
}
static inline int dl_others_up(const dl_t *d, int i)
{
    int c = 0;
    for (int k = 0; k < DL_MAX; k++) c += k != i && d->h[k].state == DL_ALIVE;
    return c;
}
static inline int32_t dl_bleed_left(const dl_t *d, int i) { return i >= 0 && i < DL_MAX && d->h[i].state == DL_DOWN ? d->h[i].bleed_left : 0; }
static inline int dl_being_revived(const dl_t *d, int i) { return i >= 0 && i < DL_MAX && d->h[i].state == DL_DOWN && d->h[i].reviver >= 0; }

/* ---- the game-over decision: any human dead, or nobody up (with at least two humans in the session) ---- */
static inline void dl_check_over(dl_t *d)
{
    if (d->over || !d->cfg.enabled) return;
    int dead = 0, up = 0, down = 0;
    for (int i = 0; i < DL_MAX; i++) {
        dead += d->h[i].state == DL_DEAD;
        up += d->h[i].state == DL_ALIVE;
        down += d->h[i].state == DL_DOWN;
    }
    if (dead || (up == 0 && down > 0)) {
        d->over = 1, d->n_over++;
        dl_push(d, DL_O_GAME_OVER, d->me, dead);
        if (d->h[d->me].state == DL_DOWN || d->h[d->me].state == DL_ALIVE) {
            if (d->h[d->me].state == DL_DOWN) {
                d->h[d->me].state = DL_DEAD;
                dl_push(d, DL_O_SEND_DEAD, d->me, 0);
            }
            dl_push(d, DL_O_DIE, d->me, 0);
        }
    }
}

/* ---- my own human ---- */

/* The local human would die from this hit (hp would reach 0). Returns 1 = absorb it: clamp hp to 1, the human is DOWN now; 0 = let him die (the rule is off, nobody is up to raise him, he is down already
 * and the glue's clamp failed). A hit on a human who is DOWN already is absorbed too (he is at hp 1, the clamp keeps him there; hits do not change the timer). */
static inline int dl_local_lethal(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (!d->cfg.enabled) return 0;
    if (m->state == DL_DOWN) return 1;
    if (m->state != DL_ALIVE) return 0;
    if (dl_others_up(d, d->me) == 0) return 0;   /* nobody to raise me: an ordinary death (also the single-human game) */
    m->state = DL_DOWN, m->down_at = now, m->bleed_left = d->cfg.bleed_ms ? (int32_t)d->cfg.bleed_ms : 0x7FFFFFFF, m->reviver = -1, m->revive_last = 0;
    if (m->hold_target >= 0) {   /* I was holding a revive: it ends */
        dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target), dl_push(d, DL_O_HOLD_RESET, d->me, 0);
        m->hold_target = -1, m->hold_ms = 0;
    }
    d->n_down++;
    dl_push(d, DL_O_GO_DOWN, d->me, 0);
    dl_push(d, DL_O_SEND_DOWN, d->me, 0);
    return 1;
}

/* my human is dead by a path the rule does not soften (the engine says so): hp 0 for real */
static inline void dl_local_dead(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->state == DL_DEAD || m->state == DL_GONE) return;
    m->state = DL_DEAD, m->last_seen = now;
    if (m->hold_target >= 0) {
        dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
        m->hold_target = -1, m->hold_ms = 0;
    }
    dl_push(d, DL_O_SEND_DEAD, d->me, 0);
    dl_check_over(d);
}

/* my human is standing at full control again with hp > 1 although the mirror says DOWN (the engine's own rescue completed, a heal, a console command): follow the engine */
static inline void dl_local_up(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->state != DL_DOWN) return;
    int by = m->reviver;
    dl_human_reset(m, DL_ALIVE, now);
    m->up_at = now ? now : 1;
    d->n_revive++;
    dl_push(d, DL_O_SEND_DONE, d->me, d->me);   /* "I am up": the partner's mirror of me (arg = me) */
    (void)by;
}

/* ---- the glue's per-pass follow-up of MY human (downed_frame step 1), pure so that the order of a revive can be tested ----
 * 0x79 dying_start and 0x7A / 0x7B wait_rescue are the engine's wounded chain: entering it while I am UP is a new down (the support role's finishers). 0x7D be_rescued
 * belongs to the lying family (no "left the chain" fix while it runs) but is never an entry: it runs only while somebody raises me. Run C 2026-11-07 02:14 (copy 2): the
 * DONE event made me UP, the same pass still saw 0x7D at hp 1 (the completion runs after this step) and counted a new down: GO DOWN, completion, "back to 0x7A", BEGIN,
 * DONE ... 53 times. Now: no new down from 0x7D, and none for DL_UP_GRACE_MS after a revive made me UP (the get-up motion; the stock grace +0xCF0 is 3 s too).
 * Times of 0 mean "never" (a clock past 2^31 ms would make (now - 0) negative and block the checks). */
#define DL_UP_GRACE_MS 3000u
/* Run C on rev2-int 2026-11-07 06:53 (`work\runs\20261107-063859-downedC`, copy 2 log 379-395): the host's ENGINE hold on the puppet ended without success and its mod hold
 * began in the same pass, so copy 2 got CANCEL + BEGIN together: "back to lying (0x7A)" and "being raised (0x7D)" in one pass restarted be_rescued and the engine cleared
 * +0x7BD8 (the rescuer) 70 ms later. At DONE the completion FUN_0070CCC0 (needs +0x7BD8 != 0) did not stand her up: the rule set hp 245 (35 %), she stayed in 0x7D, the
 * engine ended it 7 s later into 0x7A: lying with hp > 1 while the rule says UP ("C1 round 2: NOT raised (act 122 hp 700)", C2 / C3 skipped). Two fixes:
 * dl_getup_rescuer_word: the get-up writes the rescuer back before the completion when the word is 0; DL_F_STUCK_UP: UP in the rule but wait_rescue / be_rescued with
 * hp > 1 (the engine enters them only at hp 1) for DL_GETUP_VERIFY_MS after the last revive -> stand up (idle). */
#define DL_GETUP_VERIFY_MS 1500u
/* Run C on rev2-int 2026-11-07 08:22 (`work\runs\20261107-080258-downedC`, copy 1 log 527-537), C2 = the MAIN role down: the lethal hit made Barry DOWN, the next pass's
 * follow-up saw act 2 and took the "back to the chain" repair (straight into 0x7A) BEFORE the queued GO_DOWN ran, so GO_DOWN found 0x7A and skipped 0x79. The 0x79 start
 * (cPlAction_pl_act_dying_start slot 6, 0x6C6B60) is what calls FUN_007102A0: player+0x6F54 := 150, +0x6F1C := g_sPlayer+0x54C (a timer the update 0x72C1E6 counts down),
 * +0x78EC |= 0x400. Without it the wait_rescue check (slot 11 0x6D6BC0 -> FUN_006CCBD0) kills a MAIN-role human in a session with gm+0x8F0 == 1 at once (+0x6F54 < 1 or
 * +0x6F1C <= 0; the support role is exempt: FUN_007015C0 = role 1): 122 -> 111 hp 0 after 67 ms, game over. Now: no repair within DL_GO_DOWN_MS of the logic's own
 * down (GO_DOWN puts the human through 0x79 first), and dl_main_dying_keep keeps the stock timer and gauge alive while the rule says DOWN (the rule owns the bleed-out). */
#define DL_GO_DOWN_MS 1000u
#ifdef DL_MODEL_BEFORE_RUNC0822   /* tests only: the logic before this fix; tests\build.sh checks that test_downed FAILS when built with it */
#define DL_GO_DOWN_FIRST 0
#else
#define DL_GO_DOWN_FIRST 1
#endif
#define DL_MAIN_GAUGE 150          /* FUN_007102A0's value of +0x6F54 */
#define DL_MAIN_TIMER_MIN 30.0f    /* +0x6F1C below this while DOWN: topped up to DL_MAIN_TIMER_SET (its unit, seconds or frames, is not measured: both values are large) */
#define DL_MAIN_TIMER_SET 300.0f
enum { DL_F_NONE = 0, DL_F_DEAD, DL_F_ENGINE_DOWN, DL_F_ENGINE_UP, DL_F_BACK_TO_CHAIN, DL_F_STUCK_UP };
static inline int dl_act_lying_family(uint32_t a) { return a == 0x79u || a == 0x7Au || a == 0x7Bu || a == 0x7Du; }
static inline int dl_act_chain_entry(uint32_t a) { return a == 0x79u || a == 0x7Au || a == 0x7Bu; }
static inline int dl_act_stuck_lying(uint32_t a) { return a == 0x7Au || a == 0x7Bu || a == 0x7Du; }
#ifdef DL_MODEL_BEFORE_RUNC0653   /* tests only: the logic before this fix; tests\build.sh checks that test_downed FAILS when built with it */
static inline uint32_t dl_getup_rescuer_word(uint32_t act, uint32_t word, uint32_t remembered) { (void)act, (void)remembered; return word; }
#define DL_STUCK_UP_ON 0
#else
static inline uint32_t dl_getup_rescuer_word(uint32_t act, uint32_t word, uint32_t remembered) { return act == 0x7Du && word == 0 && remembered ? remembered : word; }
#define DL_STUCK_UP_ON 1
#endif
static inline int dl_since_more(uint32_t now, uint32_t t, uint32_t ms) { return t == 0 || dl_diff(now, t) > (int32_t)ms; }
/* while the rule says I am DOWN and I lie (0x79..0x7D): what to write into +0x6F54 (gauge) / +0x6F1C (timer) of a MAIN-role human; returns a mask 1 = gauge, 2 = timer */
static inline int dl_main_dying_keep(int role, int down, uint32_t act, int gauge, float timer, int *new_gauge, float *new_timer)
{
    int m = 0;
#ifdef DL_MODEL_BEFORE_RUNC0822
    (void)role, (void)down, (void)act, (void)gauge, (void)timer, (void)new_gauge, (void)new_timer;
    return 0;
#else
    if (role != 0 || !down || !dl_act_lying_family(act)) return 0;
    if (gauge < 1) *new_gauge = DL_MAIN_GAUGE, m |= 1;
    if (!(timer >= DL_MAIN_TIMER_MIN)) *new_timer = DL_MAIN_TIMER_SET, m |= 2;   /* NaN counts as low */
    return m;
#endif
}
typedef struct { uint32_t act; int hp; unsigned dead_reasons; int getup_pending; uint32_t now, getup_t, down_at, act_fix_t; } dl_follow_in;
static inline int dl_follow(const dl_t *d, const dl_follow_in *in)
{
    const dl_human *m = &d->h[d->me];
    if (m->state == DL_ALIVE) {
        if (in->dead_reasons) return DL_F_DEAD;
        if (dl_act_chain_entry(in->act) && in->hp > 0 && in->hp <= 1 && !in->getup_pending && dl_since_more(in->now, in->getup_t, 3000) && dl_since_more(in->now, m->up_at, DL_UP_GRACE_MS))
            return DL_F_ENGINE_DOWN;
        if (DL_STUCK_UP_ON && dl_act_stuck_lying(in->act) && in->hp > 1 && !in->getup_pending && dl_since_more(in->now, m->up_at, DL_GETUP_VERIFY_MS) && dl_since_more(in->now, in->act_fix_t, 500))
            return DL_F_STUCK_UP;
    } else if (m->state == DL_DOWN) {
        if (in->hp <= 0 || in->dead_reasons) return DL_F_DEAD;
        if (!in->getup_pending && in->hp > 1 && !dl_act_lying_family(in->act) && dl_since_more(in->now, in->down_at, 1500)) return DL_F_ENGINE_UP;
        if (!in->getup_pending && !dl_act_lying_family(in->act) && in->act != 0x80000000u && dl_since_more(in->now, in->act_fix_t, 500) && (!DL_GO_DOWN_FIRST || dl_since_more(in->now, m->down_at, DL_GO_DOWN_MS)))
            return DL_F_BACK_TO_CHAIN;
    }
    return DL_F_NONE;
}

/* ---- the hold of my human on a downed partner (mod-driven revive; the engine's own hold uses dl_local_revive_*) ----
 * holding = the interact button is down, in_range = close enough, called every pass with the time passed (dt ms) */
static inline void dl_local_hold(dl_t *d, int target, int holding, int in_range, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (!d->cfg.enabled) return;
    int ok = holding && in_range && m->state == DL_ALIVE && target >= 0 && target < DL_MAX && target != d->me && d->h[target].state == DL_DOWN;
    if (!ok) {
        if (m->hold_target >= 0) {
            dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
            dl_push(d, DL_O_HOLD_RESET, d->me, 0);
            if (d->h[m->hold_target].reviver == d->me) d->h[m->hold_target].reviver = -1;
            m->hold_target = -1, m->hold_ms = 0;
        }
        return;
    }
    if (m->hold_target != target) {   /* a new hold starts at zero */
        if (m->hold_target >= 0) dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
        m->hold_target = target, m->hold_ms = 0, m->hold_ka = now, m->hold_t = now;
        d->h[target].reviver = d->me, d->h[target].revive_last = now;
        dl_push(d, DL_O_SEND_BEGIN, d->me, target);
        return;
    }
    uint32_t dt = dl_diff(now, m->hold_t) > 0 ? (uint32_t)dl_diff(now, m->hold_t) : 0;
    m->hold_t = now;
    if (dt > 250) dt = 250;   /* a long stall of the game thread is not progress */
    m->hold_ms += dt;
    d->h[target].revive_last = now;
    if (dl_diff(now, m->hold_ka) >= (int32_t)DL_KEEPALIVE_MS) {
        m->hold_ka = now;
        dl_push(d, DL_O_SEND_KEEPALIVE, d->me, target);
    }
    if (m->hold_ms >= d->cfg.revive_ms) {   /* done: he is up */
        dl_human *t = &d->h[target];
        dl_human_reset(t, DL_ALIVE, now);
        m->hold_target = -1, m->hold_ms = 0;
        d->n_revive++;
        dl_push(d, DL_O_SEND_DONE, d->me, target);
        dl_push(d, DL_O_PUPPET_UP, target, 0);
        dl_push(d, DL_O_HOLD_RESET, d->me, 0);
    }
}

/* the ENGINE's own hold (pl_act_rescue 0x7C) began / ended on my side: same bookkeeping, the progress is the engine's */
static inline void dl_local_revive_begin(dl_t *d, int target, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (!d->cfg.enabled || target < 0 || target >= DL_MAX || target == d->me || d->h[target].state != DL_DOWN || m->state != DL_ALIVE) return;
    if (m->hold_target == target) return;
    m->hold_target = target, m->hold_ms = 0, m->hold_ka = now;
    d->h[target].reviver = d->me, d->h[target].revive_last = now;
    dl_push(d, DL_O_SEND_BEGIN, d->me, target);
}
/* the engine's hold goes on: keep the downed side's watchdog quiet (call every pass while the engine's rescue action runs) */
static inline void dl_local_revive_keep(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->hold_target < 0) return;
    d->h[m->hold_target].revive_last = now;
    if (dl_diff(now, m->hold_ka) >= (int32_t)DL_KEEPALIVE_MS) {
        m->hold_ka = now;
        dl_push(d, DL_O_SEND_KEEPALIVE, d->me, m->hold_target);
    }
}
static inline void dl_local_revive_cancel(dl_t *d)
{
    dl_human *m = &d->h[d->me];
    if (m->hold_target < 0) return;
    dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
    if (d->h[m->hold_target].reviver == d->me) d->h[m->hold_target].reviver = -1;
    m->hold_target = -1, m->hold_ms = 0;
}
static inline void dl_local_revive_done(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->hold_target < 0) return;
    int t = m->hold_target;
    dl_human_reset(&d->h[t], DL_ALIVE, now);
    m->hold_target = -1, m->hold_ms = 0;
    d->n_revive++;
    dl_push(d, DL_O_SEND_DONE, d->me, t);
    dl_push(d, DL_O_PUPPET_UP, t, 0);
}

/* ---- the partner ---- */

/* a reliable event of human i (pid mapped to a human index by the glue); arg = target human for BEGIN / KEEPALIVE / CANCEL / DONE */
static inline void dl_remote_event(dl_t *d, int i, int kind, int arg, uint32_t now)
{
    if (i < 0 || i >= DL_MAX || i == d->me || !d->cfg.enabled) return;
    dl_human *r = &d->h[i];
    if (r->state == DL_GONE) return;
    r->last_seen = now, r->mismatch_since = 0;
    switch (kind) {
    case DL_E_DOWN:
        if (r->state == DL_ALIVE) {
            r->state = DL_DOWN, r->down_at = now, r->bleed_left = d->cfg.bleed_ms ? (int32_t)d->cfg.bleed_ms : 0x7FFFFFFF, r->reviver = -1;
            d->n_down++;
            dl_push(d, DL_O_PUPPET_DOWN, i, 0);
        }
        break;
    case DL_E_DEAD:
        if (r->state != DL_DEAD) {
            r->state = DL_DEAD, r->reviver = -1;
            dl_push(d, DL_O_PUPPET_UP, i, 0);   /* the puppet is not lying for the rescue any more (the death screen follows) */
        }
        break;
    case DL_E_BEGIN:   /* i holds the button on arg */
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN) {
            d->h[arg].reviver = i, d->h[arg].revive_last = now;
            if (arg == d->me) dl_push(d, DL_O_REVIVE_PLAY, d->me, i);
        }
        break;
    case DL_E_KEEPALIVE:
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN) {
            if (d->h[arg].reviver != i) {   /* a missed BEGIN: take it as one */
                d->h[arg].reviver = i;
                if (arg == d->me) dl_push(d, DL_O_REVIVE_PLAY, d->me, i);
            }
            d->h[arg].revive_last = now;
        }
        break;
    case DL_E_CANCEL:
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN && d->h[arg].reviver == i) {
            d->h[arg].reviver = -1;
            if (arg == d->me) dl_push(d, DL_O_REVIVE_STOP, d->me, i);
        }
        break;
    case DL_E_DONE:   /* i raised arg (arg == i: "I am up", the engine finished the revive on his side) */
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN) {
            dl_human *t = &d->h[arg];
            dl_human_reset(t, DL_ALIVE, now);
            d->n_revive++;
            if (arg == d->me) t->up_at = now ? now : 1, dl_push(d, DL_O_GET_UP, d->me, i);
            else dl_push(d, DL_O_PUPPET_UP, arg, 0);
        }
        break;
    default:
        break;
    }
    dl_check_over(d);
}

/* the unreliable STATE of human i: down / dead flags. Dead is believed at once (hp 0 is certain); a disagreement about DOWN / UP has to last DL_TRUST_MS (the reliable event may be on its way) */
static inline void dl_remote_sample(dl_t *d, int i, int down, int dead, uint32_t now)
{
    if (i < 0 || i >= DL_MAX || i == d->me || !d->cfg.enabled) return;
    dl_human *r = &d->h[i];
    if (r->state == DL_GONE) return;
    r->last_seen = now;
    int said = dead ? DL_DEAD : down ? DL_DOWN : DL_ALIVE;
    if (said == r->state) {
        r->mismatch_since = 0;
        return;
    }
    if (said == DL_DEAD) {
        dl_remote_event(d, i, DL_E_DEAD, 0, now);
        return;
    }
    if (r->mismatch_state != said || !r->mismatch_since) {
        r->mismatch_state = said, r->mismatch_since = now ? now : 1;
        return;
    }
    if (dl_diff(now, r->mismatch_since) >= (int32_t)DL_TRUST_MS) {
        r->mismatch_since = 0, d->n_stale_fix++;
        if (said == DL_DOWN) dl_remote_event(d, i, DL_E_DOWN, 0, now);
        else if (r->state == DL_DOWN) dl_remote_event(d, i, DL_E_DONE, i, now);   /* he is up: whatever stopped him lying, the mirror follows */
        else if (r->state == DL_DEAD) {   /* a dead mirror that comes back to life = a reload we missed: believe the sample */
            dl_human_reset(r, DL_ALIVE, now);
            dl_push(d, DL_O_PUPPET_UP, i, 0);
        }
    }
}

/* a player left / joined */
static inline void dl_set_present(dl_t *d, int i, int present, uint32_t now)
{
    if (i < 0 || i >= DL_MAX || i == d->me) return;
    if (!present) {
        d->h[i].state = DL_GONE;
        if (d->h[d->me].hold_target == i) d->h[d->me].hold_target = -1, d->h[d->me].hold_ms = 0;
        if (d->h[d->me].state == DL_DOWN) {
            d->h[d->me].reviver = d->h[d->me].reviver == i ? -1 : d->h[d->me].reviver;
            if (d->cfg.get_up_on_leave && dl_count_present(d) <= 1) {   /* alone and lying: stand up */
                dl_human_reset(&d->h[d->me], DL_ALIVE, now);
                d->h[d->me].up_at = now ? now : 1;
                dl_push(d, DL_O_GET_UP, d->me, -1);
            }
        }
    } else if (d->h[i].state == DL_GONE) {
        dl_human_reset(&d->h[i], DL_ALIVE, now);
    }
}

/* ---- time ---- call every pass (10..60 Hz) ---- */
static inline void dl_tick(dl_t *d, uint32_t now)
{
    int32_t dt = dl_diff(now, d->last_tick);
    if (dt < 0) dt = 0;
    if (dt > 1000) dt = 1000;   /* a stall (loading, the debugger): the bleed timer does not eat it */
    d->last_tick = now;
    if (!d->cfg.enabled) return;
    for (int i = 0; i < DL_MAX; i++) {
        dl_human *h = &d->h[i];
        if (h->state != DL_DOWN) continue;
        if (h->reviver >= 0) {   /* being raised: frozen; the watchdog ends a revive nobody keeps alive */
            uint32_t since = (uint32_t)dl_diff(now, h->revive_last);
            int mine = h->reviver == d->me;   /* my own hold is kept alive by dl_local_hold / the engine glue every pass */
            if (!mine && since > DL_WATCHDOG_MS) {
                h->reviver = -1;
                if (i == d->me) dl_push(d, DL_O_REVIVE_STOP, d->me, -1);
            }
            continue;
        }
        if (h->bleed_left == 0x7FFFFFFF) continue;   /* never */
        h->bleed_left -= dt;
        if (h->bleed_left <= 0) {
            if (i == d->me) {   /* my own bleed-out: I die, the engine's game over follows */
                h->state = DL_DEAD;
                d->n_bleed++;
                dl_push(d, DL_O_SEND_DEAD, d->me, 1);
                dl_push(d, DL_O_DIE, d->me, 0);
                dl_check_over(d);
            } else if (h->bleed_left <= -(int32_t)DL_REMOTE_GRACE_MS) {   /* a mirror: the DEAD event is late: believe the prediction */
                h->state = DL_DEAD;
                d->n_bleed++;
                dl_push(d, DL_O_PUPPET_UP, i, 0);
                dl_check_over(d);
            }
        }
    }
    dl_check_over(d);
}

static inline const char *dl_state_name(int s) { return s == DL_ALIVE ? "up" : s == DL_DOWN ? "DOWN" : s == DL_DEAD ? "DEAD" : "gone"; }

/* one line for the console / the log */
static inline int dl_status(const dl_t *d, char *out, int size)
{
    int o = snprintf(out, (size_t)size, "downed: %s, bleed %us revive %ums, over %d; downs %u revives %u bleed-outs %u game-overs %u (dropped %u, mirror fixes %u);",
                     d->cfg.enabled ? "ON" : "off", d->cfg.bleed_ms / 1000, d->cfg.revive_ms, d->over, d->n_down, d->n_revive, d->n_bleed, d->n_over, d->n_dropped, d->n_stale_fix);
    for (int i = 0; i < DL_MAX && o < size - 40; i++) {
        if (d->h[i].state == DL_GONE) continue;
        o += snprintf(out + o, (size_t)(size - o), " [%d%s %s", i, i == d->me ? " me" : "", dl_state_name(d->h[i].state));
        if (d->h[i].state == DL_DOWN) o += snprintf(out + o, (size_t)(size - o), " left %ds%s", d->h[i].bleed_left == 0x7FFFFFFF ? -1 : d->h[i].bleed_left / 1000, d->h[i].reviver >= 0 ? " REVIVED" : "");
        if (d->h[i].hold_target >= 0) o += snprintf(out + o, (size_t)(size - o), " holding %d %ums", d->h[i].hold_target, d->h[i].hold_ms);
        o += snprintf(out + o, (size_t)(size - o), "]");
    }
    return o;
}

/* session flags byte (cp_session.flags): bit 0 = rule on, bits 1..2 = bleed-out preset */
#define CP_SESS_DOWNED 1u
static inline uint32_t dl_preset_ms(unsigned preset) { static const uint32_t t[4] = {60000u, 30000u, 120000u, 0u}; return t[preset & 3]; }
static inline void dl_cfg_from_flags(dl_cfg *c, unsigned flags)
{
    c->enabled = (flags & CP_SESS_DOWNED) != 0;
    c->bleed_ms = dl_preset_ms((flags >> 1) & 3);
    c->revive_ms = DL_REVIVE_DEFAULT_MS;
    c->get_up_on_leave = 1;
}

/* ---- live probe of the engine's action ids 0x76..0x80 (test instances only: `net downed probe`, REV2_DOWNED_PROBE=1; downed.inc). Pure bookkeeping, the reads are in downed.inc. */
#define DL_PROBE_FIRST 0x76u
#define DL_PROBE_LAST 0x80u
/* 1 = this id was not logged yet (it is marked now), 0 = out of range or already logged. mask: bit (id - 0x76), reset with 0 */
static inline int dl_probe_first_seen(uint32_t *mask, unsigned id)
{
    if (id < DL_PROBE_FIRST || id > DL_PROBE_LAST) return 0;
    uint32_t bit = 1u << (id - DL_PROBE_FIRST);
    if (*mask & bit) return 0;
    *mask |= bit;
    return 1;
}
/* the class name the notes INFER from the ChangeAction call sites (not verified: the probe's DTI line is the proof) */
static inline const char *dl_probe_inferred_name(unsigned id)
{
    switch (id) {
    case 0x76: return "pl_act_die? (death type, INFERRED)";
    case 0x77: return "knock-down / get-up after a grab or a damage floor (INFERRED)";
    case 0x78: return "no call site found in the notes";
    case 0x79: return "pl_act_dying_start (INFERRED)";
    case 0x7A: return "pl_act_wait_rescue, type 0 (INFERRED)";
    case 0x7B: return "pl_act_wait_rescue, type 1 (INFERRED)";
    case 0x7C: return "pl_act_rescue, the rescuer's hold (INFERRED)";
    case 0x7D: return "pl_act_be_rescued (INFERRED)";
    case 0x7E: return "be_rescued follow-up, get-up decision (INFERRED)";
    case 0x7F: return "held / carried variant (INFERRED)";
    case 0x80: return "held / carried variant, catch start (INFERRED)";
    default: return "outside 0x76..0x80";
    }
}
static inline int dl_probe_in_chain(unsigned id) { return id >= DL_PROBE_FIRST && id <= DL_PROBE_LAST; }

/* ---- change watch of the probe (test instances): the action id +0x1A54 of EVERY player slot (the AI partner's slot included), logged at each change, not only
 * at the first sight of an id. Live run A2 (2026-11-06) showed why the first-sight probe alone is not enough: it logged nothing, because it ran only inside a net session.
 * Bounded: per slot at most DL_WATCH_BURST change lines a second, at most DL_WATCH_CAP lines per arming (`probe on` / `probe reset`); a change into or out of 0x76..0x80
 * is never suppressed; suppressed changes are counted and reported with the next logged line of that slot. */
#define DL_WATCH_SLOTS 2
#define DL_WATCH_BURST 12u
#define DL_WATCH_CAP 3000u
enum dl_watch_res { DL_W_NONE = 0, DL_W_NEW_OBJ, DL_W_GONE, DL_W_CHANGE, DL_W_CAP };
typedef struct {
    uint32_t obj[DL_WATCH_SLOTS], act[DL_WATCH_SLOTS], sec_t[DL_WATCH_SLOTS], sec_n[DL_WATCH_SLOTS], dropped[DL_WATCH_SLOTS];
    uint32_t lines, changes;
    int capped;
} dl_watch_t;

/* one sample of slot s (obj = the player object, 0 = none; act = its +0x1A54). *prev = the id before (0xFFFFFFFF = none known), *dropped = changes of this slot not logged since its last line */
static inline int dl_watch_step(dl_watch_t *w, int s, uint32_t obj, uint32_t act, uint32_t now, uint32_t *prev, uint32_t *dropped)
{
    *prev = 0xFFFFFFFFu, *dropped = 0;
    if (s < 0 || s >= DL_WATCH_SLOTS) return DL_W_NONE;
    if (!obj) {
        if (!w->obj[s]) return DL_W_NONE;
        *prev = w->act[s];
        w->obj[s] = 0, w->lines++;
        return DL_W_GONE;
    }
    if (obj != w->obj[s]) {   /* first sample of the slot, or a new level instance (new object) */
        if (w->obj[s]) *prev = w->act[s];
        w->obj[s] = obj, w->act[s] = act, w->sec_t[s] = now, w->sec_n[s] = 0, w->lines++;
        *dropped = w->dropped[s], w->dropped[s] = 0;
        return DL_W_NEW_OBJ;
    }
    if (act == w->act[s]) return DL_W_NONE;
    *prev = w->act[s];
    w->act[s] = act, w->changes++;
    if ((uint32_t)(now - w->sec_t[s]) >= 1000u) w->sec_t[s] = now, w->sec_n[s] = 0;
    if (!dl_probe_in_chain(act) && !dl_probe_in_chain(*prev)) {
        if (w->lines >= DL_WATCH_CAP) {
            w->dropped[s]++;
            if (w->capped) return DL_W_NONE;
            w->capped = 1;
            return DL_W_CAP;
        }
        if (w->sec_n[s] >= DL_WATCH_BURST) {
            w->dropped[s]++;
            return DL_W_NONE;
        }
    }
    w->sec_n[s]++, w->lines++;
    *dropped = w->dropped[s], w->dropped[s] = 0;
    return DL_W_CHANGE;
}

/* ---- test command `net downed pkt` (REV2_DOWNED_PKT=1 only): a synthesized packet for the engine's Raid rescue handler FUN_009DF820 (build 3658995).
 * Calling convention (disassembly, notes\downed-finish.md): `void __stdcall f(int seat, const uint8_t *pkt)`, RET 8; the incoming ECX is overwritten before any use
 * (its only caller, the dispatcher cCommunicatorPlayer_vf6 0x9DF220, pushes the two arguments and leaves ECX as it is). `seat` is only compared with gm+0x904 (equal = the
 * packet is our own: ignored). The handler reads exactly three things of the packet: bits 13..24 of the dword at +0 (sender slot), byte +4 (state 0..3), byte +5 (target slot).
 * Bits 5..12 of the dword are the message type of the dispatcher (9 for this message, INFERRED from FUN_009F78F0's send call), unused by the handler itself. */
#define DL_PKT_FN 0x9DF820u
#define DL_PKT_TABLE 0x9DF960u        /* its jump table of the 4 states */
#define DL_PKT_RET 0x9DF95Au          /* its common `ret 8` */
#define DL_PKT_TYPE 9u
static inline void dl_pkt_build(uint8_t pkt[8], unsigned sender, unsigned target, unsigned state)
{
    uint32_t w = ((sender & 0xFFFu) << 13) | (DL_PKT_TYPE << 5);
    memset(pkt, 0, 8);
    pkt[0] = (uint8_t)w, pkt[1] = (uint8_t)(w >> 8), pkt[2] = (uint8_t)(w >> 16), pkt[3] = (uint8_t)(w >> 24);
    pkt[4] = (uint8_t)state, pkt[5] = (uint8_t)target;
}
/* what the handler reads (the same arithmetic as its code: shr 0xD, and 0xFFF; movzx byte +5; movzx byte +4) */
static inline unsigned dl_pkt_sender(const uint8_t pkt[8]) { return (((uint32_t)pkt[0] | (uint32_t)pkt[1] << 8 | (uint32_t)pkt[2] << 16 | (uint32_t)pkt[3] << 24) >> 13) & 0xFFFu; }
static inline unsigned dl_pkt_target(const uint8_t pkt[8]) { return pkt[5]; }
static inline unsigned dl_pkt_state(const uint8_t pkt[8]) { return pkt[4]; }
/* the test command takes the two human slots only (the game has 8, the mod 2), different slots, states 0..3 */
static inline int dl_pkt_args_ok(int state, int sender, int target) { return state >= 0 && state <= 3 && sender >= 0 && sender <= 1 && target >= 0 && target <= 1 && sender != target; }
/* the code the command expects in memory (read from the decrypted image of build 3658995; the file on disk is encrypted by the Steam stub until it runs) */
static const uint8_t dl_pkt_entry_bytes[15] = {0xA1, 0x00, 0xAE, 0x57, 0x01, 0x8B, 0x4C, 0x24, 0x04, 0x3B, 0x88, 0x04, 0x09, 0x00, 0x00};   /* mov eax,[gm]; mov ecx,[esp+4]; cmp ecx,[eax+0x904] */
static const uint32_t dl_pkt_table_words[4] = {0x9DF8A2u, 0x9DF8F6u, 0x9DF92Du, 0x9DF945u};
static const uint8_t dl_pkt_ret_bytes[3] = {0xC2, 0x08, 0x00};   /* ret 8 */
static inline int dl_pkt_code_ok(const uint8_t entry[15], const uint32_t table[4], const uint8_t ret[3])
{
    return !memcmp(entry, dl_pkt_entry_bytes, 15) && !memcmp(table, dl_pkt_table_words, sizeof dl_pkt_table_words) && !memcmp(ret, dl_pkt_ret_bytes, 3);
}
/* ---- the rescue input of my human (dl_hold_button / `net downed input` in downed.inc), build 3658995, Ghidra (notes\downed-live-checks.md section 11).
 * The stock rescue has two action-button entries (sActionButton = [0x1552A10]): the press entry every player registers (uPlayer vf306 0x72ABC0, type 1, button 0x27,
 * keyboard action 0x88, condition FUN_006F5500 = a partner in 0x7A/0x7B within sPlayer+0xCF4, in sight, faced within sPlayer+0xCF8; trigger 0x70E220 = ChangeAction(0x7C))
 * and, while pl_act_rescue (0x7C) runs, its hold entry (start 0x6C9A70, type 0xD, the same button). The action-button update FUN_0040EDC0 writes the byte
 * sAB+0x19FC+index ONLY for a type 0xD entry (zeroed first, then held = 1) -> the byte is 0 whenever the stock rescue is not running (run C 2026-11-06: always 0).
 * The real input of that entry, read here the same way:
 *  - keyboard (FUN_0040EDC0 when [[0x157D120]+0x15C4BC] == 1): FUN_0099E1E0(sKeyboard [0x15DE918], 1 = held, key action FUN_0040EBB0(0x88) = [0x124F250] = 0x1E
 *    (the "Rescue" line of config.ini, Tab in the owner's config: Rescue=2 -> table 0x13C2840[2] = VK 0x09), 2 = both bindings);
 *  - pad: button id FUN_0040AD80(0x27) for the slot (slot's button type gm+0x7C+0xE4*slot: 0 -> id 2, 1..4 -> id 4), mask = table 0x124F020[id]
 *    (MT order: id 2 = 0x1000 Triangle/Y, id 4 = 0x2000 Circle/B), held word ("on", pad +0x2C) of the slot's pad in sActionButton (container +0x38+0x338*slot, pad at +0xC);
 *  - plus the engine's own byte (meaningful only during 0x7C).
 * Stock bug fixed by the mod (dl_kbfix in downed.inc): in keyboard mode FUN_0040EDC0 writes sAB+0x19FC+0 (no index) while the 0x7C update 0x6D2390 reads +role: a role-1
 * human (Moira/Natalia) on the keyboard can never hold the stock rescue. */
#define DL_IN_AB_CELL 0x1552A10u          /* sActionButton */
#define DL_IN_AB_HOLD 0x19FCu             /* + index: byte, held state of a type 0xD entry */
#define DL_IN_AB_CONT(slot) (0x38u + 0x338u * (unsigned)(slot))   /* the per-slot container (FUN_0040C600 sets +8 = slot) */
#define DL_IN_AB_PAD_ON 0x38u             /* container + 0xC (pad) + 0x2C ("on" word, FUN_00B42680) */
#define DL_IN_AB_PAD_NUM 0x18u            /* container + 0xC + 0xC: the pad number (player+0x7924, 0xFF none) */
#define DL_IN_KEYQ_FN 0x99E1E0u           /* thiscall(sKeyboard, mode 1 = held, key action, 2 = both bindings), ret 0xC, AL */
#define DL_IN_KEYCFG_CELL 0x15DE918u
#define DL_IN_KEYACT_CELL 0x124F250u      /* FUN_0040EBB0(0x88) reads this cell */
#define DL_IN_KEYACT 0x1Eu
#define DL_IN_BTN_FN 0x40AD80u            /* thiscall(entry, slot), ret 4: reads entry+0x2C only */
#define DL_IN_BTN_MASKS 0x124F020u
#define DL_IN_RESCUE_BTN 0x27u
#define DL_IN_KBMODE_CELL 0x157D120u
#define DL_IN_KBMODE_OFF 0x15C4BCu
#define DL_IN_GUI_CELL 0x15DE768u         /* g_sGUI: +0x16C4 = 1 action buttons allowed, 0 while a guide (tutorial) panel is up (uGuideFlow_vf8) */
#define DL_IN_GUI_OK 0x16C4u
#define DL_KBFIX_AT 0x40F02Bu             /* FUN_0040EDC0, keyboard branch of case 0xD */
static const uint8_t dl_in_keyq_bytes[11] = {0x83, 0xEC, 0x0C, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x1C, 0x8B, 0xD9};          /* sub esp,0xc; push ebx; push esi; mov esi,[esp+0x1c]; mov ebx,ecx */
static const uint8_t dl_in_btn_bytes[12] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0xD9, 0x83, 0xFF, 0xFF};    /* push ebx/esi/edi; mov edi,[esp+0x10]; mov ebx,ecx; cmp edi,-1 */
/* the keyboard write: mov ecx,[sAB]; mov bl,1; mov [ecx+0x19fc],al; jmp +0x2e  ->  add edi,[sAB]; mov bl,1; mov [edi+0x19fc],al; jmp +0x2e (EDI = the slot, dead after the jump) */
static const uint8_t dl_kbfix_orig[16] = {0x8B, 0x0D, 0x10, 0x2A, 0x55, 0x01, 0xB3, 0x01, 0x88, 0x81, 0xFC, 0x19, 0x00, 0x00, 0xEB, 0x2E};
static const uint8_t dl_kbfix_new[16] = {0x03, 0x3D, 0x10, 0x2A, 0x55, 0x01, 0xB3, 0x01, 0x88, 0x87, 0xFC, 0x19, 0x00, 0x00, 0xEB, 0x2E};
/* the code the reader calls and the two tables it reads must be those of build 3658995 (masks: id 1..4 = 0x8000 0x1000 0x4000 0x2000) */
static inline int dl_in_code_ok(const uint8_t keyq[11], const uint8_t btn[12], const uint32_t masks[5], uint32_t keyact)
{
    return !memcmp(keyq, dl_in_keyq_bytes, 11) && !memcmp(btn, dl_in_btn_bytes, 12) && masks[0] == 0 && masks[1] == 0x8000u && masks[2] == 0x1000u && masks[3] == 0x4000u &&
           masks[4] == 0x2000u && keyact == DL_IN_KEYACT;
}
/* FUN_0040AD80 case 0x27 (what the engine call returns; kept here for the tests and the log) */
static inline unsigned dl_in_rescue_btn_id(int btn_type) { return btn_type >= 1 && btn_type <= 4 ? 4u : 2u; }
/* the engine's byte counts only while my human is in the stock rescue (0x7C): nothing resets it when the hold entry is gone (run H 2026-11-07: 1 for minutes after a rescue) */
static inline int dl_in_engine_byte(int raw, uint32_t my_act) { return my_act == 0x7Cu && raw != 0; }
/* the hold decision: the engine's byte, the rescue key, or the rescue button of the slot's pad */
static inline int dl_in_held(int engine_byte, int key_held, uint32_t pad_on, uint32_t pad_mask) { return engine_byte != 0 || key_held != 0 || (pad_on & pad_mask) != 0; }
/* MT pad bit -> name and the XInput wButtons bit that produces it (`rpad` takes XInput; the MT order is the PlayStation order: Triangle 0x1000, Circle 0x2000, Cross 0x4000,
 * Square 0x8000; XInput Y 0x8000, B 0x2000, A 0x1000, X 0x4000). The XInput side is checked live (section 11), not read from the exe. */
static inline const char *dl_in_pad_name(uint32_t mt) { return mt == 0x1000u ? "Y/Triangle" : mt == 0x2000u ? "B/Circle" : mt == 0x4000u ? "A/Cross" : mt == 0x8000u ? "X/Square" : "?"; }
static inline unsigned dl_in_xinput_of(uint32_t mt) { return mt == 0x1000u ? 0x8000u : mt == 0x2000u ? 0x2000u : mt == 0x4000u ? 0x1000u : mt == 0x8000u ? 0x4000u : 0u; }
/* the keyboard fix: 1 = the bytes are the original (patch now), 2 = already patched, 0 = something else (leave it) */
static inline int dl_kbfix_state(const uint8_t cur[16]) { return !memcmp(cur, dl_kbfix_orig, 16) ? 1 : !memcmp(cur, dl_kbfix_new, 16) ? 2 : 0; }

/* what the static reading says each state does (printed with the command's answer) */
static inline const char *dl_pkt_expect(unsigned state)
{
    switch (state) {
    case 0: return "begin: target kind != 2 and act 0x7A/0x7B -> target act 0x7D (be_rescued) and target+7BD8 = sender; target not lying -> a reply only inside a Raid net session (nothing here)";
    case 1: return "rejected: sender kind != 2, act 0x7C and sender+7BE8 == target -> sender act 0 (its rescue action ends)";
    case 2: return "success: target+7BD8 == sender -> FUN_0070CCC0(target): kind != 2, act 0x7D, rescuer set -> hp (support role: max hp, else sPlayer+0xCEC), grace +6F24, get-up motion";
    case 3: return "cancel: target+7BD8 == sender -> FUN_0070CC90(target): kind != 2, act 0x7D, rescuer set -> act 0x7A (lying again)";
    default: return "invalid state";
    }
}

#endif
