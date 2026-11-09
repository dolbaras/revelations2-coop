/* test_objsync.c: offline model of the object sync (native/objsync_logic.h, stage 10: gaps G1-G4 of the Episode 1 maps).
 * Every check describes a case of the two Episode 1 halves (notes\objsync.md 5); built with -DOS_MODEL_RELEASE020 (the behaviour of release-020) the suite must fail: tests/build.sh checks both. */
#include <stdio.h>
#include <string.h>
#include "../native/objsync_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* class chains as classes_name prints them (live format: "vtable exe+.., DTI ..: name(size) : parent(size) : ...") */
static const char *CH_SHUTTER = "vtable exe+eef5a0, DTI 0156b5b8: uSm0418(43d0) : uSmShutterCoop(43d0) : uSmCoopBase(43c0) : uSetModel(4350) : uBioModel(19f0) : uModel(1960)";
static const char *CH_PAIR = "vtable exe+eef2e0, DTI 0156b3fc: uSmPairCoop(43e0) : uSmCoopBase(43c0) : uSetModel(4350) : uBioModel(19f0)";
static const char *CH_DOOR = "vtable exe+ee97c8, DTI 0156ae80: uSm0402(43d0) : uSmDoorCoop(43d0) : uSmCoopBase(43c0) : uSetModel(4350)";
static const char *CH_DEMO = "vtable exe+edf428, DTI 0156a000: uSmCoopDemo(4400) : uSetModel(4350)";
static const char *CH_PRESS = "vtable exe+f62458, DTI 01574c2c: uSm1805(4450) : uSmPress(4450) : uSmMoveBox(43d0) : uSetModel(4350)";
static const char *CH_HOLD = "vtable exe+f60338, DTI 01574770: uSm1160(46d0) : uSmInteractive(46d0) : uSmMoveBox(43d0) : uSetModel(4350)";
static const char *CH_BRICK = "vtable exe+f60bf8, DTI 01574790: uSm1814(46d0) : uSmInteractive(46d0) : uSmMoveBox(43d0) : uSetModel(4350)";
static const char *CH_BRICK_INF = "vtable exe+f5fac0, DTI 0157472c: uSm1928(46f0) : uSmInteractiveResetPos(46f0) : uSmInteractive(46d0) : uSmMoveBox(43d0) : uSetModel(4350)";
static const char *CH_CHECKER = "vtable exe+f61388, DTI 01574884: uSm1769(4480) : uSmMoveBoxPosChecker(4480) : uSetModel(4350)";
static const char *CH_CELL = "vtable exe+f81230, DTI 0157c000: uSm0453(4380) : uSmSlideDoor(4380) : uSmDoor(4370) : uSetModel(4350)";

static void t_kinds(void)
{
    CHECK(os_kind_of(CH_SHUTTER) == OS_COOP_SHUTTER);
    CHECK(os_kind_of(CH_PAIR) == OS_COOP_PAIR);
    CHECK(os_kind_of(CH_DOOR) == OS_COOP_DOOR);
    CHECK(os_kind_of(CH_DEMO) == OS_COOP_OTHER);
    CHECK(os_kind_of(CH_PRESS) == OS_MOVE_PRESS);          /* 101 (101,3,3/4), 151 (151,3,3/4) push boxes */
    CHECK(os_kind_of(CH_HOLD) == OS_MOVE_CARRY);           /* (151,1,0) room 4, (154,1,4) the box that frees Natalia's hole */
    CHECK(os_kind_of(CH_BRICK) == OS_MOVE_CARRY);
    CHECK(os_kind_of(CH_BRICK_INF) == OS_MOVE_CARRY);
    CHECK(os_kind_of(CH_CHECKER) == OS_PLAIN);             /* uSmMoveBoxPosChecker is no box: the name starts like uSmMoveBox but is another class */
    CHECK(os_kind_of(CH_CELL) == OS_PLAIN);
    CHECK(os_kind_of(NULL) == OS_PLAIN);
    CHECK(!os_has_class("x: uSmMoveBoxPosChecker(1)", "uSmMoveBox"));
    CHECK(os_has_class("x: a(1) : uSmMoveBox(2)", "uSmMoveBox"));
}

