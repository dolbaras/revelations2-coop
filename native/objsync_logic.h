/* objsync_logic.h: the pure part of the object sync of stage 10 (gaps G1-G4 of the Episode 1 maps), so tests/test_objsync.c can replay it without a game.
 *
 * G1  co-op set models (uSmDoorCoop / uSmShutterCoop / uSmPairCoop, all uSmCoopBase): a human's press is replayed on the other PC for the presser's puppet. Static facts (notes\objsync.md 2):
 *     the action tables (vtable +0x138 table, +0x13C count 7) are the same for the three classes: 0 Closed / Wait, 1 SyncPostion, 2 LoadWait, 3 Opening (door: 4 NpcAction), so a press is the change
 *     0 -> 1 for every one of them; the stock press handler FUN_0074F9E0(set model, slot) writes +0x4230[slot] = 1, calls the virtual +0x1F4 (the shutter's resets its opening side +0x43C4), picks the
 *     type 1 (4 only for the door with an AI partner) and calls Act +0x170. The partner check FUN_007548A0 (from the SyncPostion start 0x74DF60) refuses while mAccessType +0x43A4 == 1.
 * G2  movable set models (uSmMoveBox family): push boxes uSmPress (uSm1805 in 101 / 151: actions 0 Wait, 1..4 Press with a direction), carried objects uSmInteractive (uSm1160 holdable boxes, uSm1814 /
 *     uSm1928 bricks: 0 Wait, 1 Pickup, 2 / 3 / 5 Break, 4 Broken). Their action change must not be replayed with ChangeAction (the Press start 0x7DBCF0 calls the actor's +0x36C: the actor is 0 in a bare
 *     ChangeAction); their position is mirrored instead (PROP): the PC whose human is the action's actor sends, the other PC writes the transform.
 * G3  scenario flags 300..511 of the current area and the EpisodeFlags (global flags 99000 + n, the box checkers of 101 write [2] and [3]) are scanned as bit words.
 * G4  a flag that rose inside the quiet window after a load is carried by one digest at the end of the window; a digest of another area waits up to OS_PEND_MS for this PC's area to match.
 *
 * Build the model of the release-020 behaviour with -DOS_MODEL_RELEASE020: tests/test_objsync.c must FAIL with it (tests/build.sh checks that), i.e. the checks describe what the old code did not do. */
#ifndef OBJSYNC_LOGIC_H
#define OBJSYNC_LOGIC_H

#include <string.h>

/* ---------------------------------------------------------------- class kinds */
enum os_kind {
    OS_PLAIN = 0,      /* door, locker, lever, ...: the DOOR action mirror as before */
    OS_COOP_DOOR,      /* uSmDoorCoop (sm0402): press replay, proven on the pair (stage 6f) */
    OS_COOP_SHUTTER,   /* uSmShutterCoop (sm0418): the co-op shutters of the stage borders */
    OS_COOP_PAIR,      /* uSmPairCoop: two-person slides, boosts */
    OS_COOP_OTHER,     /* other uSmCoopBase / uSmCoopDemo classes */
    OS_MOVE_PRESS,     /* uSmPress (push boxes) */
    OS_MOVE_CARRY,     /* uSmInteractive (holdable boxes, bricks) */
    OS_MOVE_OTHER      /* another uSmMoveBox class */
};

/* a whole class name in the chain text of classes_name ("vtable exe+.., DTI ..: uSm0418(43d0) : uSmShutterCoop(43d0) : ..."): the name is followed by '(' */
static int os_has_class(const char *chain, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = chain; (p = strstr(p, name)) != NULL; p += n) {
        int start = p == chain || p[-1] == ' ' || p[-1] == ':';
        if (start && p[n] == '(') return 1;
    }
    return 0;
}

