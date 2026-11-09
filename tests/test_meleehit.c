/* test_meleehit.c: offline checks of native/meleehit_logic.h (0.2.0 MHIT: the client's real blows on enemy replicas reach the host's enemy).
 * Built a second time with -DMH_MODEL_BEFORE_FIX (rev2-int a8a067e: the skipped Intel update of a replica looked at nothing): that model must fail (tests/build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../native/coop_proto.h"
#include "../native/meleehit_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned g_rng = 12345;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFF) / 65535.0f; }

/* a DamageInfo as the collision code leaves it: every byte random, the fields of the compact pair set to plausible values */
static void make_di(unsigned char *di, const float epos[3], unsigned attacker)
{
    for (unsigned i = 0; i < MH_DI_BUF; i++) di[i] = (unsigned char)rnd();
    di[DI_VALID] = 1;
    mh_wr32(di, DI_DMG, 45 + rnd() % 200);
    for (int i = 0; i < 3; i++) mh_wr32(di, DI_FL + 4 * i, (rnd() << 8 | rnd()) & ~(i == 0 ? (1u << MH_FL_NODMG_BIT) : 0u));
    for (int i = 0; i < 3; i++) mh_wrf(di, DI_POS + 4 * i, epos[i] + rndf(-150, 150));
    float d[3] = {rndf(-1, 1), rndf(-1, 1), rndf(-1, 1)};
    float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) mh_wrf(di, DI_DIR + 4 * i, d[i] / (n > 0 ? n : 1));
    mh_wr32(di, DI_110, rnd());
    mh_wr32(di, DI_114, rnd() % 0x300);
    mh_wr32(di, DI_E0, rnd());
    mh_wr32(di, DI_ATT, attacker);
    mh_wr32(di, DI_264, rnd());
    mh_wr32(di, DI_A0, rnd() & 0xFFFF);
    mh_wr32(di, DI_80, rnd() & 0xFFFF);
    for (int i = 0; i < 6; i++) mh_wrf(di, DI_AC + 4 * i, rndf(0, 500));
    mh_wrf(di, DI_A8, rndf(0, 40));
    mh_wr32(di, DI_98, rnd());
}

/* the stock compact pair FUN_00ADDCD0 / FUN_00ADC650: the offsets they carry (read from the decompiled code, 2026-11-07) */
static const unsigned stock_fields[] = {DI_DMG, 0x28, 0x2C, 0x30, 0xF0, 0xF4, 0xF8, 0x100, 0x104, 0x108, 0x110, 0x114, 0xE0, 0xDC, 0x264, 0xA0, 0x80, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC, 0xC0, 0xA8, 0x98};

