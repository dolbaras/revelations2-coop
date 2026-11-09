/* meleehit_logic.h: the pure part of the client's real hits on enemy replicas (0.2.0 blocker "MHIT", notes\melee.md), tested offline by tests/test_meleehit.c.
 *
 * Why (static, from the image): every real blow on an enemy (knife, kick, stomp, bullet, thrown object) is first stored by the collision code in the enemy's pending
 * DamageInfo (enemy+0x30E0, valid byte +0x31B0 = DI+0xD0; body parts: the part objects at enemy+0x7328[0..9], each with its own DI at +0x30E0). Nothing applies it there:
 * the ONLY reader that turns it into damage is FUN_006001D0, called only from the Intel update uEmIntel_vf46 0x600F50 -> FUN_0061B050 (the hit handler: damage formula,
 * reaction flags, ApplyDamage = vtable +0x3A8, kill report). world.inc skips that update on a client ("replica AI off"), so a client's blow on a replica never reached
 * ApplyDamage: 34 knife slashes, 0 ApplyDamage (G1, 2026-11-07). With `net w ai 1` the replica ran the handler itself (local reaction + forwarded ApplyDamage).
 *
 * The fix keeps the host authoritative and uses the game's own network hit model (Raid: cCommunicatorEnemy_vf6 -> FUN_009D9D10 / FUN_009DB210 rebuild a DI on the
 * receiving PC and run FUN_0061B050; the sending PC compacts the DI with FUN_00ADDCD0, the receiver expands it with FUN_00ADC650). The field list below is exactly the
 * one of that compact pair:
 *   CLIENT: in the skipped Intel update of a replica, a valid pending DI whose attacker (DI+0xDC) is the local human (or an object it owns, +0x19C0) is packed into one
 *           event (EV_W_HITDI, 96 bytes: layout, slot, part, the compact fields; the hit point relative to the enemy). Nothing on the replica is changed.
 *   HOST:   the event is queued; inside the enemy's own Intel update, before its stock part, the DI is rebuilt (DI ctor 0x826ED0) with the attacker = the partner's
 *           puppet and handed to the hit handler directly, the Raid receiver's way (FUN_0061B050 + FUN_00ADD880): the stock damage formula, death, kill report (the stat
 *           guard / DEATH event give the credit to the client's human); the stock update right after picks the reaction from the handler's flags. NOT the mailbox: run M1
 *           (2026-11-07) showed that the mailbox check FUN_0060DFD0 refuses a DI whose attacker is a type-2 player (FUN_009F3B10, the network partner) when enemy+0x6FB8 is
 *           set (the Raid rule: a remote player's hit comes through the receiver). An entry no Intel update took for MH_DIRECT_MS is applied from the game pass. */
#ifndef MELEEHIT_LOGIC_H
#define MELEEHIT_LOGIC_H
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ---- DamageInfo offsets (FUN_00ADC650 / FUN_00ADDCD0, FUN_005FF220 / FUN_005FF370, FUN_0082C2F0 = clear) ---- */
enum {
    DI_DMG = 0x04, DI_FL = 0x28, DI_VALID = 0xD0, DI_ATT = 0xDC, DI_E0 = 0xE0, DI_E4 = 0xE4, DI_E5 = 0xE5, DI_POS = 0xF0, DI_DIR = 0x100, DI_110 = 0x110, DI_114 = 0x114,
    DI_264 = 0x264, DI_A0 = 0xA0, DI_80 = 0x80, DI_AC = 0xAC, DI_A8 = 0xA8, DI_98 = 0x98, DI_SIZE = 0x268
};
#define MH_DI_BUF 0x300u              /* a stack DI is given this much room */
#define MH_ENEMY_DI 0x30E0u           /* enemy + this = the pending DI (the mailbox) */
#define MH_PARTS 0x7328u              /* enemy + this = 10 part object pointers */
#define MH_NPARTS 10
#define MH_OWNER 0x19C0u              /* object + this = its owner (a thrown object's thrower, FUN_00606680) */
#define MH_FL_NODMG_BIT 19            /* DI+0x28 bit 19: FUN_0060DFD0 refuses such a DI */
#define MH_PART_STEP 0x46             /* FUN_006001D0: a part's DI gets +0x114 += (part index + 1) * 0x46 before the handler */