static int os_kind_of(const char *chain)
{
    if (!chain) return OS_PLAIN;
    const char *p = strstr(chain, ": ");
    p = p ? p + 2 : chain;
    if (os_has_class(p, "uSmDoorCoop")) return OS_COOP_DOOR;
    if (os_has_class(p, "uSmShutterCoop")) return OS_COOP_SHUTTER;
    if (os_has_class(p, "uSmPairCoop")) return OS_COOP_PAIR;
    if (strstr(p, "Coop")) return OS_COOP_OTHER;   /* the old rule of door.inc: any class name with "Coop" */
#ifdef OS_MODEL_RELEASE020
    return OS_PLAIN;
#else
    if (os_has_class(p, "uSmPress")) return OS_MOVE_PRESS;
    if (os_has_class(p, "uSmInteractive")) return OS_MOVE_CARRY;
    if (os_has_class(p, "uSmMoveBox")) return OS_MOVE_OTHER;
    return OS_PLAIN;
#endif
}

static int os_is_coop(int k) { return k >= OS_COOP_DOOR && k <= OS_COOP_OTHER; }
static int os_is_move(int k) { return k >= OS_MOVE_PRESS && k <= OS_MOVE_OTHER; }

/* ---------------------------------------------------------------- DOOR action mirror policy */
enum os_actor { OS_ACT_NONE = 0, OS_ACT_LOCAL = 1, OS_ACT_REMOTE = 2 };   /* who is the actor of the set model's current action instance (+0x1A68 -> +0xC) */

static int os_carry_break_act(int act) { return act == 2 || act == 3 || act == 4 || act == 5; }   /* uSmInteractive: Break (2, 3, 5) and Broken (4) need no actor (starts 0x7D29B0 / 0x7D2A30) */

/* the sender's scan saw the action id of a set model change: send DOOR? (coop_mode = `net w doors coop`, 0 = co-op set models are not mirrored) */
static int os_door_send(int kind, int act, int actor, int coop_mode)
{
    if (os_is_coop(kind)) {
        if (!coop_mode) return 0;
#ifdef OS_MODEL_RELEASE020
        (void)actor;
        return 1;
#else
        return actor != OS_ACT_REMOTE;   /* the actor is the partner's puppet: this change is our own replay of the partner's press (the echo) */
#endif
    }
#ifndef OS_MODEL_RELEASE020
    if (kind == OS_MOVE_CARRY) return os_carry_break_act(act) && actor != OS_ACT_REMOTE;
    if (os_is_move(kind)) return 0;
#endif
    (void)act, (void)actor;
    return 1;
}

/* the receiver got DOOR for a set model that is not co-op: apply it with ChangeAction(act, 1, 0, -1, 0)? (local_actor: the actor of this PC's copy) */
static int os_door_apply(int kind, int act, int local_actor)
{
#ifndef OS_MODEL_RELEASE020
    if (kind == OS_MOVE_CARRY) return os_carry_break_act(act) && local_actor != OS_ACT_LOCAL;
    if (os_is_move(kind)) return 0;
#endif
    (void)kind, (void)act, (void)local_actor;
    return 1;
}

/* the scan saw a co-op set model go to act; rp_t = when this PC replayed the partner's press on it (0 = never), actor as in os_door_send. Pair run osPB 2026-11-07 07:22:56
 * (copy 1, the north shutter (151,160,102), `work\runs\20261107-070911-osPB\n100\rev2_coop.log` 641-644): the replay set the action id 0 -> 1 but made no new action instance
 * (smtrace: no +1A68 / act+C change; the presser's puppet stood far away, both humans dead), so the instance's actor was not the puppet and the actor rule let the scan send the
 * replay's own 0 -> 1 back (the partner's copy refused it only because it was busy). Within OS_ECHO_MS of a replay the press transition (-> 1) without a LOCAL actor is ours.
 * -DOS_MODEL_BEFORE_OSPB = the logic before this rule (tests/build.sh checks that test_objsync fails with it). */
#define OS_ECHO_MS 2000u
static int os_replay_echo(int kind, int act, int actor, unsigned rp_t, unsigned now)
{
#if defined(OS_MODEL_RELEASE020) || defined(OS_MODEL_BEFORE_OSPB)
    (void)kind, (void)act, (void)actor, (void)rp_t, (void)now;
    return 0;
#else
    return os_is_coop(kind) && act == 1 && actor != OS_ACT_LOCAL && rp_t != 0 && now - rp_t < OS_ECHO_MS;
#endif
}