static void test_mapping(void)
{
    float ec[3] = {5990.0f, 1708.0f, -8356.0f};   /* the replica on the client */
    float eh[3] = {5992.5f, 1708.0f, -8353.0f};   /* the same enemy on the host, a few units away (records lag) */
    for (int round = 0; round < 500; round++) {
        unsigned char di[MH_DI_BUF], out[MH_DI_BUF];
        make_di(di, ec, 0x2ABB8060u);
        int part = round % 11;
        mh_rec_t r, u;
        mh_from_di(di, ec, part, &r);
        r.la = 151, r.lb = 0, r.lc = 2, r.slot = 0;
        unsigned char wire[MH_REC_LEN];
        mh_pack(&r, wire);
        CHECK(mh_unpack(wire, MH_REC_LEN, &u));
        CHECK(!mh_unpack(wire, MH_REC_LEN - 1, &u) || 0);
        mh_unpack(wire, MH_REC_LEN, &u);
        CHECK(u.la == 151 && u.lb == 0 && u.lc == 2 && u.slot == 0 && u.part == part);
        memset(out, 0, sizeof out);
        mh_to_di(&u, eh, 0x2AB00060u, out);
        CHECK(out[DI_VALID] == 1);
        CHECK(mh_rd32(out, DI_DMG) == mh_rd32(di, DI_DMG));
        for (int i = 0; i < 3; i++) CHECK(mh_rd32(out, DI_FL + 4 * i) == mh_rd32(di, DI_FL + 4 * i));
        for (int i = 0; i < 3; i++) {   /* the hit point keeps its place on the body: relative to the host's enemy */
            float want = eh[i] + (mh_rdf(di, DI_POS + 4 * i) - ec[i]);
            CHECK(fabsf(mh_rdf(out, DI_POS + 4 * i) - want) < 0.06f);
        }
        for (int i = 0; i < 3; i++) CHECK(mh_rdf(out, DI_DIR + 4 * i) == mh_rdf(di, DI_DIR + 4 * i));
        CHECK(mh_rd32(out, DI_110) == mh_rd32(di, DI_110));
        CHECK(mh_rd32(out, DI_114) == ((mh_rd32(di, DI_114) + (unsigned)(part * MH_PART_STEP)) & 0xFFFF));
        CHECK(mh_rd32(out, DI_E0) == mh_rd32(di, DI_E0));
        CHECK(mh_rd32(out, DI_ATT) == 0x2AB00060u);   /* the host's own object of that slot (the puppet), never the client's pointer */
        CHECK(out[DI_E4] == di[DI_E4] && out[DI_E5] == di[DI_E5]);
        CHECK(mh_rd32(out, DI_264) == mh_rd32(di, DI_264));
        CHECK(mh_rd32(out, DI_A0) == mh_rd32(di, DI_A0) && mh_rd32(out, DI_80) == mh_rd32(di, DI_80));
        for (int i = 0; i < 6; i++) CHECK(mh_rdf(out, DI_AC + 4 * i) == mh_rdf(di, DI_AC + 4 * i));
        CHECK(mh_rdf(out, DI_A8) == (float)lrintf(mh_rdf(di, DI_A8)));   /* FUN_00ADDCD0 rounds it the same way */
        CHECK(mh_rd32(out, DI_98) == mh_rd32(di, DI_98));
        CHECK(!r.clamped);
    }
    /* mh_to_di touches exactly the fields of the stock compact pair (+ the valid byte and the two w lanes), nothing else */
    unsigned char di[MH_DI_BUF], out[MH_DI_BUF];
    make_di(di, ec, 1);
    mh_rec_t r;
    mh_from_di(di, ec, 0, &r);
    memset(out, 0xA5, sizeof out);
    for (int i = 0; i < 3; i++) mh_wr32(out, DI_FL + 4 * i, 0);
    unsigned char before[MH_DI_BUF];
    memcpy(before, out, sizeof out);
    mh_to_di(&r, eh, 7, out);
    int stray = 0;
    for (unsigned o = 0; o < MH_DI_BUF; o++) {
        int listed = o == DI_VALID || o == DI_E4 || o == DI_E5 || (o >= 0xFC && o < 0x100) || (o >= 0x10C && o < 0x110);
        for (unsigned k = 0; k < sizeof stock_fields / sizeof stock_fields[0]; k++)
            if (o >= stock_fields[k] && o < stock_fields[k] + 4) listed = 1;
        if (!listed && out[o] != before[o]) stray++;
    }
    CHECK(stray == 0);
    /* a hit point outside +-3276 units of the enemy is clamped and marked */
    mh_wrf(di, DI_POS, ec[0] + 5000.0f);
    mh_from_di(di, ec, 0, &r);
    CHECK(r.clamped && r.rel[0] == 32767);
    CHECK(MH_REC_LEN <= CP_EVENT_MAX);   /* one event */
    /* a slot or part outside the range is refused */
    unsigned char wire[MH_REC_LEN];
    r.slot = 2;
    mh_pack(&r, wire);
    mh_rec_t u;
    CHECK(!mh_unpack(wire, MH_REC_LEN, &u));
    r.slot = 1, r.part = 11;
    mh_pack(&r, wire);
    CHECK(!mh_unpack(wire, MH_REC_LEN, &u));
}