enum { MH_REC_LEN = 96 };
#define MH_REL_SCALE 10.0f           /* the hit point relative to the enemy, int16 in 0.1 units (+-3276 units) */
#define MH_DIRECT_MS 1000u            /* host: an entry no Intel update took this long is applied directly (Raid way) */
#define MH_DROP_MS 3000u              /* host: an entry that still could not be applied is dropped */
#define MH_QMAX 16
#define MH_DEDUPE 64

typedef struct {
    int32_t la; uint16_t lb, lc;      /* the enemy's layout id */
    uint8_t slot, part;               /* attacker slot (the client's human), part 0 = the enemy itself, 1..10 = part index + 1 */
    int32_t dmg;                      /* DI+4 */
    uint32_t fl[3];                   /* DI+0x28 .. +0x33 (96 flag bits) */
    int16_t rel[3];                   /* (DI+0xF0 .. +0xF8) - enemy position, * MH_REL_SCALE */
    float dir[3];                     /* DI+0x100 .. +0x108 */
    uint32_t f110; uint16_t f114;     /* DI+0x110, u16 of DI+0x114 (with the part step added) */
    uint32_t fe0; uint8_t fe4, fe5;   /* DI+0xE0, +0xE4, +0xE5 */
    uint32_t f264; uint16_t fa0, f80; /* DI+0x264, u16 of DI+0xA0, u16 of DI+0x80 (Raid sends both as u16) */
    float fac[6];                     /* DI+0xAC .. +0xC0 */
    float fa8;                        /* DI+0xA8 rounded to an integer value (FUN_00ADDCD0 does the same) */
    uint32_t f98;                     /* DI+0x98 */
    uint8_t clamped;                  /* not sent: 1 when a value did not fit its field */
} mh_rec_t;

static uint32_t mh_rd32(const unsigned char *p, unsigned o) { uint32_t v; memcpy(&v, p + o, 4); return v; }
static float mh_rdf(const unsigned char *p, unsigned o) { float v; memcpy(&v, p + o, 4); return v; }
static void mh_wr32(unsigned char *p, unsigned o, uint32_t v) { memcpy(p + o, &v, 4); }
static void mh_wrf(unsigned char *p, unsigned o, float v) { memcpy(p + o, &v, 4); }

static int16_t mh_rel16(float d, uint8_t *clamped)
{
    float s = d * MH_REL_SCALE;
    if (!(s == s)) { *clamped = 1; return 0; }
    if (s > 32767.0f) { *clamped = 1; return 32767; }
    if (s < -32767.0f) { *clamped = 1; return -32767; }
    return (int16_t)lrintf(s);
}

/* the compact record of a pending DI (as FUN_00ADDCD0 reads it); epos = the enemy position on this PC (+0x40); part 0 or 1..10 */
static void mh_from_di(const unsigned char *di, const float epos[3], int part, mh_rec_t *r)
{
    memset(r, 0, sizeof *r);
    r->part = (uint8_t)part;
    r->dmg = (int32_t)mh_rd32(di, DI_DMG);
    for (int i = 0; i < 3; i++) r->fl[i] = mh_rd32(di, DI_FL + 4 * i);
    for (int i = 0; i < 3; i++) r->rel[i] = mh_rel16(mh_rdf(di, DI_POS + 4 * i) - epos[i], &r->clamped);
    for (int i = 0; i < 3; i++) r->dir[i] = mh_rdf(di, DI_DIR + 4 * i);
    r->f110 = mh_rd32(di, DI_110);
    int32_t f114 = (int32_t)mh_rd32(di, DI_114);
    if (part > 0) f114 += (int16_t)part * MH_PART_STEP;   /* FUN_006001D0: piVar7[0x45] += (short)local_4 * 0x46 */
    r->f114 = (uint16_t)f114;
    r->fe0 = mh_rd32(di, DI_E0);
    r->fe4 = di[DI_E4], r->fe5 = di[DI_E5];
    r->f264 = mh_rd32(di, DI_264);
    uint32_t a0 = mh_rd32(di, DI_A0), x80 = mh_rd32(di, DI_80);
    if (a0 > 0xFFFFu || x80 > 0xFFFFu) r->clamped = 1;
    r->fa0 = (uint16_t)a0, r->f80 = (uint16_t)x80;
    for (int i = 0; i < 6; i++) r->fac[i] = mh_rdf(di, DI_AC + 4 * i);
    r->fa8 = (float)lrintf(mh_rdf(di, DI_A8));
    r->f98 = mh_rd32(di, DI_98);
}