/* ---------------------------------------------------------------- co-op press replay */
enum os_replay { OS_RP_SKIP_NOTPRESS = 0, OS_RP_SKIP_NOPRESSER, OS_RP_SKIP_BUSY, OS_RP_ACT, OS_RP_FULL };
#define OS_PRESS_ACT 1   /* SyncPostion: the action a press puts every co-op class in (action tables 0x12E8810, 0x12F02D0, 0x12EF208) */

/* act = the sender's new action, local_act = this PC's copy (0 = waiting), presser_ok = the presser's puppet exists here and is not this PC's human, mode = `net w doors coop` (2 default, 3 = full press for all) */
static int os_coop_plan(int kind, int act, int local_act, int presser_ok, int mode)
{
    if (act != OS_PRESS_ACT) return OS_RP_SKIP_NOTPRESS;
    if (!presser_ok) return OS_RP_SKIP_NOPRESSER;
    if (local_act != 0) return OS_RP_SKIP_BUSY;   /* out of the waiting action: this PC's own press or chain runs (6f: a second press cancelled the chain) */
#ifdef OS_MODEL_RELEASE020
    (void)kind, (void)mode;
    return OS_RP_ACT;
#else
    if (mode >= 3) return OS_RP_FULL;
    return kind == OS_COOP_SHUTTER || kind == OS_COOP_PAIR ? OS_RP_FULL : OS_RP_ACT;   /* the door keeps the call proven in 6f (Act only) */
#endif
}

/* mAccessType (+0x43A4) of this PC's copy: 1 = "NG" (SmCoopAccess(1) of the FSM) makes the partner check refuse the replayed press although the presser's PC accepted it */
static int os_access_override(int access, int enabled)
{
#ifdef OS_MODEL_RELEASE020
    (void)access, (void)enabled;
    return 0;
#else
    return enabled && access == 1;
#endif
}
#define OS_ACCESS_HOLD_MS 2000u
/* restore the overridden value once the copy left its waiting action (the partner check of the SyncPostion start has run) or after OS_ACCESS_HOLD_MS */
static int os_access_restore_due(unsigned since, unsigned now, int local_act) { return local_act != 0 || now - since >= OS_ACCESS_HOLD_MS; }

/* a co-op event for a layout id that is not active here (a border shutter has one layout id per stage list: (153,160,0) = (151,160,135)): another co-op set model of the same kind this close is the same shutter */
#define OS_ALIAS_DIST 150.0f
static int os_alias_ok(int kind_sent, int kind_here, float dx, float dy, float dz)
{
#ifdef OS_MODEL_RELEASE020
    (void)kind_sent, (void)kind_here, (void)dx, (void)dy, (void)dz;
    return 0;
#else
    return os_is_coop(kind_sent) && kind_sent == kind_here && dx * dx + dy * dy + dz * dz <= OS_ALIAS_DIST * OS_ALIAS_DIST;
#endif
}

/* ---------------------------------------------------------------- PROP: transforms of movable set models */
typedef struct { float p[3], q[4]; } os_xf_t;   /* position +0x40..+0x48, rotation quaternion +0x50..+0x5C */
#define OS_EPS_POS 1.0f          /* units: below this a change is noise */
#define OS_EPS_ROT 0.0005f       /* 1 - |dot| of the quaternions */
#define OS_PROP_MS 100u          /* at most 10 samples per second and object */
#define OS_SETTLE_MS 2000u       /* after the local actor let go: the push end snaps the box to the ground (FUN_007DFD20), keep sending the changes, then one final REST */
#define OS_REMOTE_MS 2000u       /* a remote sample makes the remote PC the owner this long (this PC's scan stays silent) */
enum os_prop_flags { OS_PF_MOVE = 1, OS_PF_REST = 2, OS_PF_FINAL = 4 };
enum os_owner { OS_OWN_NONE = 0, OS_OWN_LOCAL = 1, OS_OWN_REMOTE = 2 };

