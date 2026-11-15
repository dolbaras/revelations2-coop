/* pmot_logic.h: 0.3.2 anim032, the motion mirror (notes\motion-mirror.md). Pure: no Windows, no engine; native/pmot.inc runs it on the game thread, tests/test_pmot.c drives it.
 *
 * The action mirror (pupanim_logic.h) replays only the ids of its mask; the rest (set-model events = INT_MIN, 55-85 move / ladder, 100-107 crab walk, 126-130, 42) needs the
 * owner's world. Here the owner's PC sends the MOTIONS of its human's 16 layers (STATE tail, coop_proto.h cp_motion) while it is in such an action, and the other PC plays the
 * same motions on the puppet's layers through the puppet's own setMotion (slot 27): only the picture, the engine's own body copy uPlCarryControl_vf9 (0x728480) does the
 * same steps. The puppet is held in action 0 (wait: no body-layer controller, FUN_00724D30 needs +0x78E8 bit 3) while the mirror runs. A set model's motion list that the owner's
 * bank 15 holds (lever, box) is put into the puppet's bank 15 with the stock FUN_00D422C0 and given back at the end (pm_bank_*: references balanced).
 * A lever's / board's / rock's step that the receiver does not replay (objsync_logic.h os_door_apply) brings the sender's layer-0 motion in the DOOR event (pm_sm_*): the handle
 * moves on both screens, the end pose still comes with the replayed 0 / 2 / 4.
 * Built with -DPM_MODEL_BEFORE (rel032 cfd6200: no motion mirror) tests/test_pmot.c must fail (tests/build.sh). */
#ifndef PMOT_LOGIC_H
#define PMOT_LOGIC_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "coop_proto.h"
#include "pupanim_logic.h"
#include "objsync_logic.h"

#define PM_LAYERS 16
#define PM_LAYER_OFF 0x4A0u      /* uModel: layer records, 0x140 each (PROVEN 0xD41FF0) */
#define PM_LAYER_SIZE 0x140u
#define PM_BANKS_OFF 0x18F4u     /* 16 motion lists (PROVEN 0xD41A30) */
#define PM_BANK_EVENT 15u        /* the bank the set-model interactions use (PROVEN 0x7C39F0, 0x6C9250) */
#define PM_NO_ID 0xFFFFu
#define PM_INT_MIN 0x80000000u
#define PM_DRIFT 6.0f            /* frames: a puppet layer this far from the owner's frame (at a new sample) is set again */
#define PM_TAIL_LOST_MS 500u     /* motion mode ends when no tail came this long */
#define PM_RESET_MAX 4u          /* re-sets of one layer and id after the puppet changed it, then the layer is left to the puppet (no fight) */
#define PM_RESET_GAP_MS 50u      /* and not sooner than this after the previous set */
#define PM_HOLD_RETRY_MS 1000u   /* the hold (ChangeAction 0) again at most once a second */
#define PM_RATE_DEF 0.03f        /* frames per ms at speed 1 until a rate was measured from the owner's samples */
#define PM_RATE_MAX 0.2f
#define PM_EXTRA_MS 120u         /* the owner's frame is projected at most this far past its sample */
#define PM_BANK_DEFER_MS 3000u   /* after the exit: bank 15 is given back once no puppet layer plays a motion of it; after this long the caller stops those layers first */

/* ---- which owner actions the motion mirror covers (always: and NOT in the action mask) ---- */
#ifdef PM_MODEL_BEFORE
static inline int pm_enter_id(uint32_t a) { (void)a; return 0; }
static inline int pm_cont_id(uint32_t a) { (void)a; return 0; }
#else
/* enter: the set-model events (INT_MIN), the inventory (42), 55-85 fence / jump over / climb / fall / ladder / step up / long jump, 100-107 crab walk, 126 put down, 127 pick up.
 * Not: 34-38 (the equipment sync owns the hand; Raid), 108-125 and 149-151 (die / dying / rescue: game over and downed1 own them), 128-130 (carried / take down / call fellow:
 * story; a carried human's layers are copied every frame by the engine's own uPlCarryControl_vf9 on each PC), 217-241 (not the human's), ids outside the table */