/* the DI the host posts (as FUN_00ADC650 writes it): di was made by the DI constructor (zero flags), attacker = the host's object of that slot */
static void mh_to_di(const mh_rec_t *r, const float epos[3], uint32_t attacker, unsigned char *di)
{
    di[DI_VALID] = 1;
    mh_wr32(di, DI_DMG, (uint32_t)r->dmg);
    for (int i = 0; i < 3; i++) mh_wr32(di, DI_FL + 4 * i, mh_rd32(di, DI_FL + 4 * i) | r->fl[i]);
    for (int i = 0; i < 3; i++) mh_wrf(di, DI_POS + 4 * i, epos[i] + (float)r->rel[i] / MH_REL_SCALE);
    mh_wr32(di, DI_POS + 12, 0);
    for (int i = 0; i < 3; i++) mh_wrf(di, DI_DIR + 4 * i, r->dir[i]);
    mh_wr32(di, DI_DIR + 12, 0);
    mh_wr32(di, DI_110, r->f110);
    mh_wr32(di, DI_114, r->f114);
    mh_wr32(di, DI_E0, r->fe0);
    mh_wr32(di, DI_ATT, attacker);
    di[DI_E4] = r->fe4, di[DI_E5] = r->fe5;
    mh_wr32(di, DI_264, r->f264);
    mh_wr32(di, DI_A0, r->fa0);
    mh_wr32(di, DI_80, r->f80);
    for (int i = 0; i < 6; i++) mh_wrf(di, DI_AC + 4 * i, r->fac[i]);
    mh_wrf(di, DI_A8, r->fa8);
    mh_wr32(di, DI_98, r->f98);
}

/* the wire form (little endian, MH_REC_LEN bytes) */
static void mh_pack(const mh_rec_t *r, unsigned char *b)
{
    unsigned o = 0;
#define MH_P(v, n) (memcpy(b + o, &(v), n), o += n)
    MH_P(r->la, 4); MH_P(r->lb, 2); MH_P(r->lc, 2); MH_P(r->slot, 1); MH_P(r->part, 1);
    MH_P(r->dmg, 4); MH_P(r->fl, 12); MH_P(r->rel, 6); MH_P(r->dir, 12);
    MH_P(r->f110, 4); MH_P(r->f114, 2); MH_P(r->fe0, 4); MH_P(r->fe4, 1); MH_P(r->fe5, 1);
    MH_P(r->f264, 4); MH_P(r->fa0, 2); MH_P(r->f80, 2); MH_P(r->fac, 24); MH_P(r->fa8, 4); MH_P(r->f98, 4);
#undef MH_P
}

static int mh_unpack(const unsigned char *b, unsigned len, mh_rec_t *r)
{
    if (len != MH_REC_LEN) return 0;
    memset(r, 0, sizeof *r);
    unsigned o = 0;
#define MH_U(v, n) (memcpy(&(v), b + o, n), o += n)
    MH_U(r->la, 4); MH_U(r->lb, 2); MH_U(r->lc, 2); MH_U(r->slot, 1); MH_U(r->part, 1);
    MH_U(r->dmg, 4); MH_U(r->fl, 12); MH_U(r->rel, 6); MH_U(r->dir, 12);
    MH_U(r->f110, 4); MH_U(r->f114, 2); MH_U(r->fe0, 4); MH_U(r->fe4, 1); MH_U(r->fe5, 1);
    MH_U(r->f264, 4); MH_U(r->fa0, 2); MH_U(r->f80, 2); MH_U(r->fac, 24); MH_U(r->fa8, 4); MH_U(r->f98, 4);
#undef MH_U
    return r->slot < 2 && r->part <= MH_NPARTS;
}