typedef struct {
    os_xf_t base;                 /* last sent / applied / learned transform */
    unsigned last_tx, own_until, rx_until;
    unsigned short seq_tx, seq_rx;
    unsigned char owner, have_rx, final_due;
} os_prop_t;

static float os_absf(float v) { return v < 0 ? -v : v; }

static int os_xf_moved(const os_xf_t *a, const os_xf_t *b)
{
    float dx = a->p[0] - b->p[0], dy = a->p[1] - b->p[1], dz = a->p[2] - b->p[2];
    if (dx * dx + dy * dy + dz * dz > OS_EPS_POS * OS_EPS_POS) return 1;
    float dot = a->q[0] * b->q[0] + a->q[1] * b->q[1] + a->q[2] * b->q[2] + a->q[3] * b->q[3];
    return 1.0f - os_absf(dot) > OS_EPS_ROT;
}

static void os_prop_learn(os_prop_t *e, const os_xf_t *cur)
{
    memset(e, 0, sizeof *e);
    e->base = *cur;
}

/* one scan of one learned object; actor = who acts on it (OS_ACT_*); returns the flags of a sample to send (0 = none); the caller sends e->base with seq e->seq_tx */
static int os_prop_tick(os_prop_t *e, const os_xf_t *cur, int actor, unsigned now)
{
#ifdef OS_MODEL_RELEASE020
    (void)e, (void)cur, (void)actor, (void)now;
    return 0;
#else
    int moved = os_xf_moved(cur, &e->base);
    if (actor == OS_ACT_LOCAL) {
        e->owner = OS_OWN_LOCAL, e->own_until = now + OS_SETTLE_MS, e->final_due = 1;
        if (moved && now - e->last_tx >= OS_PROP_MS) {
            e->base = *cur, e->last_tx = now, e->seq_tx++;
            return OS_PF_MOVE;
        }
        return 0;
    }
    if (e->owner == OS_OWN_LOCAL) {
        if ((int)(now - e->own_until) < 0) {
            if (moved && now - e->last_tx >= OS_PROP_MS) {
                e->base = *cur, e->last_tx = now, e->seq_tx++;
                return OS_PF_REST;
            }
            return 0;
        }
        e->owner = OS_OWN_NONE;
        if (e->final_due) {
            e->final_due = 0;
            e->base = *cur, e->last_tx = now, e->seq_tx++;
            return OS_PF_REST | OS_PF_FINAL;
        }
        return 0;
    }
    if (e->owner == OS_OWN_REMOTE && (int)(now - e->rx_until) >= 0) e->owner = OS_OWN_NONE;
    if (moved) e->base = *cur;   /* the story / the engine moved it on this PC too (both PCs run it), or the last remote sample settled: nothing to send */
    return 0;
#endif
}

/* sequence numbers wrap: newer = within half the range ahead */
static int os_seq_newer(unsigned short a, unsigned short b) { return (short)(a - b) > 0; }

/* the receiver got a sample: apply it to this PC's copy? local_actor = the actor of this PC's copy */
static int os_prop_accept(os_prop_t *e, unsigned short seq, int local_actor, unsigned now)
{
#ifdef OS_MODEL_RELEASE020
    (void)e, (void)seq, (void)local_actor, (void)now;
    return 0;
#else
    if (local_actor == OS_ACT_LOCAL || e->owner == OS_OWN_LOCAL) return 0;   /* this PC's human holds / pushes it, or just let go: its own state wins here (both PCs keep their own until one lets go) */
    if (e->have_rx && !os_seq_newer(seq, e->seq_rx)) return 0;
    e->have_rx = 1, e->seq_rx = seq;
    e->owner = OS_OWN_REMOTE, e->rx_until = now + OS_REMOTE_MS;
    return 1;
#endif
}

static void os_prop_applied(os_prop_t *e, const os_xf_t *xf) { e->base = *xf; }