static inline int pm_enter_id(uint32_t a)
{
    if (a == PM_INT_MIN) return 1;
    return a == 42 || (a >= 55 && a <= 85) || (a >= 100 && a <= 107) || a == 126 || a == 127;
}
/* continue: plus 0 / 1 (wait: the owner stands still after a door or a pickup) */
static inline int pm_cont_id(uint32_t a) { return pm_enter_id(a) || a == 0 || a == 1; }
#endif

/* ---- sender ---- */
typedef struct { int on; uint32_t n_on, n_off, n_enemy, n_nosrc, n_tails; } pm_tx_t;
static inline void pm_tx_reset(pm_tx_t *t) { memset(t, 0, sizeof *t); }
/* 1 = this sample carries the tail. held_enemy: the owner's human is held by an enemy (flags7900 bit 13): its own path, no tail; an INT_MIN whose bank 15 holds a list that is
 * no set model's (an enemy's grab, a scene) has no tail either */
static inline int pm_tx_want(pm_tx_t *t, const uint32_t bits[8], uint32_t act, int held_enemy, int bank15_used, int src_found)
{
    int ok = !pa_anim_allowed(bits, act) && (pm_enter_id(act) || (t->on && pm_cont_id(act)));
    if (ok && held_enemy) t->n_enemy++, ok = 0;
    if (ok && act == PM_INT_MIN && bank15_used && !src_found) t->n_nosrc++, ok = 0;
    if (ok != t->on) {
        if (ok) t->n_on++;
        else t->n_off++;
        t->on = ok;
    }
    if (ok) t->n_tails++;
    return ok;
}

/* ---- bank 15: the set model's list on the puppet, references balanced ---- */
enum { PM_OP_NONE = 0, PM_OP_ADDREF, PM_OP_ATTACH, PM_OP_RELEASE };   /* ADDREF / RELEASE = FUN_00C0DCE0 / FUN_00C0E080 (this = resource); ATTACH = FUN_00D422C0(puppet, res, 15) */
typedef struct { uint8_t op; uint32_t res; } pm_op_t;
typedef struct { int active; uint32_t saved, ours; uint32_t n_attach, n_restore, n_busy, n_keep_rel; } pm_bank_t;

/* the mirror wants `list` in the puppet's bank 15 (cur = what it holds now). ops: up to 2; returns their count, -1 when bank 15 was changed by someone else since our
 * attach (an enemy's grab, a stock event): not touched */
static inline int pm_bank_use(pm_bank_t *b, uint32_t cur, uint32_t list, pm_op_t ops[3])
{
    int n = 0;
    if (!list) return 0;
    if (!b->active) {
        if (cur == list) return 0;   /* already there: nothing of ours */
        b->active = 1, b->saved = cur, b->ours = list;
        if (cur) ops[n].op = PM_OP_ADDREF, ops[n++].res = cur;   /* the puppet's own list stays alive while ours is in (ATTACH releases it once) */
        ops[n].op = PM_OP_ATTACH, ops[n++].res = list;
        b->n_attach++;
        return n;
    }
    if (cur != b->ours) {
        b->n_busy++;
        return -1;
    }
    if (list == b->ours) return 0;
    ops[n].op = PM_OP_ATTACH, ops[n++].res = list, b->ours = list, b->n_attach++;
    return n;
}
/* A layer caches pointers into its motion's data (setMotion 0xD41FF0 writes rec +0x110.. / +0x134 / +0x138 from the motion; the reset path 0xD4222C clears them): the list of
 * bank 15 must not lose a reference while a layer of the puppet still plays one of its motions (the release could free that data). The caller gives bank 15 back only when
 * pm_bank_due says so, and stops those layers first when it answers 2 (the deferral ran out, a stage unload, the mirror switched off). */