/* ---- client: is this pending DI the local human's blow? ---- */
enum { MH_TAKE_NO = 0, MH_TAKE_MINE = 1, MH_TAKE_OTHER = 2, MH_TAKE_INVALID = 3 };
typedef struct {
    int valid;                        /* DI+0xD0 */
    uint32_t attacker, attacker_owner;/* DI+0xDC, [attacker+0x19C0] */
    uint32_t mine;                    /* the local human's player object */
    uint32_t fl0;                     /* DI+0x28 */
    int replica_hp;                   /* the replica's hp (from the host's records) */
} mh_take_in_t;

static int mh_take(const mh_take_in_t *in)
{
#ifdef MH_MODEL_BEFORE_FIX
    (void)in;
    return MH_TAKE_NO;                /* rev2-int a8a067e: the skipped update looked at nothing */
#else
    if (!in->valid) return MH_TAKE_NO;
    if (!in->mine || !in->attacker) return MH_TAKE_INVALID;
    if (in->attacker != in->mine && in->attacker_owner != in->mine) return MH_TAKE_OTHER;   /* the partner's puppet, an enemy, the scenery: the host has the real one */
    if ((in->fl0 >> MH_FL_NODMG_BIT) & 1) return MH_TAKE_INVALID;
    if (in->replica_hp <= 0) return MH_TAKE_INVALID;
    return MH_TAKE_MINE;
#endif
}

/* a DI signature over the raw DI fields (absolute hit point: the enemy moves under a DI that stays valid): a DI that stays valid over several updates (no per-frame
 * clear) is sent once; a new blow writes new values */
static uint32_t mh_sig_di(const unsigned char *di)
{
    static const unsigned short off[] = {DI_DMG, DI_FL, DI_FL + 4, DI_FL + 8, DI_POS, DI_POS + 4, DI_POS + 8, DI_DIR, DI_DIR + 4, DI_DIR + 8, DI_110, DI_114, DI_E0, DI_ATT,
                                         DI_264, DI_A0, DI_80, DI_AC, DI_AC + 4, DI_AC + 8, DI_AC + 12, DI_AC + 16, DI_AC + 20, DI_A8, DI_98};
    uint32_t h = 2166136261u;
    for (unsigned k = 0; k < sizeof off / sizeof off[0]; k++)
        for (unsigned i = 0; i < 4; i++) h = (h ^ di[off[k] + i]) * 16777619u;
    h = (h ^ di[DI_E4]) * 16777619u;
    h = (h ^ di[DI_E5]) * 16777619u;
    return h ? h : 1;
}

typedef struct { uint32_t enemy; uint8_t part; uint32_t sig; uint32_t t; int seen_clear; } mh_seen_t;
typedef struct { mh_seen_t e[MH_DEDUPE]; uint32_t n_dup; } mh_dedupe_t;

static mh_seen_t *mh_seen_slot(mh_dedupe_t *d, uint32_t enemy, uint8_t part, uint32_t now, int create)
{
    mh_seen_t *old = &d->e[0];
    for (int i = 0; i < MH_DEDUPE; i++) {
        if (d->e[i].enemy == enemy && d->e[i].part == part) return &d->e[i];
        if ((uint32_t)(now - d->e[i].t) > (uint32_t)(now - old->t)) old = &d->e[i];
    }
    if (!create) return 0;
    memset(old, 0, sizeof *old);
    old->enemy = enemy, old->part = part, old->t = now, old->seen_clear = 1;
    return old;
}

/* the mailbox of (enemy, part) was seen without a valid DI: the next valid one is new whatever its values */
static void mh_dedupe_clear(mh_dedupe_t *d, uint32_t enemy, uint8_t part, uint32_t now)
{
    mh_seen_t *s = mh_seen_slot(d, enemy, part, now, 0);
    if (s) s->seen_clear = 1, s->t = now;
}

/* bomb: would mh_dedupe_new call this DI new? (no change: the client first looks at every DI of one replica update, then marks them) */
static int mh_dedupe_peek(mh_dedupe_t *d, uint32_t enemy, uint8_t part, uint32_t sig, uint32_t now)
{
    mh_seen_t *s = mh_seen_slot(d, enemy, part, now, 0);
    return !s || s->seen_clear || s->sig != sig;
}