/* A carried object (uSmInteractive, action 1 = Pickup / held) is attached to its holder: its +0x40 / +0x50 then hold the position / rotation RELATIVE to the holder
 * (pair runs wpB2c 17:42 (154,1,4) and wpB3c 22:40:14 (151,1,0): (-81.2,-74.4,10.5) on the carrier's PC). Sent as they are, the partner's PC wrote them as a world position: its
 * copy of the box left the room (to near the world origin) for the whole carry and came back only with the put-down. The sample to send for one scan:
 *   - not carried / not held: the raw +0x40 / +0x50;
 *   - held, the raw position lies at the holder (within OS_CARRY_NEAR: before it is attached, at the put-down): the raw transform;
 *   - held, attached: the root matrix translation (+0xE0) when THAT lies at the holder, with the last world rotation (the relative one would turn the partner's copy);
 *   - otherwise (no holder known, neither position at the holder): no sample (returns 0; the caller keeps its base, the partner's copy stays where it was until the rest).
 * holder = the world position of the action's actor (+0x40 of the player), NULL when unknown. -DOS_MODEL_BEFORE_WPB3C = the logic before this rule (raw always). */
#define OS_CARRY_NEAR 400.0f
static int os_near(const float a[3], const float b[3], float r)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz <= r * r;
}
/* Attached = the box hangs on its holder: +0x40 is then relative (wpB3d: (-81.2,-74.4,10.5)) while the root matrix translation +0xE0 is the world position; a free object has
 * +0x40 == +0xE0 (the world matrix of an object without a parent is its own transform; os_write_xf writes both). */
#define OS_ATTACH_EPS 50.0f
static int os_carry_attached(const os_xf_t *raw, const float mat_t[3])
{
    return mat_t && !os_near(raw->p, mat_t, OS_ATTACH_EPS);
}

/* Who acts on a carried box NOW. The game keeps the box in action 1 with the carrier as the action's actor after the put-down (pair run wpB3d: 23:13:18 (151,1,0) on the
 * checker and 23:20:15 (154,1,4) 20 s after the put-down, both "act 1 actor LOCAL" with +0x40 = the world position). Taken as it is, the carrier's PC stayed the box's local
 * owner for good: it ignored every later sample of the partner (wpB3d 23:21:42: Natalia picked (154,1,4) up on her PC and carried it to x 27766, copy 2 sent 18 samples,
 * copy 1 applied none and kept the box at 27507.7) and never sent the final rest of its own put-down. So the actor of a carried box counts only while it is attached.
 * -DOS_MODEL_BEFORE_WPB3D = the rule before (the action's actor as it is). */
static int os_carry_actor(int kind, int act, int actor, const os_xf_t *raw, const float mat_t[3])
{
#ifdef OS_MODEL_BEFORE_WPB3D
    (void)kind, (void)act, (void)raw, (void)mat_t;
    return actor;
#else
    if (kind != OS_MOVE_CARRY || act != 1 || actor == OS_ACT_NONE || !mat_t) return actor;
    return os_carry_attached(raw, mat_t) ? actor : OS_ACT_NONE;
#endif
}

static int os_carry_sample(int kind, int act, const os_xf_t *raw, const float mat_t[3], const float *holder, const os_xf_t *base, os_xf_t *out)
{
    *out = *raw;
#ifdef OS_MODEL_BEFORE_WPB3C
    (void)kind, (void)act, (void)mat_t, (void)holder, (void)base;
    return 1;
#else
    if (kind != OS_MOVE_CARRY || act != 1) return 1;
#ifndef OS_MODEL_BEFORE_WPB3D
    if (mat_t && !os_carry_attached(raw, mat_t)) return 1;   /* free (before the attach, after the put-down): +0x40 is the world position */
#endif
    if (!holder) return 0;
    if (os_near(raw->p, holder, OS_CARRY_NEAR)) return 1;
    if (mat_t && os_near(mat_t, holder, OS_CARRY_NEAR)) {
        for (int i = 0; i < 3; i++) out->p[i] = mat_t[i];
        for (int i = 0; i < 4; i++) out->q[i] = base->q[i];
        return 1;
    }
    return 0;
#endif
}