static inline int pm_ids_use_bank(const uint16_t ids[PM_LAYERS], unsigned bank)
{
    for (unsigned l = 0; l < PM_LAYERS; l++)
        if (ids[l] != PM_NO_ID && ((ids[l] >> 8) & 0xFu) == bank) return 1;
    return 0;
}
/* the end of the mirror (exit, stage unload, puppet gone): puppet_ok = the same live puppet object. Gives bank 15 back (when it still holds ours) and drops the keep reference */
static inline int pm_bank_end(pm_bank_t *b, int puppet_ok, uint32_t cur, pm_op_t ops[3])
{
    int n = 0;
    if (!b->active) return 0;
    if (puppet_ok && cur == b->ours) ops[n].op = PM_OP_ATTACH, ops[n++].res = b->saved, b->n_restore++;   /* saved may be 0: bank 15 empty again, ours released */
    if (b->saved) ops[n].op = PM_OP_RELEASE, ops[n++].res = b->saved, b->n_keep_rel++;
    b->active = 0, b->saved = b->ours = 0;
    return n;
}

/* ---- receiver ---- */
enum { PM_RX_OFF = 0, PM_RX_ENTER, PM_RX_RUN, PM_RX_EXIT, PM_RX_DROP };
typedef struct {
    uint32_t pup;                 /* the puppet object this state belongs to */
    int on, exit_quiet;           /* exit_quiet: the exit must not touch the puppet (scripted here, dead / down: their own paths) */
    uint32_t last_tail_ms, hold_t;
    uint16_t last_seq;
    int have_seq;
    uint32_t s_t;                 /* the shown time (cp_state.t_ms of an interpolated sample) at which the current sample became the shown one */
    uint16_t set_id[PM_LAYERS];   /* what this mirror set last on the layer (PM_NO_ID: stopped / nothing) */
    uint32_t set_t[PM_LAYERS];
    uint8_t resets[PM_LAYERS];
    uint16_t refused_id[PM_LAYERS];   /* the target this layer refused or gave up: not tried again until the owner's id changes */
    uint8_t stop_gaveup[PM_LAYERS];   /* the puppet keeps filling a layer the owner has empty: left to it until the owner plays that layer */
    uint32_t touched;             /* layers set or stopped in this episode */
    uint16_t r_id[PM_LAYERS];     /* frame rate from the owner's consecutive samples */
    float r_frame[PM_LAYERS], rate[PM_LAYERS];
    uint32_t r_t[PM_LAYERS];
    uint32_t bank_defer_t;        /* when the give-back of bank 15 started to wait for the layers (0 = not waiting) */
    uint32_t n_enter, n_exit, n_drop, n_hold, n_rehold, n_bank_wait, n_bank_forced;
    uint32_t n_set, n_resync, n_reset, n_gaveup, n_stop, n_keep, n_ref_motion, n_ref_frames, n_ref_storage, n_ref_bank;
    pm_bank_t bank;
} pm_rx_t;

static inline void pm_rx_episode(pm_rx_t *r)
{
    for (unsigned l = 0; l < PM_LAYERS; l++)
        r->set_id[l] = r->refused_id[l] = r->r_id[l] = PM_NO_ID, r->set_t[l] = 0, r->resets[l] = 0, r->stop_gaveup[l] = 0, r->rate[l] = 0, r->r_t[l] = 0, r->r_frame[l] = 0;
    r->touched = 0, r->have_seq = 0;
}
static inline void pm_rx_reset(pm_rx_t *r)
{
    memset(r, 0, sizeof *r);
    pm_rx_episode(r);
}

typedef struct {
    uint32_t pup;        /* the puppet object now (0 = none) */
    uint32_t own_act;    /* the shown sample's action */
    int tail;            /* the shown sample carries a motion tail */
    int enabled;
    int scripted;        /* the puppet stands in INT_MIN on THIS PC (a replayed co-op door, an enemy's grab): the stock event owns it */
    int dead_down;       /* CP_SF_DEAD / CP_SF_DOWN */
    int held_enemy;      /* the owner is HELD and its tail is no set model's */
    uint32_t now;
} pm_rx_in;

/* one frame: what the mirror does. DROP = the puppet object changed or is gone while the mirror ran: its state is dropped at once, no call on the old object (pm_bank_end with
 * puppet_ok 0). EXIT: the caller gives bank 15 back, stops the layers >= 4 it set (unless exit_quiet) and lets the action mirror (pa_decide, guard reset) switch the puppet. */