/* 1 = send it (a new blow), 0 = the same DI as the last one sent */
static int mh_dedupe_new(mh_dedupe_t *d, uint32_t enemy, uint8_t part, uint32_t sig, uint32_t now)
{
    mh_seen_t *s = mh_seen_slot(d, enemy, part, now, 1);
    int fresh = s->seen_clear || s->sig != sig;
    s->t = now;
    if (!fresh) { d->n_dup++; return 0; }
    s->sig = sig, s->seen_clear = 0;
    return 1;
}

/* ---- host: the queue between the event (game pass) and the enemy's Intel update (its own job) ---- */
typedef struct { mh_rec_t r; uint32_t t; uint16_t tries; uint8_t used; } mh_qent_t;
typedef struct { mh_qent_t q[MH_QMAX]; uint32_t n_in, n_over, n_posted, n_deferred, n_direct, n_dropped; } mh_queue_t;

/* host: may a queued blow still be applied to the host's enemy? Pair run m1 (2026-11-08 04:29:54 / 04:39:16): after the DEATH event a late record set the client's
 * replica hp back for a moment, the client sent one more blow and the host ran the hit handler on its dead enemy ("hp 0 -> 0"; no second DEATH / kill report that
 * time). A dead enemy takes no blow: the entry is dropped (counted as dead). */
static int mh_apply_ok(int enemy_hp)
{
#ifdef MH_MODEL_BEFORE_DEADGUARD
    (void)enemy_hp;
    return 1;   /* a6a3108 .. e7e1983: applied whatever the enemy's hp */
#else
    return enemy_hp > 0;
#endif
}

/* combat2 (pair run combat2-a, 2026-11-08 06:52-06:58, static): the PART damage. A blow lowers the enemy's per-part threshold (enemy+0x71B0 + part * 8: body 400, head 200,
 * arms 200, legs 400 on uEm1120; FUN_00604170 adds -damage, at < 1 the part is "ready") through vtable +0x3B0 (uEm1100_vf236 0x5D5B00: part = vf 0x3C0(region), region 0 = head,
 * 9 = body ...). The stock collision path calls it at collision time (vf 0x3AC 0x605350 -> +0x3B0) on the PC where the blow lands = the client's replica, never on the host.
 * The Raid receiver (0x9D9E22) calls it itself after FUN_0061B050: vf3B0(DI+4 damage, DI+0x98, DI+0x110, DI+0x114 region). MHIT copied the receiver without that call, so the
 * host's thresholds never moved for a client's blow: a client's head shot (400 on a 2000-hp enemy, live 06:52:56) gave no head stagger (action 0x83, the only kind-0 window of
 * uEm1120, uEm1120Intel_vf62 picks it when the head threshold <= 0), and no arm / leg / body flinch either. Fix: the host calls +0x3B0 with these four values (mh_part_args). */
static int mh_part_args(const unsigned char *di, uint32_t a[4])
{
#ifdef MH_MODEL_BEFORE_PARTDMG
    (void)di, (void)a;
    return 0;   /* 5a0af8d: FUN_0061B050 + FUN_00ADD880 only */
#else
    a[0] = mh_rd32(di, DI_DMG), a[1] = mh_rd32(di, DI_98), a[2] = mh_rd32(di, DI_110), a[3] = mh_rd32(di, DI_114);
    return 1;
#endif
}

static void mh_q_push(mh_queue_t *q, const mh_rec_t *r, uint32_t now)
{
    mh_qent_t *old = &q->q[0];
    for (int i = 0; i < MH_QMAX; i++) {
        if (!q->q[i].used) { old = &q->q[i]; goto put; }
        if ((uint32_t)(now - q->q[i].t) > (uint32_t)(now - old->t)) old = &q->q[i];
    }
    q->n_over++;   /* full: the oldest goes */
put:
    old->r = *r, old->t = now, old->tries = 0, old->used = 1;
    q->n_in++;
}