/* event body: i32 a, u16 b, u16 c, u8 flags, u8 sender slot, u16 seq, f32 pos[3], f32 quat[4], i32 action id = 44 bytes */
#define OS_PROP_LEN 44
typedef struct { int a; unsigned b, c; int flags, slot; unsigned short seq; os_xf_t xf; int act; } os_prop_msg_t;

static int os_prop_pack(const os_prop_msg_t *m, unsigned char *b)
{
    memcpy(b, &m->a, 4);
    b[4] = (unsigned char)(m->b & 0xFF), b[5] = (unsigned char)(m->b >> 8), b[6] = (unsigned char)(m->c & 0xFF), b[7] = (unsigned char)(m->c >> 8);
    b[8] = (unsigned char)m->flags, b[9] = (unsigned char)m->slot;
    b[10] = (unsigned char)(m->seq & 0xFF), b[11] = (unsigned char)(m->seq >> 8);
    memcpy(b + 12, m->xf.p, 12), memcpy(b + 24, m->xf.q, 16), memcpy(b + 40, &m->act, 4);
    return OS_PROP_LEN;
}

static int os_prop_unpack(const unsigned char *b, int len, os_prop_msg_t *m)
{
    if (len < OS_PROP_LEN) return 0;
    memcpy(&m->a, b, 4);
    m->b = b[4] | (b[5] << 8), m->c = b[6] | (b[7] << 8);
    m->flags = b[8], m->slot = b[9];
    m->seq = (unsigned short)(b[10] | (b[11] << 8));
    memcpy(m->xf.p, b + 12, 12), memcpy(m->xf.q, b + 24, 16), memcpy(&m->act, b + 40, 4);
    for (int i = 0; i < 3; i++)
        if (!(m->xf.p[i] > -1.0e6f && m->xf.p[i] < 1.0e6f)) return 0;   /* also rejects NaN */
    for (int i = 0; i < 4; i++)
        if (!(m->xf.q[i] >= -1.01f && m->xf.q[i] <= 1.01f)) return 0;
    return 1;
}

/* ---------------------------------------------------------------- scenario flags as bit words */
#define OS_SF_WORDS 16                 /* 512 flags: an index below 0x200 is the current area (FUN_00975B50) */
#define OS_EP_AREA 99                  /* EpisodeFlag[n] = global scenario flag 99000 + n (getter 0x8681B0); marker in the SF event's area field */
#define OS_EP_BASE 99000
#ifdef OS_MODEL_RELEASE020
#define OS_SF_LIMIT 300                /* release-020: SF_N 300 (scen.inc) */
#else
#define OS_SF_LIMIT 512
#endif

/* bit of flag i in its word: 0x80000000 >> (i & 31) of word i >> 5 (FUN_00A90510) */
static int os_sf_bit(const unsigned *w, int i) { return (w[i >> 5] >> (31 - (i & 31))) & 1; }
static void os_sf_setbit(unsigned *w, int i) { w[i >> 5] |= 0x80000000u >> (i & 31); }

/* flags that rose between two scans, up to max, below OS_SF_LIMIT (episode = 1: the EpisodeFlag block, release-020 never scanned it) */
static int os_sf_rising(const unsigned *prev, const unsigned *cur, int episode, int *out, int max)
{
    int n = 0;
#ifdef OS_MODEL_RELEASE020
    if (episode) return 0;
#endif
    int lim = episode ? OS_SF_WORDS * 32 : OS_SF_LIMIT;
    for (int i = 0; i < lim && n < max; i++)
        if (os_sf_bit(cur, i) && !os_sf_bit(prev, i)) out[n++] = i;
    return n;
}

/* the receiver's range check of a single SF event (index of the current area, or of the EpisodeFlag block) */
static int os_sf_index_ok(int idx) { return idx >= 0 && idx < OS_SF_LIMIT; }