static inline int pm_rx_decide(pm_rx_t *r, const uint32_t bits[8], const pm_rx_in *in)
{
    if (r->on && in->pup != r->pup) {
        r->on = 0, r->n_drop++;
        return PM_RX_DROP;
    }
    if (!r->on) r->pup = in->pup;
    if (in->tail) r->last_tail_ms = in->now;
    int in_mask = pa_anim_allowed(bits, in->own_act);
    int block = !in->enabled || !in->pup || in->scripted || in->dead_down || in->held_enemy;
    if (!r->on) {
        if (block || in_mask || !in->tail || !pm_enter_id(in->own_act)) return PM_RX_OFF;
        pm_rx_episode(r);
        r->on = 1, r->exit_quiet = 0, r->hold_t = 0, r->bank_defer_t = 0, r->n_enter++;
        return PM_RX_ENTER;
    }
    int lost = !in->tail && in->now - r->last_tail_ms > PM_TAIL_LOST_MS;
    if (block || in_mask || lost || !pm_cont_id(in->own_act)) {
        r->on = 0, r->n_exit++;
        r->exit_quiet = in->scripted || in->dead_down || !in->pup;
        return PM_RX_EXIT;
    }
    return PM_RX_RUN;
}

/* after the exit (the mirror off, bank 15 still ours): 0 = wait (a puppet layer still plays a motion of bank 15), 1 = give it back now, 2 = stop the layers that still play bank 15,
 * then give it back (the deferral ran out) */
static inline int pm_bank_due(pm_rx_t *r, const uint16_t ids[PM_LAYERS], uint32_t now)
{
    if (!r->bank.active || r->on) return 0;
    if (!pm_ids_use_bank(ids, PM_BANK_EVENT)) {
        r->bank_defer_t = 0;
        return 1;
    }
    if (!r->bank_defer_t) r->bank_defer_t = now ? now : 1, r->n_bank_wait++;
    if (now - r->bank_defer_t >= PM_BANK_DEFER_MS) {
        r->bank_defer_t = 0, r->n_bank_forced++;
        return 2;
    }
    return 0;
}

/* the hold: 1 = ChangeAction(0) on the puppet now (at the entry, again at most once a second when the puppet left 0 by itself) */
static inline int pm_rx_hold(pm_rx_t *r, uint32_t pup_act, uint32_t now)
{
    if (pup_act == 0) return 0;
    if (r->hold_t && now - r->hold_t < PM_HOLD_RETRY_MS) return 0;
    if (r->hold_t) r->n_rehold++;
    else r->n_hold++;
    r->hold_t = now ? now : 1;
    return 1;
}

/* a new sample (its sequence): the owner's frame rate of every layer whose id did not change */
static inline int pm_rx_sample(pm_rx_t *r, const cp_state *o)
{
    if (r->have_seq && o->seq == r->last_seq) return 0;
    r->have_seq = 1, r->last_seq = o->seq, r->s_t = o->t_ms;
    for (unsigned i = 0; i < o->mo_n && i < CP_MO_MAX; i++) {
        const cp_motion *m = &o->mo[i];
        unsigned l = m->layer;
        if (l >= PM_LAYERS) continue;
        if (r->r_id[l] == m->id && o->t_ms != r->r_t[l]) {
            int32_t dt = (int32_t)(o->t_ms - r->r_t[l]);
            float df = m->frame - r->r_frame[l];
            if (dt > 0 && dt < 1000 && df >= 0) {
                float rt = df / (float)dt;
                r->rate[l] = rt > PM_RATE_MAX ? PM_RATE_MAX : rt;
            }
        } else if (r->r_id[l] != m->id)
            r->rate[l] = -1.0f;   /* not measured yet for this id */
        r->r_id[l] = m->id, r->r_frame[l] = m->frame, r->r_t[l] = o->t_ms;
    }
    return 1;
}
/* the owner's frame of a layer at the shown time (cs_remote_sample gives the older sample with t_ms = the shown time: the frame is projected from the time that sample
 * became the shown one, at most PM_EXTRA_MS) */