/* DOOR action mirror: what the scan sends, what the receiver replays with ChangeAction */
static void t_door_policy(void)
{
    /* plain set models: unchanged */
    CHECK(os_door_send(OS_PLAIN, 3, OS_ACT_NONE, 2));
    CHECK(os_door_apply(OS_PLAIN, 3, OS_ACT_NONE));
    /* a push: uSm1805 0 -> 1..4 (Press, direction). The receiver's ChangeAction has no actor and the Press start calls the actor's +0x36C: never replayed, never sent */
    for (int a = 1; a <= 4; a++) {
        CHECK(!os_door_send(OS_MOVE_PRESS, a, OS_ACT_LOCAL, 2));
        CHECK(!os_door_apply(OS_MOVE_PRESS, a, OS_ACT_NONE));
    }
    CHECK(!os_door_apply(OS_MOVE_PRESS, 0, OS_ACT_NONE));
    /* carried objects: Pickup (1) has a carrier on the sender only: not mirrored; Break / Broken (2..5) are: the brick is gone on both PCs */
    CHECK(!os_door_send(OS_MOVE_CARRY, 1, OS_ACT_LOCAL, 2));
    CHECK(!os_door_apply(OS_MOVE_CARRY, 1, OS_ACT_NONE));
    CHECK(!os_door_apply(OS_MOVE_CARRY, 0, OS_ACT_NONE));
    CHECK(os_door_send(OS_MOVE_CARRY, 2, OS_ACT_LOCAL, 2));
    CHECK(os_door_send(OS_MOVE_CARRY, 4, OS_ACT_NONE, 2));
    CHECK(os_door_apply(OS_MOVE_CARRY, 4, OS_ACT_NONE));
    CHECK(!os_door_apply(OS_MOVE_CARRY, 2, OS_ACT_LOCAL));   /* this PC's human carries its copy: not taken from his hands */
    CHECK(!os_door_send(OS_MOVE_CARRY, 2, OS_ACT_REMOTE, 2));
    CHECK(!os_door_send(OS_MOVE_OTHER, 1, OS_ACT_LOCAL, 2));
    /* co-op: the replay of the partner's press makes this PC's copy change too (actor = the partner's puppet): no echo */
    CHECK(os_door_send(OS_COOP_SHUTTER, 1, OS_ACT_LOCAL, 2));
    CHECK(os_door_send(OS_COOP_SHUTTER, 1, OS_ACT_NONE, 2));
    CHECK(!os_door_send(OS_COOP_SHUTTER, 1, OS_ACT_REMOTE, 2));
    CHECK(!os_door_send(OS_COOP_DOOR, 1, OS_ACT_REMOTE, 2));
    CHECK(!os_door_send(OS_COOP_DOOR, 1, OS_ACT_LOCAL, 0));
}

/* the echo of a replay whose action instance kept no actor: pair run osPB 2026-11-07 07:22:56, copy 1, (151,160,102): replay at 56.162, its scan saw 0 -> 1 at 56.234 with the
 * actor not the puppet (no new action instance) and sent it back; only the partner's busy guard stopped a phantom press of "slot 0" there */
static void t_replay_echo(void)
{
    unsigned rp = 0x00A1B2C3u;
    CHECK(os_replay_echo(OS_COOP_SHUTTER, 1, OS_ACT_NONE, rp, rp + 72));     /* the run */
    CHECK(os_replay_echo(OS_COOP_SHUTTER, 1, OS_ACT_REMOTE, rp, rp + 72));   /* the normal case is caught by os_door_send too */
    CHECK(os_replay_echo(OS_COOP_PAIR, 1, OS_ACT_NONE, rp, rp + 1999));
    CHECK(os_replay_echo(OS_COOP_DOOR, 1, OS_ACT_NONE, 0xFFFFFF00u, 0x00000100u));   /* across the 2^32 wrap of GetTickCount */
    CHECK(!os_replay_echo(OS_COOP_SHUTTER, 1, OS_ACT_LOCAL, rp, rp + 72));  /* this PC's human pressed: a real press, sent */
    CHECK(!os_replay_echo(OS_COOP_SHUTTER, 1, OS_ACT_NONE, rp, rp + 2000)); /* window over */
    CHECK(!os_replay_echo(OS_COOP_SHUTTER, 1, OS_ACT_NONE, 0, 72));         /* never replayed */
    CHECK(!os_replay_echo(OS_COOP_SHUTTER, 3, OS_ACT_NONE, rp, rp + 72));   /* Opening (2 -> 3, actor = the set model): not a press, the receiver skips it anyway */
    CHECK(!os_replay_echo(OS_PLAIN, 1, OS_ACT_NONE, rp, rp + 72));
    CHECK(!os_replay_echo(OS_MOVE_PRESS, 1, OS_ACT_NONE, rp, rp + 72));
    /* the whole decision of the scan for the run's transition: the old rule alone sent it */
    int send = !os_replay_echo(OS_COOP_SHUTTER, 1, OS_ACT_NONE, rp, rp + 72) && os_door_send(OS_COOP_SHUTTER, 1, OS_ACT_NONE, 2);
    CHECK(!send);
}