/* digest: i32 area (OS_EP_AREA for the EpisodeFlags), u8 kind (0 area, 1 episode), u8 word count, u32 words[] */
#define OS_DIGEST_LEN (6 + 4 * OS_SF_WORDS)
static int os_digest_pack(int area, int kind, const unsigned *w, unsigned char *b)
{
    memcpy(b, &area, 4);
    b[4] = (unsigned char)kind, b[5] = OS_SF_WORDS;
    memcpy(b + 6, w, 4 * OS_SF_WORDS);
    return OS_DIGEST_LEN;
}

static int os_digest_unpack(const unsigned char *b, int len, int *area, int *kind, unsigned *w)
{
    if (len < 6) return 0;
    int nw = b[5];
    if (nw < 1 || nw > OS_SF_WORDS || len < 6 + 4 * nw) return 0;
    memcpy(area, b, 4);
    *kind = b[4];
    if (*kind > 1) return 0;
    memset(w, 0, 4 * OS_SF_WORDS);
    memcpy(w, b + 6, 4 * (size_t)nw);
    return 1;
}

/* flags set in the digest and clear here (the receiver sets them: sets only, as the SF sync does) */
static int os_digest_missing(const unsigned *digest, const unsigned *local, int lim, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < lim && i < OS_SF_WORDS * 32 && n < max; i++)
        if (os_sf_bit(digest, i) && !os_sf_bit(local, i)) out[n++] = i;
    return n;
}

/* the digest is due at the first scan after the quiet window (in_window: this scan took a new baseline) */
static int os_digest_due(int was_in_window, int in_window)
{
#ifdef OS_MODEL_RELEASE020
    (void)was_in_window, (void)in_window;
    return 0;
#else
    return was_in_window && !in_window;
#endif
}

/* one scan after the baseline: send the digest now? *pending = a baseline was taken and its digest is not out yet; can_send = the session is in its PLAYING phase (and the
 * switches are on). Pair run osPB 2026-11-07 (`work\runs\20261107-070911-osPB`): at the 153 -> 151 change both copies took the 151 baseline while the glue was "loading"
 * (copy 1 07:16:53.554 running -> loading, barrier open 07:17:16.237); the next scan found the digest due but could not send it, and the old code dropped it: no
 * "SF digest ... area 151" in either log, and copy 2's SF[5] / SF[80] (risen at 07:16:59.937, inside the load) were never sent. Now the digest waits for PLAYING. */
static int os_digest_step(int *pending, int can_send)
{
#if defined(OS_MODEL_RELEASE020)
    *pending = 0;
    (void)can_send;
    return 0;
#elif defined(OS_MODEL_BEFORE_OSPB)
    int due = os_digest_due(*pending, 0);
    *pending = 0;
    return due && can_send;
#else
    if (!*pending || !can_send) return 0;
    *pending = 0;
    return 1;
#endif
}

/* a digest (or SF event) for an area this PC is not in yet (the stage gate lets the two PCs change the area up to some seconds apart): kept this long */
#define OS_PEND_MS 15000u
typedef struct { int area, valid; unsigned t; unsigned w[OS_SF_WORDS]; } os_pend_t;

static void os_pend_store(os_pend_t *p, int area, const unsigned *w, unsigned now)
{
#ifdef OS_MODEL_RELEASE020
    (void)p, (void)area, (void)w, (void)now;
#else
    if (p->valid && p->area == area)
        for (int i = 0; i < OS_SF_WORDS; i++) p->w[i] |= w[i];   /* two digests of the same area: union (sets only) */
    else
        memcpy(p->w, w, sizeof p->w);
    p->area = area, p->valid = 1, p->t = now;
#endif
}

/* returns 1 and the words when this PC is now in the pending digest's area; drops it after OS_PEND_MS */
static int os_pend_take(os_pend_t *p, int area, unsigned now, unsigned *w)
{
    if (!p->valid) return 0;
    if (now - p->t > OS_PEND_MS) {
        p->valid = 0;
        return 0;
    }
    if (p->area != area) return 0;
    memcpy(w, p->w, sizeof p->w);
    p->valid = 0;
    return 1;
}

#endif