static inline float pm_frame_now(const pm_rx_t *r, const cp_motion *m, uint32_t shown_t)
{
    int32_t dt = (int32_t)(shown_t - r->s_t);
    if (dt < 0) dt = 0;
    if (dt > (int32_t)PM_EXTRA_MS) dt = PM_EXTRA_MS;
    float rate = m->layer < PM_LAYERS && r->rate[m->layer] >= 0 && r->r_id[m->layer] == m->id ? r->rate[m->layer] : PM_RATE_DEF * (m->speed > 0 ? m->speed : 0);
    return m->frame + rate * (float)dt;
}
static inline float pm_drift(float a, float b, unsigned nframes)
{
    float d = fabsf(a - b);
    if (nframes > 1) {
        float m = fmodf(d, (float)nframes);
        float o = (float)nframes - m;
        d = m < o ? m : o;
    }
    return d;
}

enum { PM_L_NONE = 0, PM_L_SET, PM_L_RESYNC, PM_L_KEEP, PM_L_STOP, PM_L_REFUSE };
enum { PM_WHY_NONE = 0, PM_WHY_MOTION, PM_WHY_FRAMES, PM_WHY_STORAGE, PM_WHY_BANK, PM_WHY_LOST };
typedef struct {
    int allowed;             /* the layer is in `net motion layers` */
    int have;                /* the owner plays this layer */
    uint16_t oid, onframes;
    float oframe;            /* the owner's frame now (pm_frame_now) */
    uint16_t pid;            /* the puppet's layer id */
    float pframe;
    uint16_t found_frames;   /* frame count of the puppet's own motion with oid (0 = not found: no list in that bank / index out of the table / no motion) */
    int storage_ok;          /* the layer has its per-bone storage (or the model has no bones) */
    int bank_ok;             /* bank-15 ids: the set model's list is in the puppet's bank 15 */
    int new_sample;
    uint32_t now;
} pm_layer_in;

/* one layer, one frame. Refusals are sticky until the owner's id on the layer changes (never a retry loop); a layer the puppet changes back after our set is set again at
 * most PM_RESET_MAX times (>= PM_RESET_GAP_MS apart), then left to the puppet */
static inline int pm_layer_plan(pm_rx_t *r, unsigned l, const pm_layer_in *in, uint8_t *why)
{
    *why = PM_WHY_NONE;
    if (l >= PM_LAYERS || !in->allowed) return PM_L_NONE;
    uint16_t target = in->have ? in->oid : PM_NO_ID;
    if (r->refused_id[l] != PM_NO_ID && r->refused_id[l] != target) r->refused_id[l] = PM_NO_ID;   /* the owner moved on: may be tried again */
    if (target != PM_NO_ID && r->refused_id[l] == target) return PM_L_NONE;
    if (target != PM_NO_ID) r->stop_gaveup[l] = 0;
    if (target == PM_NO_ID) {
        if (r->stop_gaveup[l]) return PM_L_NONE;
        if (in->pid == PM_NO_ID) {
            if (!(r->touched >> l & 1u)) return PM_L_NONE;
            r->set_id[l] = PM_NO_ID;
            return PM_L_NONE;
        }
        if (r->set_id[l] == PM_NO_ID && (r->touched >> l & 1u)) {   /* stopped before, the puppet put something there again */
            if (in->now - r->set_t[l] < PM_RESET_GAP_MS) return PM_L_NONE;
            if (++r->resets[l] > PM_RESET_MAX) {
                r->n_gaveup++, *why = PM_WHY_LOST;
                r->stop_gaveup[l] = 1;   /* left to the puppet until the owner plays this layer */
                return PM_L_NONE;
            }
            r->n_reset++;
        } else
            r->resets[l] = 0;
        r->set_id[l] = PM_NO_ID, r->set_t[l] = in->now, r->touched |= 1u << l, r->n_stop++;
        return PM_L_STOP;
    }
    if (!in->found_frames) *why = PM_WHY_MOTION;
    else if (in->onframes && in->found_frames != in->onframes) *why = PM_WHY_FRAMES;
    else if (!in->storage_ok) *why = PM_WHY_STORAGE;
    else if (((target >> 8) & 0xFu) == PM_BANK_EVENT && !in->bank_ok) *why = PM_WHY_BANK;
    if (*why) {
        r->refused_id[l] = target;
        if (*why == PM_WHY_MOTION) r->n_ref_motion++;
        else if (*why == PM_WHY_FRAMES) r->n_ref_frames++;
        else if (*why == PM_WHY_STORAGE) r->n_ref_storage++;
        else r->n_ref_bank++;
        return PM_L_REFUSE;
    }
    if (in->pid != target) {
        if (r->set_id[l] == target) {   /* we set it, the puppet changed it */
            if (in->now - r->set_t[l] < PM_RESET_GAP_MS) return PM_L_NONE;
            if (++r->resets[l] > PM_RESET_MAX) {
                r->refused_id[l] = target, r->n_gaveup++, *why = PM_WHY_LOST;
                return PM_L_REFUSE;
            }
            r->n_reset++;
        } else
            r->resets[l] = 0, r->n_set++;
        r->set_id[l] = target, r->set_t[l] = in->now, r->touched |= 1u << l;
        return PM_L_SET;
    }
    if (r->set_id[l] != target) r->set_id[l] = target, r->resets[l] = 0, r->touched |= 1u << l;   /* already the same (the hold's own motion, an earlier set) */
    if (in->new_sample && pm_drift(in->pframe, in->oframe, in->onframes) > PM_DRIFT) {
        r->set_t[l] = in->now, r->n_resync++;
        return PM_L_RESYNC;
    }
    r->n_keep++;
    return PM_L_KEEP;
}