/* G1: press replay of the co-op classes */
static void t_coop_plan(void)
{
    CHECK(os_coop_plan(OS_COOP_DOOR, 1, 0, 1, 2) == OS_RP_ACT);         /* proven in 6f, unchanged */
    CHECK(os_coop_plan(OS_COOP_SHUTTER, 1, 0, 1, 2) == OS_RP_FULL);     /* the stock press: +0x4230[slot], vf +0x1F4 (the shutter's opening side), then Act */
    CHECK(os_coop_plan(OS_COOP_PAIR, 1, 0, 1, 2) == OS_RP_FULL);
    CHECK(os_coop_plan(OS_COOP_OTHER, 1, 0, 1, 2) == OS_RP_ACT);
    CHECK(os_coop_plan(OS_COOP_DOOR, 1, 0, 1, 3) == OS_RP_FULL);
    CHECK(os_coop_plan(OS_COOP_SHUTTER, 3, 0, 1, 2) == OS_RP_SKIP_NOTPRESS);   /* Opening is the chain's own business */
    CHECK(os_coop_plan(OS_COOP_SHUTTER, 0, 1, 1, 2) == OS_RP_SKIP_NOTPRESS);
    CHECK(os_coop_plan(OS_COOP_SHUTTER, 1, 1, 1, 2) == OS_RP_SKIP_BUSY);
    CHECK(os_coop_plan(OS_COOP_PAIR, 1, 2, 1, 2) == OS_RP_SKIP_BUSY);
    CHECK(os_coop_plan(OS_COOP_PAIR, 1, 0, 0, 2) == OS_RP_SKIP_NOPRESSER);
    /* SmCoopAccess(1) on this PC only: the presser's PC accepted, so the copy's mAccessType is held at 0 for the replay */
    CHECK(os_access_override(1, 1));
    CHECK(!os_access_override(0, 1));
    CHECK(!os_access_override(1, 0));
    CHECK(!os_access_restore_due(1000, 1500, 0));
    CHECK(os_access_restore_due(1000, 1500, 1));
    CHECK(os_access_restore_due(1000, 1000 + OS_ACCESS_HOLD_MS, 0));
    /* border shutter with two layout ids: alias by kind and distance */
    CHECK(os_alias_ok(OS_COOP_SHUTTER, OS_COOP_SHUTTER, 0, -23, 0));     /* (100,161,160) y 1600 = (101,160,105) y 1577 */
    CHECK(!os_alias_ok(OS_COOP_SHUTTER, OS_COOP_DOOR, 0, 0, 0));
    CHECK(!os_alias_ok(OS_COOP_SHUTTER, OS_COOP_SHUTTER, 200, 0, 0));
    CHECK(!os_alias_ok(OS_PLAIN, OS_PLAIN, 0, 0, 0));
}

/* ---- G2: two PCs with the same box; a frame loop at 10 Hz ---- */
typedef struct {
    os_prop_t e;
    os_xf_t box;          /* this PC's copy of the box */
    int actor;            /* who acts on this PC's copy */
    int sent, applied, sent_final;
} pc_t;

typedef struct { os_prop_msg_t m[64]; int n; } wire_t;

static void pc_init(pc_t *p, float x, float y, float z)
{
    memset(p, 0, sizeof *p);
    p->box.p[0] = x, p->box.p[1] = y, p->box.p[2] = z, p->box.q[3] = 1.0f;
    os_prop_learn(&p->e, &p->box);
}

static void pc_tick(pc_t *p, unsigned now, wire_t *out, int slot)
{
    int f = os_prop_tick(&p->e, &p->box, p->actor, now);
    if (!f) return;
    p->sent++;
    if (f & OS_PF_FINAL) p->sent_final++;
    if (out->n < 64) {
        os_prop_msg_t *m = &out->m[out->n++];
        memset(m, 0, sizeof *m);
        m->a = 101, m->b = 3, m->c = 3, m->flags = f, m->slot = slot, m->seq = p->e.seq_tx, m->xf = p->e.base, m->act = 0;
    }
}