static void test_take(void)
{
    mh_take_in_t in;
    memset(&in, 0, sizeof in);
    in.valid = 1, in.attacker = 0x100, in.mine = 0x100, in.replica_hp = 220;
    CHECK(mh_take(&in) == MH_TAKE_MINE);
    in.attacker = 0x200, in.attacker_owner = 0x100;   /* a thrown object of my human (a brick, a bottle) */
    CHECK(mh_take(&in) == MH_TAKE_MINE);
    in.attacker_owner = 0x300;                         /* the partner's puppet / anything else: the host has the real blow */
    CHECK(mh_take(&in) == MH_TAKE_OTHER);
    in.attacker = 0x100, in.fl0 = 1u << MH_FL_NODMG_BIT;
    CHECK(mh_take(&in) == MH_TAKE_INVALID);
    in.fl0 = 0, in.replica_hp = 0;
    CHECK(mh_take(&in) == MH_TAKE_INVALID);
    in.replica_hp = 1;                                 /* the stock hp floor: hp 1 still takes the finishing blow */
    CHECK(mh_take(&in) == MH_TAKE_MINE);
    in.valid = 0;
    CHECK(mh_take(&in) == MH_TAKE_NO);
    in.valid = 1, in.attacker = 0;
    CHECK(mh_take(&in) == MH_TAKE_INVALID);
}

/* the replica's mailbox over game frames; mode 0: the collision code clears it every frame, mode 1: it stays valid until a new blow writes it */
static int run_client(int mode, int slashes, int frames_between)
{
    mh_dedupe_t d;
    memset(&d, 0, sizeof d);
    unsigned char di[MH_DI_BUF];
    float ep[3] = {0, 0, 0};
    int valid = 0, sent = 0;
    unsigned now = 1000;
    for (int s = 0; s < slashes; s++) {
        make_di(di, ep, 0x100);
        mh_wr32(di, DI_DMG, 45);   /* every slash of the same knife has the same damage and flags: the hit point differs */
        valid = 1;
        for (int f = 0; f < frames_between; f++, now += 33) {
            if (mode == 0 && f > 0) valid = 0;
            ep[0] += 3.0f, ep[2] -= 2.0f;   /* the replica walks on (records) while the DI stays */
            mh_take_in_t in = {valid, 0x100, 0, 0x100, mh_rd32(di, DI_FL), 220};
            if (!valid) { mh_dedupe_clear(&d, 0xE0, 0, now); continue; }
            if (mh_take(&in) != MH_TAKE_MINE) continue;
            if (mh_dedupe_new(&d, 0xE0, 0, mh_sig_di(di), now)) sent++;
        }
    }
    return sent;
}

static void test_client(void)
{
    CHECK(run_client(0, 34, 20) == 34);   /* G1: 34 slashes, each reaches the host once */
    CHECK(run_client(1, 34, 20) == 34);   /* the same when the mailbox is never cleared on the replica */
    CHECK(run_client(1, 5, 300) == 5);    /* a DI that stays valid for 10 s is still sent once */
    /* the same values written again after the mailbox was seen clear: a new blow */
    mh_dedupe_t d;
    memset(&d, 0, sizeof d);
    CHECK(mh_dedupe_new(&d, 1, 0, 77, 10));
    CHECK(!mh_dedupe_new(&d, 1, 0, 77, 43));
    mh_dedupe_clear(&d, 1, 0, 76);
    CHECK(mh_dedupe_new(&d, 1, 0, 77, 109));
    CHECK(mh_dedupe_new(&d, 1, 3, 77, 110));   /* another part of the same enemy is its own mailbox */
    CHECK(mh_dedupe_new(&d, 2, 0, 77, 111));   /* another enemy */
    for (unsigned e = 10; e < 10 + 2 * MH_DEDUPE; e++) mh_dedupe_new(&d, e, 0, 5, 200 + e);   /* the table recycles the oldest entries */
    CHECK(mh_dedupe_new(&d, 1, 0, 77, 1000));
}