/* ---- the set model's layer-0 motion in the DOOR event (after the 31-byte stage-10 tail): magic 'L', id u16, nframes u16, frame f32, speed f32, flags u32 ---- */
#define PM_SM_MAGIC 0x4Cu
#define PM_SM_OFF 31u
#define PM_SM_LEN 17u
typedef struct { uint16_t id, nframes; float frame, speed; uint32_t flags; } pm_sm_t;
static inline unsigned pm_sm_enc(uint8_t *b, const pm_sm_t *m)
{
    b[0] = PM_SM_MAGIC, b[1] = (uint8_t)(m->id & 0xFF), b[2] = (uint8_t)(m->id >> 8), b[3] = (uint8_t)(m->nframes & 0xFF), b[4] = (uint8_t)(m->nframes >> 8);
    memcpy(b + 5, &m->frame, 4), memcpy(b + 9, &m->speed, 4), memcpy(b + 13, &m->flags, 4);
    return PM_SM_LEN;
}
/* body = the whole DOOR body: 1 = a valid tail */
static inline int pm_sm_dec(const uint8_t *body, unsigned len, pm_sm_t *m)
{
    if (!body || len < PM_SM_OFF + PM_SM_LEN || body[PM_SM_OFF] != PM_SM_MAGIC) return 0;
    const uint8_t *b = body + PM_SM_OFF;
    m->id = (uint16_t)(b[1] | (b[2] << 8)), m->nframes = (uint16_t)(b[3] | (b[4] << 8));
    memcpy(&m->frame, b + 5, 4), memcpy(&m->speed, b + 9, 4), memcpy(&m->flags, b + 13, 4);
    if (m->id == PM_NO_ID || !isfinite(m->frame) || !isfinite(m->speed) || fabsf(m->frame) > 100000.0f || fabsf(m->speed) > 100.0f) return 0;
    return 1;
}
/* the steps whose motion travels: exactly the ones the receiver does not replay because they need the operating character (os_door_apply, rel032's rule, unchanged) */
static inline int pm_sm_wanted(int kind, int act)
{
#ifdef PM_MODEL_BEFORE
    (void)kind, (void)act;
    return 0;
#else
    return (kind == OS_LEVER_OP || kind == OS_BOARD_OP || kind == OS_ROCK_OP) && !os_door_apply(kind, act, OS_ACT_NONE);
#endif
}
#endif