static void pc_recv(pc_t *p, wire_t *in, unsigned now)
{
    for (int i = 0; i < in->n; i++) {
        unsigned char b[OS_PROP_LEN];
        os_prop_msg_t m;
        int len = os_prop_pack(&in->m[i], b);
        if (!os_prop_unpack(b, len, &m)) continue;
        int local = p->actor == OS_ACT_LOCAL ? OS_ACT_LOCAL : OS_ACT_NONE;
        if (os_prop_accept(&p->e, m.seq, local, now)) {
            p->box = m.xf;
            os_prop_applied(&p->e, &m.xf);
            p->applied++;
        }
    }
    in->n = 0;
}

static float dist3(const os_xf_t *a, const os_xf_t *b)
{
    float dx = a->p[0] - b->p[0], dy = a->p[1] - b->p[1], dz = a->p[2] - b->p[2];
    return dx * dx + dy * dy + dz * dz;
}

static void t_prop_push(void)
{
    pc_t a, b;
    wire_t ab = {0}, ba = {0};
    pc_init(&a, 5410, 2000, -18258), pc_init(&b, 5410, 2000, -18258);
    unsigned now = 10000;
    /* A's human pushes the box 155 units toward the checker (101,3,2) at (5255,2000,-18260) in 1.5 s */
    a.actor = OS_ACT_LOCAL;
    for (int k = 0; k < 15; k++, now += 100) {
        a.box.p[0] -= 10.3f;
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    CHECK(a.sent >= 12);
    CHECK(b.applied == a.sent);
    CHECK(b.sent == 0);                                   /* no echo from the receiver */
    /* the push ends; the end of the push snaps the box down 2 units (FUN_007DFD20 ground ray) */
    a.actor = OS_ACT_NONE;
    a.box.p[1] -= 2.0f;
    for (int k = 0; k < 30; k++, now += 100) {
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    CHECK(a.sent_final == 1);
    CHECK(dist3(&a.box, &b.box) < 0.01f);                  /* the partner's PC has the box at the checker too: its own checker sets EpisodeFlag[2] */
    CHECK(b.sent == 0);
    CHECK(b.e.owner == OS_OWN_REMOTE);                    /* the remote PC owns it for OS_REMOTE_MS after its last sample */
    for (int k = 0; k < 25; k++, now += 100) pc_tick(&b, now, &ba, 1);
    CHECK(b.e.owner == OS_OWN_NONE && b.sent == 0);
    /* now B's human carries it back a little: B owns it, A follows */
    b.actor = OS_ACT_LOCAL;
    for (int k = 0; k < 5; k++, now += 100) {
        b.box.p[2] += 20.0f;
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    b.actor = OS_ACT_NONE;
    for (int k = 0; k < 30; k++, now += 100) {
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    CHECK(b.sent >= 5);
    CHECK(dist3(&a.box, &b.box) < 0.01f);
}

/* pair run wpB3c 22:40:14 (151,1,0): while Barry holds the box its +0x40 reads (-81.2,-74.4,10.5) (relative to him); the partner's PC wrote that as a world position:
 * its box left the room for the whole carry. With os_carry_sample the partner's copy stays at the holder (or where it was) and ends on the checker. */
static int near_h(const os_xf_t *x, const float h[3]) { float dx = x->p[0] - h[0], dy = x->p[1] - h[1], dz = x->p[2] - h[2]; return dx * dx + dy * dy + dz * dz <= 400.0f * 400.0f; }
static void carry_scan(pc_t *a, int act, const os_xf_t *raw, const float *mt, const float *h, unsigned now, wire_t *ab)
{
    os_xf_t cur;
    if (!os_carry_sample(OS_MOVE_CARRY, act, raw, mt, h, &a->e.base, &cur)) cur = a->e.base;
    a->box = cur;
    pc_tick(a, now, ab, 0);
}
static void t_prop_carry(void)
{
    pc_t a, b;
    wire_t ab = {0}, ba = {0};
    pc_init(&a, 3854.4f, 2050.0f, -11798.4f), pc_init(&b, 3854.4f, 2050.0f, -11798.4f);
    a.box.q[1] = 0.110f, a.box.q[3] = 0.994f, os_prop_learn(&a.e, &a.box), b.box = a.box, os_prop_learn(&b.e, &b.box);
    unsigned now = 50000;
    float h[3] = {3874.1f, 2053.5f, -11742.9f}, mt[3];
    os_xf_t raw = a.box, rel = {{-81.2f, -74.4f, 10.5f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    a.actor = OS_ACT_LOCAL;
    carry_scan(&a, 1, &raw, raw.p, h, now, &ab), pc_recv(&b, &ab, now), now += 100;   /* the pick-up starts: still the world position */
    int far = 0, rot_ok = 1;
    for (int k = 1; k <= 12; k++, now += 100) {   /* attached: Barry walks to the checker (3990.3,2007.5,-12673.9) */
        h[0] = 3874.1f + (3980.2f - 3874.1f) * k / 12, h[1] = 2053.5f + (2009.5f - 2053.5f) * k / 12, h[2] = -11742.9f + (-12589.2f + 11742.9f) * k / 12;
        mt[0] = h[0] + 9.0f, mt[1] = h[1] + 40.0f, mt[2] = h[2] - 90.0f;
        carry_scan(&a, 1, &rel, mt, h, now, &ab);
        for (int i = 0; i < ab.n; i++) if (!near_h(&ab.m[i].xf, h)) far++;
        for (int i = 0; i < ab.n; i++) if (ab.m[i].xf.q[1] != 0.110f) rot_ok = 0;
        pc_recv(&b, &ab, now), pc_tick(&b, now, &ba, 1);
        if (!near_h(&b.box, h)) far++;
    }
    CHECK(far == 0);                     /* no sample and no state of the partner's copy away from the holder */
    CHECK(rot_ok);                       /* the world rotation of before the pick-up, not the relative one */
    CHECK(b.applied >= 10);
    os_xf_t put = {{3990.1f, 2009.4f, -12675.4f}, {0.0f, 0.110f, 0.0f, 0.994f}};
    carry_scan(&a, 1, &put, put.p, h, now, &ab), pc_recv(&b, &ab, now), now += 100;   /* put down: world again, action still 1 */
    a.actor = OS_ACT_NONE;
    for (int k = 0; k < 30; k++, now += 100) { carry_scan(&a, 0, &put, put.p, NULL, now, &ab); pc_recv(&b, &ab, now); pc_tick(&b, now, &ba, 1); }
    CHECK(a.sent_final == 1);
    CHECK(dist3(&b.box, &put) < 0.01f);  /* on the checker on the partner's PC too */
    CHECK(b.sent == 0);
    /* held with no holder known and a relative +0x40: no sample at all */
    os_xf_t o;
    CHECK(!os_carry_sample(OS_MOVE_CARRY, 1, &rel, NULL, NULL, &a.e.base, &o));
    float far_h[3] = {5000, 2000, -12000}, mt2[3] = {0, 0, 0};   /* neither the relative +0x40 nor the matrix lies at the holder: no sample */
    CHECK(!os_carry_sample(OS_MOVE_CARRY, 1, &rel, mt2, far_h, &a.e.base, &o));
    /* a push box and a carried object at rest pass through */
    CHECK(os_carry_sample(OS_MOVE_PRESS, 1, &rel, NULL, NULL, &a.e.base, &o) && o.p[0] == rel.p[0]);
    CHECK(os_carry_sample(OS_MOVE_CARRY, 0, &rel, NULL, NULL, &a.e.base, &o) && o.p[0] == rel.p[0]);
}

/* pair run wpB3d: after the put-down the box stays in action 1 with the carrier as its actor (23:13:18, 23:20:15). Barry carries (154,1,4) off the hole and puts it down
 * on his PC; later Natalia picks it up on HER PC (23:21:42) and carries it to x 27766. Without os_carry_actor Barry's PC stays the box's local owner: no final rest of his
 * put-down and none of Natalia's samples applied (copy 1 kept the box at 27507.7). Each PC's copy: free (+0x40 = +0xE0 = world) or attached (+0x40 relative, +0xE0 world). */
static void cpc_raw(const pc_t *p, int attached, const os_xf_t *rel, const float mt[3], os_xf_t *raw, float m[3])
{
    *raw = attached ? *rel : p->box;
    for (int i = 0; i < 3; i++) m[i] = attached ? mt[i] : p->box.p[i];
}
static void cpc_tick(pc_t *p, int attached, const os_xf_t *rel, const float mt[3], const float *h, unsigned now, wire_t *out, int slot)
{
    os_xf_t raw, cur;
    float m[3];
    cpc_raw(p, attached, rel, mt, &raw, m);
    if (!os_carry_sample(OS_MOVE_CARRY, 1, &raw, m, h, &p->e.base, &cur)) cur = p->e.base;
    p->box = cur;
    int act = p->actor;
    p->actor = os_carry_actor(OS_MOVE_CARRY, 1, act, &raw, m);
    pc_tick(p, now, out, slot);
    p->actor = act;
}
static void cpc_recv(pc_t *p, int attached, const os_xf_t *rel, const float mt[3], wire_t *in, unsigned now)
{
    os_xf_t raw;
    float m[3];
    cpc_raw(p, attached, rel, mt, &raw, m);
    int act = p->actor;
    p->actor = os_carry_actor(OS_MOVE_CARRY, 1, act, &raw, m);
    pc_recv(p, in, now);
    p->actor = act;
}
static void t_prop_carry_after_put(void)
{
    pc_t a, b;
    wire_t ab = {0}, ba = {0};
    pc_init(&a, 27815.6f, 4165.5f, -29952.4f), pc_init(&b, 27815.6f, 4165.5f, -29952.4f);
    unsigned now = 90000;
    os_xf_t rel = {{-81.2f, -74.4f, 10.5f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    float h[3], mt[3];
    a.actor = OS_ACT_LOCAL;   /* Barry (copy 1) holds it: attached */
    for (int k = 1; k <= 10; k++, now += 100) {
        h[0] = 27815.6f - 300.0f * k / 10, h[1] = 4165.6f, h[2] = -29860.9f;
        mt[0] = h[0] - 75.0f, mt[1] = h[1] + 24.0f, mt[2] = h[2] - 92.0f;
        cpc_tick(&a, 1, &rel, mt, h, now, &ab, 0);
        cpc_recv(&b, 0, &rel, mt, &ab, now), cpc_tick(&b, 0, &rel, mt, NULL, now, &ba, 1);
    }
    CHECK(b.applied >= 5);
    CHECK(dist3(&b.box, &a.box) < 0.01f);
    os_xf_t put = {{27507.7f, 4165.6f, -29952.8f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    a.box = put;   /* put down: free again, the action's actor is still Barry (a.actor stays LOCAL) */
    for (int k = 0; k < 30; k++, now += 100) {
        cpc_tick(&a, 0, &rel, mt, h, now, &ab, 0);
        cpc_recv(&b, 0, &rel, mt, &ab, now), cpc_tick(&b, 0, &rel, mt, NULL, now, &ba, 1);
    }
    CHECK(a.sent_final == 1);             /* the put-down ends with a final rest */
    CHECK(dist3(&b.box, &put) < 0.01f);
    CHECK(b.sent == 0);
    int a_sent = a.sent, a_applied = a.applied;
    b.actor = OS_ACT_LOCAL;   /* Natalia (copy 2) picks it up on her PC and carries it east; on copy 1 the box keeps Barry as its actor */
    for (int k = 1; k <= 10; k++, now += 100) {
        h[0] = 27598.7f + 242.0f * k / 10, h[1] = 4165.6f, h[2] = -29953.0f;
        mt[0] = h[0] - 75.0f, mt[1] = h[1] + 24.0f, mt[2] = h[2];
        cpc_tick(&b, 1, &rel, mt, h, now, &ba, 1);
        cpc_recv(&a, 0, &rel, mt, &ba, now), cpc_tick(&a, 0, &rel, mt, NULL, now, &ab, 0);
        cpc_recv(&b, 1, &rel, mt, &ab, now);
    }
    os_xf_t put2 = {{27766.4f, 4165.6f, -29952.4f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    b.box = put2;
    for (int k = 0; k < 30; k++, now += 100) {
        cpc_tick(&b, 0, &rel, mt, h, now, &ba, 1);
        cpc_recv(&a, 0, &rel, mt, &ba, now), cpc_tick(&a, 0, &rel, mt, NULL, now, &ab, 0);
        cpc_recv(&b, 0, &rel, mt, &ab, now);
    }
    CHECK(a.applied - a_applied >= 5);    /* Barry's PC follows Natalia's carry */
    CHECK(dist3(&a.box, &put2) < 0.01f);  /* and ends where she put it */
    CHECK(a.sent == a_sent);              /* without sending anything back */
    CHECK(b.sent_final == 1);
    /* the rule itself */
    os_xf_t freebox = put;
    CHECK(os_carry_actor(OS_MOVE_CARRY, 1, OS_ACT_LOCAL, &freebox, put.p) == OS_ACT_NONE);
    CHECK(os_carry_actor(OS_MOVE_CARRY, 1, OS_ACT_LOCAL, &rel, put.p) == OS_ACT_LOCAL);
    CHECK(os_carry_actor(OS_MOVE_CARRY, 1, OS_ACT_LOCAL, &rel, NULL) == OS_ACT_LOCAL);
    CHECK(os_carry_actor(OS_MOVE_PRESS, 1, OS_ACT_LOCAL, &freebox, put.p) == OS_ACT_LOCAL);
    CHECK(os_carry_actor(OS_MOVE_CARRY, 0, OS_ACT_REMOTE, &freebox, put.p) == OS_ACT_REMOTE);
}

static void t_prop_conflict(void)
{
    pc_t a, b;
    wire_t ab = {0}, ba = {0};
    pc_init(&a, 0, 0, 0), pc_init(&b, 0, 0, 0);
    unsigned now = 5000;
    /* both humans grab their own copy at the same time: each PC keeps its own human's box while he holds it */
    a.actor = b.actor = OS_ACT_LOCAL;
    for (int k = 0; k < 5; k++, now += 100) {
        a.box.p[0] += 10, b.box.p[0] -= 10;
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    CHECK(a.applied == 0 && b.applied == 0);
    CHECK(a.box.p[0] > 40 && b.box.p[0] < -40);
    /* B lets go first: after A lets go too, A's final REST makes both equal (the last to let go wins) */
    b.actor = OS_ACT_NONE;
    for (int k = 0; k < 25; k++, now += 100) {
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    a.actor = OS_ACT_NONE;
    for (int k = 0; k < 30; k++, now += 100) {
        pc_tick(&a, now, &ab, 0), pc_tick(&b, now, &ba, 1);
        pc_recv(&b, &ab, now), pc_recv(&a, &ba, now);
    }
    CHECK(dist3(&a.box, &b.box) < 0.01f);
}

static void t_prop_wire(void)
{
    os_prop_msg_t m, r;
    memset(&m, 0, sizeof m);
    m.a = 154, m.b = 1, m.c = 4, m.flags = OS_PF_REST | OS_PF_FINAL, m.slot = 1, m.seq = 65535, m.act = 0;
    m.xf.p[0] = 27815.6f, m.xf.p[1] = 4165.4f, m.xf.p[2] = -29952.4f, m.xf.q[1] = 0.7071f, m.xf.q[3] = 0.7071f;
    unsigned char b[64];
    int len = os_prop_pack(&m, b);
    CHECK(len == OS_PROP_LEN);
    CHECK(os_prop_unpack(b, len, &r));
    CHECK(r.a == 154 && r.b == 1 && r.c == 4 && r.flags == m.flags && r.slot == 1 && r.seq == 65535);
    CHECK(r.xf.p[2] == m.xf.p[2] && r.xf.q[1] == m.xf.q[1]);
    CHECK(!os_prop_unpack(b, len - 1, &r));
    float bad = 1.0e30f;
    memcpy(b + 12, &bad, 4);
    CHECK(!os_prop_unpack(b, len, &r));
    /* sequence wrap */
    CHECK(os_seq_newer(1, 65535));
    CHECK(!os_seq_newer(65535, 1));
    CHECK(!os_seq_newer(7, 7));
    os_prop_t e;
    os_xf_t z = {{0, 0, 0}, {0, 0, 0, 1}};
    os_prop_learn(&e, &z);
    CHECK(os_prop_accept(&e, 10, OS_ACT_NONE, 100));
    CHECK(!os_prop_accept(&e, 9, OS_ACT_NONE, 200));     /* an older sample arriving late */
    CHECK(os_prop_accept(&e, 11, OS_ACT_NONE, 300));
    CHECK(!os_prop_accept(&e, 12, OS_ACT_LOCAL, 400));   /* this PC's human holds it */
}

/* ---- G3 / G4: scenario flags ---- */
static void t_sf(void)
{
    unsigned prev[OS_SF_WORDS] = {0}, cur[OS_SF_WORDS] = {0};
    int out[64];
    os_sf_setbit(cur, 5);
    os_sf_setbit(cur, 320);   /* 154: the generator back door (SmSleep at SF[320]) */
    os_sf_setbit(cur, 300);   /* 154: F300_goal_demo, the end of the stage */
    os_sf_setbit(cur, 511);   /* 152: set with StageClear */
    CHECK(os_sf_bit(cur, 320) && !os_sf_bit(cur, 321));
    CHECK((cur[10] & (0x80000000u >> 0)) != 0);              /* flag 320 = word 10, top bit (the game's bit order) */
    int n = os_sf_rising(prev, cur, 0, out, 64);
    CHECK(n == 4);
    int has320 = 0, has511 = 0;
    for (int i = 0; i < n; i++) has320 |= out[i] == 320, has511 |= out[i] == 511;
    CHECK(has320 && has511);
    CHECK(os_sf_index_ok(305));
    CHECK(!os_sf_index_ok(512));
    CHECK(!os_sf_index_ok(-1));
    /* EpisodeFlag[2] and [3]: the two push box checkers of 101 */
    unsigned ep0[OS_SF_WORDS] = {0}, ep1[OS_SF_WORDS] = {0};
    os_sf_setbit(ep1, 2), os_sf_setbit(ep1, 3);
    n = os_sf_rising(ep0, ep1, 1, out, 64);
    CHECK(n == 2 && out[0] == 2 && out[1] == 3);
}

static void t_digest(void)
{
    /* A crosses the 153 -> 151 border first: zone151#0 sets SF[5] inside A's quiet window (absorbed by A's own scan); B's PC never got it in release-020 */
    unsigned a_w[OS_SF_WORDS] = {0}, b_w[OS_SF_WORDS] = {0}, w[OS_SF_WORDS];
    os_sf_setbit(a_w, 5), os_sf_setbit(a_w, 30), os_sf_setbit(b_w, 30);
    int was = 1;
    CHECK(!os_digest_due(1, 1));
    CHECK(os_digest_due(was, 0));
    CHECK(!os_digest_due(0, 0));
    /* pair run osPB 07:16:53 (`work\runs\20261107-070911-osPB`): the 151 baseline was taken while the glue was loading (barrier open 23 s later); the old step dropped the
     * digest at the next scan because the session was not PLAYING. It waits now, and goes out once */
    int pend = 1;
    CHECK(!os_digest_step(&pend, 0) && pend == 1);
    CHECK(!os_digest_step(&pend, 0) && pend == 1);
    CHECK(os_digest_step(&pend, 1) && pend == 0);
    CHECK(!os_digest_step(&pend, 1) && pend == 0);
    pend = 0;
    CHECK(!os_digest_step(&pend, 1));
    unsigned char b[OS_DIGEST_LEN + 8];
    int len = os_digest_pack(151, 0, a_w, b), area = 0, kind = -1;
    CHECK(len == OS_DIGEST_LEN && len <= 96);
    CHECK(os_digest_unpack(b, len, &area, &kind, w) && area == 151 && kind == 0);
    int out[64];
    int n = os_digest_missing(w, b_w, OS_SF_LIMIT, out, 64);
    CHECK(n == 1 && out[0] == 5);
    CHECK(!os_digest_unpack(b, 5, &area, &kind, w));
    b[5] = 40;
    CHECK(!os_digest_unpack(b, len, &area, &kind, w));
    /* B is still in area 153 when A's digest of 151 arrives: kept, applied when B's area turns 151 within 15 s */
    os_pend_t p;
    memset(&p, 0, sizeof p);
    os_pend_store(&p, 151, a_w, 1000);
    CHECK(!os_pend_take(&p, 153, 2000, w));
    CHECK(os_pend_take(&p, 151, 4000, w) && os_sf_bit(w, 5));
    CHECK(!os_pend_take(&p, 151, 4100, w));                 /* taken once */
    os_pend_store(&p, 152, a_w, 1000);
    CHECK(!os_pend_take(&p, 152, 1000 + OS_PEND_MS + 1, w)); /* too old: dropped */
    /* a high flag in a digest: SF[320] of 154 */
    unsigned c_w[OS_SF_WORDS] = {0};
    os_sf_setbit(c_w, 320);
    n = os_digest_missing(c_w, b_w, OS_SF_LIMIT, out, 64);
    CHECK(n == 1 && out[0] == 320);
}

int main(void)
{
    t_kinds();
    t_door_policy();
    t_replay_echo();
    t_coop_plan();
    t_prop_push();
    t_prop_carry();
    t_prop_carry_after_put();
    t_prop_conflict();
    t_prop_wire();
    t_sf();
    t_digest();
#ifdef OS_MODEL_RELEASE020
    printf("objsync (model of release-020): %d checks, %d failed\n", g_checks, g_fail);
#else
    printf("objsync: %d checks, %d failed\n", g_checks, g_fail);
#endif
    return g_fail ? 1 : 0;
}