/* the host: a toy enemy whose stock update applies the mailbox; the host's own human hits sometimes in the same frame */
static void test_host(void)
{
    mh_queue_t q;
    memset(&q, 0, sizeof q);
    mh_rec_t r;
    memset(&r, 0, sizeof r);
    r.la = 151, r.lb = 0, r.lc = 2, r.slot = 0, r.dmg = 45;
    unsigned now = 5000;
    mh_q_push(&q, &r, now);
    CHECK(q.n_in == 1);
    CHECK(mh_q_take(&q, 151, 0, 3, 0, now) == 0);       /* another enemy's update does not take it */
    CHECK(mh_q_take(&q, 151, 0, 2, 1, now + 33) == 0);  /* the mailbox holds a hit of this frame: deferred */
    CHECK(q.n_deferred == 1);
    mh_qent_t *e = mh_q_take(&q, 151, 0, 2, 0, now + 66);
    CHECK(e && e->r.dmg == 45 && !e->used);
    CHECK(mh_q_take(&q, 151, 0, 2, 0, now + 99) == 0);  /* taken once */
    /* nothing lost when both humans hit in the same frames: 20 frames, the host hits on even frames, the client's blows arrive every frame */
    int hp = 2000, host_hits = 0, client_hits = 0;
    memset(&q, 0, sizeof q);
    for (int f = 0; f < 20; f++, now += 33) {
        mh_q_push(&q, &r, now);
        int mailbox = 0, mdmg = 0;
        if (f % 2 == 0) mailbox = 1, mdmg = 10, host_hits++;   /* the host's own blow landed this frame */
        mh_qent_t *t = mh_q_take(&q, 151, 0, 2, mailbox, now);
        if (t) mailbox = 1, mdmg = t->r.dmg, client_hits++;
        if (mailbox) hp -= mdmg;   /* the stock update applies the mailbox */
    }
    for (int f = 0; f < 20; f++, now += 33) {   /* the queue drains in the following frames */
        mh_qent_t *t = mh_q_take(&q, 151, 0, 2, 0, now);
        if (t) hp -= t->r.dmg, client_hits++;
    }
    CHECK(client_hits == 20 && host_hits == 10);
    CHECK(hp == 2000 - 20 * 45 - 10 * 10);
    /* no Intel update for the enemy: applied directly after MH_DIRECT_MS, the rest dropped after MH_DROP_MS */
    memset(&q, 0, sizeof q);
    mh_q_push(&q, &r, 10000);
    int dropped = 0;
    CHECK(mh_q_stale(&q, 10000 + MH_DIRECT_MS - 1, &dropped) == 0 && !dropped);
    e = mh_q_stale(&q, 10000 + MH_DIRECT_MS, &dropped);
    CHECK(e && q.n_direct == 1);
    mh_q_push(&q, &r, 20000);
    q.q[0].t = 20000, q.q[0].used = 1;
    CHECK(mh_q_stale(&q, 20000 + MH_DROP_MS, &dropped) == 0 && dropped >= 1);
    /* overflow: the oldest is replaced */
    memset(&q, 0, sizeof q);
    for (int i = 0; i < MH_QMAX + 3; i++) r.dmg = i, mh_q_push(&q, &r, 30000 + i);
    CHECK(q.n_over == 3);
    int minv = 999;
    for (int i = 0; i < MH_QMAX; i++) if (q.q[i].used && q.q[i].r.dmg < minv) minv = q.q[i].r.dmg;
    CHECK(minv == 3);
}

/* the host's dead-enemy guard (pair run m1): the replayed blows of the run, the enemy's hp at the moment the host takes each */
static void test_dead_guard(void)
{
    static const int hp_at_take[] = {120, 20, 1, 0, 320, 0, 220, 20, 1, 0};   /* m1: (151,0,3) 120 -> 20 -> 1 -> 0, (151,0,0) 320 -> 0 then a late blow, (151,0,2) ... */
    int applied_dead = 0, applied_live = 0;
    for (unsigned i = 0; i < sizeof hp_at_take / sizeof hp_at_take[0]; i++) {
        if (mh_apply_ok(hp_at_take[i])) {
            if (hp_at_take[i] <= 0) applied_dead++;
            else applied_live++;
        }
    }
    CHECK(applied_live == 7);
    CHECK(applied_dead == 0);
    for (int hp = -5; hp <= 5; hp++) CHECK(mh_apply_ok(hp) == (hp > 0));
}

/* combat2 (pair run combat2-a 06:52:56): the part damage of a client's blow on the host. Model of the stock pieces: the per-part thresholds of uEm1120 (enemy+0x71B0 + part * 8:
 * body 400, head 200, arms 200, legs 400, live), FUN_00604170 (threshold += -damage, at < 1 it is 0 and "ready"), the region -> part map uEm1100_vf240 (0x5DDB90), and the head rule of
 * uEm1120Intel_vf62 (a hit on part 1 while the head threshold is <= 0 = action 0x83, the head stagger, then the threshold is reset). The host applies a client's MHIT record; the
 * stock collision path lowered the threshold on the CLIENT (the replica) only, so the host's reaction never saw it before the fix. */