/* the Intel update of the enemy with this layout runs: returns the entry to post (oldest first) or 0; mailbox_busy = its DI is valid already (a hit of this frame) */
static mh_qent_t *mh_q_take(mh_queue_t *q, int32_t la, uint16_t lb, uint16_t lc, int mailbox_busy, uint32_t now)
{
    mh_qent_t *best = 0;
    for (int i = 0; i < MH_QMAX; i++) {
        mh_qent_t *e = &q->q[i];
        if (!e->used || e->r.la != la || e->r.lb != lb || e->r.lc != lc) continue;
        if (!best || (uint32_t)(now - e->t) > (uint32_t)(now - best->t)) best = e;
    }
    if (!best) return 0;
    if (mailbox_busy) { best->tries++; q->n_deferred++; return 0; }
    best->used = 0;
    q->n_posted++;
    return best;
}

/* the game pass: an entry no update took for MH_DIRECT_MS: apply it directly (returned, oldest first); older than MH_DROP_MS: dropped */
static mh_qent_t *mh_q_stale(mh_queue_t *q, uint32_t now, int *dropped)
{
    *dropped = 0;
    for (int i = 0; i < MH_QMAX; i++) {
        mh_qent_t *e = &q->q[i];
        if (!e->used) continue;
        uint32_t age = now - e->t;
        if (age >= MH_DROP_MS) { e->used = 0; q->n_dropped++; (*dropped)++; continue; }
        if (age >= MH_DIRECT_MS) { e->used = 0; q->n_direct++; return e; }
    }
    return 0;
}

/* bomb (2026-11-09, notes\bomb.md, static bm_1 / bm_15): ONE blow per replica update. The stock reader FUN_006001D0 looks at the enemy's own DI and the DIs of its
 * part objects (+0x7328[0..9]) and runs the hit handler with ONE of them: the main DI, replaced by a part's DI only when that one has more damage (or as much when
 * the Intel's vtable +0xE8 says so for that part; left out here: ties keep the earlier one); the others of that frame are never applied. An explosion's sphere can
 * put a DI into the main body and into part objects in the same frame; the client sent every one of them as MHIT and the host applied each: more damage than the
 * stock game. Now the client sends, of the fresh DIs (not sent before) of this replica in this update, only the one the stock would pick; the others are marked as
 * seen. Returns the index (0 = main, 1..10 = part + 1) or -1. */
static inline int mh_pick_blow(const int32_t dmg[MH_NPARTS + 1], const uint8_t take[MH_NPARTS + 1])
{
    int best = -1;
    for (int i = 0; i <= MH_NPARTS; i++) {
        if (!take[i]) continue;
        if (best < 0 || dmg[i] > dmg[best]) best = i;
    }
    return best;
}
static inline int mh_send_this(int idx, int picked)
{
#ifdef MH_MODEL_BEFORE_BOMB
    (void)picked;
    return idx >= 0;   /* b94ff2e: every fresh DI of the replica went out */
#else
    return idx >= 0 && idx == picked;
#endif
}

/* bomb (2026-11-09, notes\bomb.md): the log budget of an MHIT line. One shared budget (g_mh_log, 60 lines per PC) was spent on the light's damage-0 hits within a
 * minute (flashlive run c: 2689 MHIT for the light), so the bottle's MHIT 11 minutes later left no line on either PC and the run was read as "no MHIT". Each kind
 * has its own budget now: plain damage-0 hits, hits with damage, blows of an object of the human (a thrown bomb, a held weapon). */
enum { MH_LOG_PLAIN = 0, MH_LOG_DAMAGE = 1, MH_LOG_OBJECT = 2, MH_LOG_KINDS = 3 };
static inline int mh_log_kind(int32_t dmg, int by_object)
{
    if (by_object) return MH_LOG_OBJECT;
    return dmg != 0 ? MH_LOG_DAMAGE : MH_LOG_PLAIN;
}
static inline int mh_log_take(uint32_t used[MH_LOG_KINDS], int kind, uint32_t cap)
{
#ifdef MH_MODEL_BEFORE_BOMB
    (void)kind;
    return ++used[0] <= cap;   /* b94ff2e: one budget for every MHIT line */
#else
    if (kind < 0 || kind >= MH_LOG_KINDS) kind = MH_LOG_PLAIN;
    return ++used[kind] <= cap;
#endif
}

#endif