typedef struct { int thr[8], reset[8]; } em_parts_t;
static int em_part_of_region(unsigned r)
{
    switch (r) {
    case 0: case 0xb: case 0xc: return 1;
    case 1: case 2: case 0xd: case 0xe: return 5;
    case 3: case 4: case 0xf: case 0x10: return 4;
    case 5: case 7: case 0x11: case 0x13: return 3;
    case 6: case 8: case 0x12: case 0x14: return 2;
    case 9: case 0x15: return 0;
    case 10: return 6;
    default: return -1;
    }
}
static void em_part_damage(em_parts_t *e, int dmg, unsigned region)   /* vf 0x3B0 -> FUN_00604170(part, -dmg) */
{
    int p = em_part_of_region(region);
    if (p < 0) return;
    int t = e->thr[p] - dmg;
    e->thr[p] = t < 1 ? 0 : (t > e->reset[p] ? e->reset[p] : t);
}
static unsigned em_reaction(em_parts_t *e, unsigned region)   /* the part rule of uEm1120Intel_vf62 for an alerted enemy; 0 = another reaction */
{
    int p = em_part_of_region(region);
    if (p == 1 && e->thr[1] <= 0) { e->thr[1] = e->reset[1]; return 0x83; }
    if (p == 0 && e->thr[0] <= 0) { e->thr[0] = e->reset[0]; return 0x88; }
    return 0;
}
/* the host's direct apply of one MHIT record: FUN_0061B050, then (the fix) vf 0x3B0 with the DI's values, then FUN_00ADD880; the stock update picks the reaction */
static unsigned host_apply(em_parts_t *e, int dmg, unsigned region)
{
    unsigned char di[MH_DI_BUF];
    memset(di, 0, sizeof di);
    mh_wr32(di, DI_DMG, (uint32_t)dmg), mh_wr32(di, DI_114, region), mh_wr32(di, DI_110, 7), mh_wr32(di, DI_98, 1);
    uint32_t a[4];
    if (mh_part_args(di, a)) {
        CHECK(a[0] == (uint32_t)dmg && a[3] == region && a[2] == 7 && a[1] == 1);
        em_part_damage(e, (int)a[0], a[3]);
    }
    return em_reaction(e, region);
}
static void test_part_damage(void)
{
    static const int thr[8] = {400, 200, 200, 200, 400, 400, 400, 400};
    em_parts_t e;
    memcpy(e.thr, thr, sizeof thr), memcpy(e.reset, thr, sizeof thr);
    /* live combat2-a: the client's pistol head shot, DI damage 400, region (+114) 0, on a 2000-hp enemy: the head stagger */
    CHECK(host_apply(&e, 400, 0) == 0x83);
    CHECK(e.thr[1] == 200);   /* reset after the stagger */
    /* two body shots of 100 (+114 9, m1 / m2): no stagger yet; the fourth reaches 400 */
    memcpy(e.thr, thr, sizeof thr);
    CHECK(host_apply(&e, 100, 9) == 0 && host_apply(&e, 100, 9) == 0 && host_apply(&e, 100, 9) == 0);
    CHECK(host_apply(&e, 100, 9) == 0x88);
    /* knife slashes on the head (41 each): the fifth lowers the head threshold below 1 */
    memcpy(e.thr, thr, sizeof thr);
    int n = 0;
    while (n < 20 && host_apply(&e, 41, 0xb) != 0x83) n++;
    CHECK(n == 4);
    /* a stepped part region (a part object's DI: +0x46 per part, FUN_006001D0) maps to no part, as in the stock path */
    memcpy(e.thr, thr, sizeof thr);
    CHECK(host_apply(&e, 400, 0x46) == 0);
    CHECK(memcmp(e.thr, thr, sizeof thr) == 0);
}

int main(void)
{
    test_mapping();
    test_take();
    test_client();
    test_host();
    test_dead_guard();
    test_part_damage();
#ifdef MH_MODEL_BEFORE_FIX
    printf("test_meleehit (model before the fix): %d checks, %d failed\n", g_checks, g_fail);
#else
    printf("test_meleehit: %d checks, %d failed\n", g_checks, g_fail);
#endif
    return g_fail ? 1 : 0;
}
