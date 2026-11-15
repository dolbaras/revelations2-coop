/* test_pmot.c: offline checks of native/pmot_logic.h and the STATE motion tail of native/coop_proto.c (0.3.2 anim032, notes\motion-mirror.md).
 *   1. the wire: a state with the tail round-trips; a state without it is the same 60 bytes as before (an older peer reads exactly that); a malformed tail is dropped whole and
 *      the rest of the state stands; the 60-byte prefix of a tailed state decodes as the plain state (forward rule);
 *   2. the sender: the ids the action mirror replays (knife, shoot, dodge, crouch, walk ...) never carry the tail; doors / levers (INT_MIN), pickup, put down, ladders,
 *      climbs, crab walk do; wait only as a continuation; never for an enemy's hold, an INT_MIN with a stranger's bank 15, die / dying / rescue;
 *   3. the receiver: enter / run / exit / drop, blocked while the puppet is scripted here, dead, down or held by an enemy, the lost tail;
 *   4. one layer: set / keep / resync / stop, refusals sticky until the owner's id changes, a fight with the puppet's own controller ends after PM_RESET_MAX re-sets;
 *   5. bank 15: references balanced over enter / exit / stage unload / despawn / a stock attach in between (a model of the resource manager, use after free detected);
 *   6. the handle: the lever / board / rock steps the receiver skips carry the motion, the replayed end poses do not, the DOOR body stays a plain set model's for old receivers;
 *   7. timelines: a lever pull (2 -> INT_MIN -> 0 -> 2) and a pickup while walking (3 -> 127 -> 3) are mirrored and handed back to the action mirror; a combat burst never is.
 * Built a second time with -DPM_MODEL_BEFORE (rel032 cfd6200: no motion mirror): that model must fail (tests/build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/pmot_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 60) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static uint32_t g_bits[8];

static cp_state base_state(void)
{
    cp_state s;
    memset(&s, 0, sizeof s);
    s.pid = 1, s.seq = 77, s.t_ms = 123456, s.pos[0] = 1.5f, s.pos[1] = -2.0f, s.pos[2] = 3000.0f, s.yaw = 0.5f, s.action = 127, s.hp = 900, s.hpmax = 1000;
    s.aim = 12.5f, s.weapon = 1, s.epoch = 3, s.flags = 8, s.pstate = 0x10, s.ctrl = 0x8, s.item[0] = 0x14d, s.move = 100, s.elev = -50;
    return s;
}
static void add_layer(cp_state *s, unsigned l, unsigned id, unsigned nf, float frame, float speed, float w)
{
    cp_motion *m = &s->mo[s->mo_n++];
    m->layer = (uint8_t)l, m->id = (uint16_t)id, m->nframes = (uint16_t)nf, m->frame = frame, m->speed = speed, m->weight = w, m->blend = 8, m->flags = 0x44;
}
static int same_base(const cp_state *a, const cp_state *b)
{
    return a->pid == b->pid && a->seq == b->seq && a->t_ms == b->t_ms && a->action == b->action && a->hp == b->hp && a->hpmax == b->hpmax && a->weapon == b->weapon && a->epoch == b->epoch &&
           a->flags == b->flags && a->pstate == b->pstate && a->ctrl == b->ctrl && a->item[0] == b->item[0] && a->move == b->move && a->elev == b->elev;
}

/* 1. the wire */
static void test_wire(void)
{
    cp_msg m, d;
    uint8_t buf[CP_MAX_PACKET];
    memset(&m, 0, sizeof m);
    m.type = CP_STATE, m.u.state = base_state();
    size_t n0 = cp_encode(&m, buf, sizeof buf);
    CHECK(n0 == 60);   /* no tail: the bytes of 0.3.1 */
    CHECK(cp_decode(buf, n0, &d) && d.u.state.mo_n == 0 && same_base(&d.u.state, &m.u.state));

    cp_state *s = &m.u.state;
    s->action = PM_INT_MIN, s->mo_kind = CP_MOK_SETMODEL, s->mo_src_bank = 1, s->mo_src_a = 101, s->mo_src_b = 2, s->mo_src_c = 1;
    add_layer(s, 0, 0xF00, 91, 12.25f, 1.0f, 1.0f);
    add_layer(s, 1, 0xF01, 91, 12.25f, 1.0f, 0.75f);
    add_layer(s, 11, 0x747, 40, 3.0f, 1.5f, 0.5f);
    size_t n1 = cp_encode(&m, buf, sizeof buf);
    CHECK(n1 == 60 + 10 + 3 * 18);
    CHECK(cp_decode(buf, n1, &d) && same_base(&d.u.state, s));
    const cp_state *r = &d.u.state;
    CHECK(r->mo_n == 3 && r->mo_kind == CP_MOK_SETMODEL && r->mo_src_bank == 1 && r->mo_src_a == 101 && r->mo_src_b == 2 && r->mo_src_c == 1);
    CHECK(r->mo[0].layer == 0 && r->mo[0].id == 0xF00 && r->mo[0].nframes == 91 && r->mo[0].frame == 12.25f && r->mo[0].speed == 1.0f && r->mo[0].weight == 1.0f && r->mo[0].blend == 8 && r->mo[0].flags == 0x44);
    CHECK(r->mo[1].weight > 0.749f && r->mo[1].weight < 0.751f && r->mo[2].layer == 11 && r->mo[2].id == 0x747 && r->mo[2].speed == 1.5f);
    /* an older decoder reads the first 60 bytes: exactly the plain state */
    CHECK(cp_decode(buf, 60, &d) && d.u.state.mo_n == 0 && same_base(&d.u.state, s));
    /* the full 16 layers fit a packet */
    cp_msg big = m;
    big.u.state.mo_n = 0;
    for (unsigned l = 0; l < 16; l++) add_layer(&big.u.state, l, 0x100 + l, 30, (float)l, 1.0f, 1.0f);
    size_t nb = cp_encode(&big, buf, sizeof buf);
    CHECK(nb == 60 + 10 + 16 * 18 && nb <= CP_MAX_PACKET);
    CHECK(cp_decode(buf, nb, &d) && d.u.state.mo_n == 16 && d.u.state.mo[15].id == 0x10F);
    /* malformed tails: dropped whole, the state stands */
    size_t n2 = cp_encode(&m, buf, sizeof buf);
    uint8_t bad[CP_MAX_PACKET];
    memcpy(bad, buf, n2), bad[61] = 17;   /* count > 16 */
    CHECK(cp_decode(bad, n2, &d) && d.u.state.mo_n == 0 && same_base(&d.u.state, s));
    memcpy(bad, buf, n2), bad[70 + 18] = 0;   /* the second layer says layer 0 again */
    CHECK(cp_decode(bad, n2, &d) && d.u.state.mo_n == 0 && same_base(&d.u.state, s));
    memcpy(bad, buf, n2), bad[70 + 5] = 0x28, bad[70 + 6] = 0x6B, bad[70 + 7] = 0x6E, bad[70 + 8] = 0x4E;   /* frame = 1e9 (a NaN reads as 0 in rf) */
    CHECK(cp_decode(bad, n2, &d) && d.u.state.mo_n == 0 && same_base(&d.u.state, s));
    CHECK(cp_decode(buf, n2 - 5, &d) && d.u.state.mo_n == 0 && same_base(&d.u.state, s));   /* cut short */
    memcpy(bad, buf, n2), bad[60] = 0x00;   /* no magic: not a tail */
    CHECK(cp_decode(bad, n2, &d) && d.u.state.mo_n == 0);
}

/* 2. the sender */
static void test_sender(void)
{
    pm_tx_t t;
    pm_tx_reset(&t);
    static const uint32_t mask_ids[] = {2, 3, 4, 8, 13, 17, 21, 22, 24, 28, 32, 43, 47, 86, 87, 158, 172, 205};
    for (unsigned i = 0; i < sizeof mask_ids / sizeof *mask_ids; i++) CHECK(!pm_tx_want(&t, g_bits, mask_ids[i], 0, 0, 0));
    static const uint32_t covered[] = {PM_INT_MIN, 127, 126, 42, 55, 57, 58, 63, 68, 69, 72, 75, 76, 81, 82, 83, 85, 100, 104, 107};
    for (unsigned i = 0; i < sizeof covered / sizeof *covered; i++) {
        pm_tx_reset(&t);
        CHECK(pm_tx_want(&t, g_bits, covered[i], 0, 0, 0));
    }
    pm_tx_reset(&t);
    CHECK(!pm_tx_want(&t, g_bits, 0, 0, 0, 0) && !pm_tx_want(&t, g_bits, 1, 0, 0, 0));   /* wait alone: no */
    CHECK(pm_tx_want(&t, g_bits, PM_INT_MIN, 0, 1, 1) && pm_tx_want(&t, g_bits, 0, 0, 0, 0) && pm_tx_want(&t, g_bits, 1, 0, 0, 0));   /* after a door: yes */
    CHECK(!pm_tx_want(&t, g_bits, 2, 0, 0, 0) && !pm_tx_want(&t, g_bits, 0, 0, 0, 0));   /* idle in between ends it */
    static const uint32_t never[] = {34, 35, 36, 37, 38, 108, 113, 118, 119, 121, 122, 123, 124, 125, 128, 129, 130, 149, 150, 151, 217, 225, 238, 241, 242, 300};
    for (unsigned i = 0; i < sizeof never / sizeof *never; i++) {
        pm_tx_reset(&t);
        CHECK(!pm_tx_want(&t, g_bits, never[i], 0, 0, 0));
    }
    pm_tx_reset(&t);
    CHECK(!pm_tx_want(&t, g_bits, PM_INT_MIN, 1, 0, 0) && t.n_enemy == 1);   /* held by an enemy: its own path */
    CHECK(!pm_tx_want(&t, g_bits, PM_INT_MIN, 0, 1, 0) && t.n_nosrc == 1);   /* bank 15 of a stranger (an enemy's grab list, a scene) */
    CHECK(pm_tx_want(&t, g_bits, 126, 0, 1, 0));   /* a put-down with an unknown list: the tail goes, the receiver leaves the bank-15 layers alone */
    /* the mask decides: an id the console added to the action mirror leaves the motion mirror */
    uint32_t b2[8];
    memcpy(b2, g_bits, sizeof b2);
    b2[127 >> 5] |= 1u << (127 & 31);
    pm_tx_reset(&t);
    CHECK(!pm_tx_want(&t, b2, 127, 0, 0, 0));
}

/* 3. the receiver's mode */
static int rx(pm_rx_t *r, uint32_t pup, uint32_t act, int tail, uint32_t now)
{
    pm_rx_in in;
    memset(&in, 0, sizeof in);
    in.pup = pup, in.own_act = act, in.tail = tail, in.enabled = 1, in.now = now;
    return pm_rx_decide(r, g_bits, &in);
}
static void test_receiver(void)
{
    pm_rx_t r;
    pm_rx_reset(&r);
    CHECK(rx(&r, 0x1000, 3, 0, 100) == PM_RX_OFF);
    CHECK(rx(&r, 0x1000, 127, 0, 110) == PM_RX_OFF);                      /* no tail (the partner's build or switch): nothing */
    CHECK(rx(&r, 0x1000, 127, 1, 120) == PM_RX_ENTER && r.on);
    CHECK(rx(&r, 0x1000, 127, 1, 130) == PM_RX_RUN);
    CHECK(rx(&r, 0x1000, 0, 1, 140) == PM_RX_RUN);                        /* wait continues */
    CHECK(rx(&r, 0x1000, 2, 0, 150) == PM_RX_EXIT && !r.on && !r.exit_quiet);   /* a mask id: back to the action mirror */
    CHECK(rx(&r, 0x1000, 0, 1, 160) == PM_RX_OFF);                        /* wait does not enter */
    CHECK(rx(&r, 0x1000, PM_INT_MIN, 1, 170) == PM_RX_ENTER);
    CHECK(rx(&r, 0x2000, PM_INT_MIN, 1, 180) == PM_RX_DROP && !r.on && r.n_drop == 1);   /* the puppet object changed */
    CHECK(rx(&r, 0x2000, PM_INT_MIN, 1, 190) == PM_RX_ENTER && r.pup == 0x2000);
    CHECK(rx(&r, 0x2000, PM_INT_MIN, 0, 300) == PM_RX_RUN);               /* a sample without a tail: still within PM_TAIL_LOST_MS */
    CHECK(rx(&r, 0x2000, PM_INT_MIN, 0, 191 + PM_TAIL_LOST_MS) == PM_RX_EXIT);
    /* blocks */
    pm_rx_in in;
    static const int which[] = {0, 1, 2, 3, 4};
    for (unsigned k = 0; k < 5; k++) {
        pm_rx_reset(&r);
        memset(&in, 0, sizeof in);
        in.pup = 0x3000, in.own_act = 69, in.tail = 1, in.enabled = 1, in.now = 1000;
        CHECK(pm_rx_decide(&r, g_bits, &in) == PM_RX_ENTER);
        in.now = 1010;
        if (which[k] == 0) in.scripted = 1;
        if (which[k] == 1) in.dead_down = 1;
        if (which[k] == 2) in.held_enemy = 1;
        if (which[k] == 3) in.enabled = 0;
        if (which[k] == 4) in.pup = 0;
        int d = pm_rx_decide(&r, g_bits, &in);
        CHECK(d == (which[k] == 4 ? PM_RX_DROP : PM_RX_EXIT));
        if (which[k] <= 1) CHECK(r.exit_quiet);   /* the stock event / downed1 owns the puppet: the exit touches nothing */
        CHECK(pm_rx_decide(&r, g_bits, &in) == PM_RX_OFF);   /* and does not come back while blocked */
    }
    /* the hold: once at the entry, again at most once a second */
    pm_rx_reset(&r);
    rx(&r, 0x1000, 127, 1, 5000);
    CHECK(pm_rx_hold(&r, 2, 5000) == 1 && r.n_hold == 1);
    CHECK(pm_rx_hold(&r, 0, 5010) == 0);
    CHECK(pm_rx_hold(&r, 3, 5500) == 0);
    CHECK(pm_rx_hold(&r, 3, 6001) == 1 && r.n_rehold == 1);
}

/* 4. one layer */
static pm_layer_in lin(uint16_t oid, uint16_t pid, uint32_t now)
{
    pm_layer_in in;
    memset(&in, 0, sizeof in);
    in.allowed = 1, in.have = oid != PM_NO_ID, in.oid = oid, in.onframes = 60, in.oframe = 10.0f, in.pid = pid, in.pframe = 10.0f, in.found_frames = 60, in.storage_ok = 1, in.bank_ok = 1, in.now = now;
    return in;
}
static void test_layer(void)
{
    pm_rx_t r;
    pm_rx_reset(&r);
    uint8_t why;
    pm_layer_in in = lin(0x101, 0x005, 1000);
    CHECK(pm_layer_plan(&r, 0, &in, &why) == PM_L_SET && r.set_id[0] == 0x101 && r.n_set == 1);
    in = lin(0x101, 0x101, 1016);
    CHECK(pm_layer_plan(&r, 0, &in, &why) == PM_L_KEEP);
    in.new_sample = 1, in.pframe = 30.0f;
    CHECK(pm_layer_plan(&r, 0, &in, &why) == PM_L_RESYNC && r.n_resync == 1);
    in.pframe = 59.0f, in.oframe = 1.0f;   /* a looping motion just wrapped on one side: 2 frames apart, not 58 */
    CHECK(pm_layer_plan(&r, 0, &in, &why) == PM_L_KEEP);
    /* refusals are sticky until the owner's id changes */
    pm_rx_reset(&r);
    in = lin(0x305, 0x005, 2000);
    in.found_frames = 0;
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_REFUSE && why == PM_WHY_MOTION);
    in.now = 2016;
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_NONE && r.n_ref_motion == 1);
    in = lin(0x306, 0x005, 2032);
    in.found_frames = 61;
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_REFUSE && why == PM_WHY_FRAMES);
    in = lin(0x307, 0x005, 2048);
    in.storage_ok = 0;
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_REFUSE && why == PM_WHY_STORAGE);
    in = lin(0xF00, 0x005, 2064);
    in.bank_ok = 0;
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_REFUSE && why == PM_WHY_BANK);
    in.bank_ok = 1, in.now = 2080;
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_NONE);   /* the same id stays refused, also after the bank came */
    in = lin(0xF01, 0x005, 2096);
    CHECK(pm_layer_plan(&r, 2, &in, &why) == PM_L_SET);    /* a new id of the owner: tried */
    /* a fight with the puppet's own controller: at most PM_RESET_MAX re-sets, each >= PM_RESET_GAP_MS after the last, then left alone */
    pm_rx_reset(&r);
    unsigned t = 3000, sets = 0, refused = 0;
    for (int f = 0; f < 120; f++, t += 16) {
        in = lin(0x40A, f == 0 ? 0x001 : 0x0AA, t);   /* the controller puts 0x0AA back every frame */
        int p = pm_layer_plan(&r, 10, &in, &why);
        if (p == PM_L_SET) sets++;
        else if (p == PM_L_REFUSE) { refused++; CHECK(why == PM_WHY_LOST); }
    }
    CHECK(sets == 1 + PM_RESET_MAX && refused == 1 && r.n_gaveup == 1 && r.n_reset == PM_RESET_MAX);
    /* owner layer empty: the puppet's layer stopped, a controller re-filling it is stopped again a bounded number of times */
    pm_rx_reset(&r);
    in = lin(PM_NO_ID, 0x0AA, 4000);
    CHECK(pm_layer_plan(&r, 11, &in, &why) == PM_L_STOP && r.n_stop == 1);
    in = lin(PM_NO_ID, PM_NO_ID, 4016);
    CHECK(pm_layer_plan(&r, 11, &in, &why) == PM_L_NONE);
    unsigned stops = 0;
    t = 4032;
    for (int f = 0; f < 60; f++, t += 16) {
        in = lin(PM_NO_ID, 0x0AA, t);
        if (pm_layer_plan(&r, 11, &in, &why) == PM_L_STOP) stops++;
    }
    CHECK(stops == PM_RESET_MAX && r.n_gaveup == 1);
    in = lin(0x0AB, 0x0AA, t);   /* the owner plays the layer again: mirrored again */
    CHECK(pm_layer_plan(&r, 11, &in, &why) == PM_L_SET);
    /* the layer mask */
    pm_rx_reset(&r);
    in = lin(0x101, 0x005, 5000);
    in.allowed = 0;
    CHECK(pm_layer_plan(&r, 3, &in, &why) == PM_L_NONE && r.touched == 0);
    /* frame projection: from the time the sample became the shown one, measured rate, capped */
    cp_state o;
    memset(&o, 0, sizeof o);
    pm_rx_reset(&r);
    o.seq = 1, o.t_ms = 10000, o.mo_n = 1, o.mo[0].layer = 0, o.mo[0].id = 0x101, o.mo[0].frame = 10.0f, o.mo[0].speed = 1.0f;
    CHECK(pm_rx_sample(&r, &o) == 1 && pm_rx_sample(&r, &o) == 0);
    CHECK(fabsf(pm_frame_now(&r, &o.mo[0], 10100) - (10.0f + PM_RATE_DEF * 100.0f)) < 0.01f);   /* no rate yet: speed x the default */
    o.seq = 2, o.t_ms = 10050, o.mo[0].frame = 13.0f;   /* 3 frames in 50 ms */
    pm_rx_sample(&r, &o);
    CHECK(fabsf(r.rate[0] - 0.06f) < 0.0001f);
    CHECK(fabsf(pm_frame_now(&r, &o.mo[0], 10100) - 16.0f) < 0.01f);
    CHECK(fabsf(pm_frame_now(&r, &o.mo[0], 20000) - (13.0f + 0.06f * PM_EXTRA_MS)) < 0.01f);   /* capped */
}

/* 5. bank 15: a model of the resource manager */
#define NRES 8
static int g_ref[NRES + 1], g_uaf;
static uint32_t g_bank15;   /* the puppet's bank 15 */
static void res_addref(uint32_t r) { if (!r) return; if (g_ref[r] <= 0) g_uaf++; g_ref[r]++; }
static void res_release(uint32_t r) { if (!r) return; if (g_ref[r] <= 0) g_uaf++; g_ref[r]--; }
static void stock_attach(uint32_t r)   /* FUN_00D422C0: release the old one, add-ref the new one */
{
    if (g_bank15) res_release(g_bank15);
    g_bank15 = 0;
    if (r) res_addref(r);
    g_bank15 = r;
}
static void run_ops(const pm_op_t *ops, int n)
{
    for (int i = 0; i < n; i++) {
        if (ops[i].op == PM_OP_ADDREF) res_addref(ops[i].res);
        else if (ops[i].op == PM_OP_RELEASE) res_release(ops[i].res);
        else if (ops[i].op == PM_OP_ATTACH) stock_attach(ops[i].res);
    }
}
static void world_reset(uint32_t own)
{
    memset(g_ref, 0, sizeof g_ref), g_uaf = 0, g_bank15 = 0;
    for (int i = 1; i <= NRES; i++) g_ref[i] = 1;   /* each held once by its owner (the set model / the stage) */
    if (own) stock_attach(own);                     /* the puppet's own bank 15 from an earlier stock event */
}
static void test_bank(void)
{
    pm_bank_t b;
    pm_op_t ops[3];
    for (uint32_t own = 0; own <= 1; own++) {   /* with and without a list of the puppet's own in bank 15 */
        /* enter, change of the set model, exit */
        world_reset(own ? 5 : 0);
        memset(&b, 0, sizeof b);
        int r5 = g_ref[5];
        run_ops(ops, pm_bank_use(&b, g_bank15, 2, ops));
        CHECK(g_bank15 == 2 && g_ref[2] == 2 && (!own || g_ref[5] == r5));
        run_ops(ops, pm_bank_use(&b, g_bank15, 2, ops));   /* the same: nothing */
        CHECK(g_ref[2] == 2);
        run_ops(ops, pm_bank_use(&b, g_bank15, 3, ops));   /* the owner moved to another set model */
        CHECK(g_bank15 == 3 && g_ref[2] == 1 && g_ref[3] == 2);
        run_ops(ops, pm_bank_end(&b, 1, g_bank15, ops));
        CHECK(g_bank15 == (own ? 5u : 0u) && g_ref[2] == 1 && g_ref[3] == 1 && (!own || g_ref[5] == r5) && !g_uaf && !b.active);
        /* stage unload while on: the set model drops its own reference first, ours keeps the list alive until we give it back */
        world_reset(own ? 5 : 0);
        memset(&b, 0, sizeof b);
        run_ops(ops, pm_bank_use(&b, g_bank15, 4, ops));
        res_release(4);   /* the stage frees the set model */
        CHECK(g_ref[4] == 1);   /* still alive: the puppet's bank holds it */
        run_ops(ops, pm_bank_end(&b, 1, g_bank15, ops));
        CHECK(g_ref[4] == 0 && g_bank15 == (own ? 5u : 0u) && !g_uaf);
        if (own) CHECK(g_ref[5] == 2);   /* back to its owner + the puppet's bank, as before */
        /* despawn while on: the puppet's destructor releases its bank 15 (ours); we drop only the keep reference, no call on the old puppet */
        world_reset(own ? 5 : 0);
        memset(&b, 0, sizeof b);
        run_ops(ops, pm_bank_use(&b, g_bank15, 2, ops));
        stock_attach(0);   /* ~uModel: the bank released */
        run_ops(ops, pm_bank_end(&b, 0, 0, ops));
        CHECK(g_ref[2] == 1 && !g_uaf && (!own || g_ref[5] == 1));   /* the puppet's own list: its bank reference went with the puppet (released by our attach, our keep released now) */
        for (int i = 1; i <= NRES; i++) if (i != 5 || !own) CHECK(g_ref[i] == 1);
        /* a stock attach in between (an enemy grabs the puppet on this PC): not touched, the exit leaves the enemy's list in place */
        world_reset(own ? 5 : 0);
        memset(&b, 0, sizeof b);
        run_ops(ops, pm_bank_use(&b, g_bank15, 2, ops));
        stock_attach(7);
        CHECK(pm_bank_use(&b, g_bank15, 2, ops) == -1 && b.n_busy == 1);
        run_ops(ops, pm_bank_end(&b, 1, g_bank15, ops));
        CHECK(g_bank15 == 7 && g_ref[7] == 2 && g_ref[2] == 1 && !g_uaf && (!own || g_ref[5] == 1));
        /* the list already there: nothing owned, nothing given back */
        world_reset(2);
        memset(&b, 0, sizeof b);
        CHECK(pm_bank_use(&b, g_bank15, 2, ops) == 0 && !b.active && pm_bank_end(&b, 1, g_bank15, ops) == 0);
    }
}

/* 5b. bank 15 and the layers: a layer caches pointers into its motion's data, so the list must not lose its last reference while a layer still plays one of its motions */
static void test_bank_layers(void)
{
    pm_rx_t r;
    pm_op_t ops[3];
    uint16_t ids[PM_LAYERS];
    for (int forced = 0; forced <= 1; forced++) {
        pm_rx_reset(&r);
        world_reset(0);
        for (unsigned l = 0; l < PM_LAYERS; l++) ids[l] = PM_NO_ID;
        run_ops(ops, pm_bank_use(&r.bank, g_bank15, 2, ops));
        ids[0] = 0xF00, ids[1] = 0xF01;   /* the lever pull plays on layers 0 / 1 from list 2 */
        res_release(2);                   /* the stage frees the lever: our reference is the last one */
        CHECK(g_ref[2] == 1);
        r.on = 0;                         /* the exit */
        unsigned t = 9000, given = 0, freed_in_use = 0;
        for (int f = 0; f < 400 && r.bank.active; f++, t += 16) {
            if (!forced && f == 10) ids[0] = 0x003, ids[1] = 0x004;   /* the puppet's own action replaced the motions after 10 frames */
            int due = pm_bank_due(&r, ids, t);
            if (due == 2) for (unsigned l = 0; l < PM_LAYERS; l++) if (ids[l] != PM_NO_ID && ((ids[l] >> 8) & 15u) == 15u) ids[l] = PM_NO_ID;   /* the caller stops them */
            if (due) {
                if (pm_ids_use_bank(ids, 15)) freed_in_use++;
                run_ops(ops, pm_bank_end(&r.bank, 1, g_bank15, ops));
                given++;
            }
        }
        CHECK(given == 1 && freed_in_use == 0 && g_ref[2] == 0 && !g_uaf && g_bank15 == 0);
        if (forced) CHECK(r.n_bank_forced == 1 && r.n_bank_wait == 1);
        else CHECK(r.n_bank_forced == 0 && r.n_bank_wait == 1);
    }
    /* nothing of ours / the mirror still on: never due */
    pm_rx_reset(&r);
    for (unsigned l = 0; l < PM_LAYERS; l++) ids[l] = PM_NO_ID;
    CHECK(pm_bank_due(&r, ids, 100) == 0);
    world_reset(0);
    run_ops(ops, pm_bank_use(&r.bank, g_bank15, 3, ops));
    r.on = 1;
    CHECK(pm_bank_due(&r, ids, 200) == 0);
    r.on = 0;
    CHECK(pm_bank_due(&r, ids, 300) == 1);
}

/* 6. the handle */
static void test_handle(void)
{
    int lever = os_kind_of("vtable exe+f594d8, DTI 1573904: uSm1840(43e0) : uSmLever(43e0) : uSmLeverBase(43e0) : uSetModel(4370)");
    int onoff = os_kind_of("vtable exe+f5aa00, DTI 15739a0: uSm1842(43e0) : uSmLeverOnOff(43e0) : uSmLeverBase(43e0) : uSetModel(4370)");
    int board = os_kind_of("vtable exe+f20000, DTI 1570000: uSm1826(4400) : uSmBarBoard(4400) : uSetModel(4370)");
    int rock = os_kind_of("vtable exe+f30000, DTI 1571000: uSm1890(4400) : uSmDrilledRock(4400) : uSetModel(4370)");
    int power = os_kind_of("vtable exe+f594d8, DTI 1573904: uSm1773(43e0) : uSmLever(43e0) : uSmLeverBase(43e0) : uSetModel(4370)");
    for (int act = 0; act <= 4; act++) {
        CHECK(pm_sm_wanted(lever, act) == (act == 1 || act == 3));
        CHECK(pm_sm_wanted(onoff, act) == (act == 1 || act == 3));
        CHECK(pm_sm_wanted(board, act) == (act == 1));
        CHECK(pm_sm_wanted(rock, act) == (act == 1 || act == 3));
        CHECK(!pm_sm_wanted(power, act));   /* uSm1773: replayed by its ChangeAction (null-safe), the motion comes with it */
        CHECK(!pm_sm_wanted(OS_PLAIN, act) && !pm_sm_wanted(OS_COOP_DOOR, act) && !pm_sm_wanted(OS_MOVE_CARRY, act));
        /* exactly the skipped steps (rel032's rule): a replayed step never carries a second motion */
        if (pm_sm_wanted(lever, act)) CHECK(!os_door_apply(lever, act, OS_ACT_NONE));
    }
    /* the DOOR body: 31 bytes of stage 10 (byte 16 = 0: not co-op) + 17 */
    uint8_t body[48];
    memset(body, 0, sizeof body);
    int a = 101, act = 1, prev = 0;
    memcpy(body, &a, 4), body[4] = 2, body[6] = 1, memcpy(body + 8, &act, 4), memcpy(body + 12, &prev, 4);
    pm_sm_t m = {0x0002, 46, 1.5f, 1.0f, 0x44}, d;
    CHECK(pm_sm_enc(body + PM_SM_OFF, &m) == PM_SM_LEN && PM_SM_OFF + PM_SM_LEN == 48 && 48 <= CP_EVENT_MAX);
    CHECK(pm_sm_dec(body, 48, &d) && d.id == 2 && d.nframes == 46 && d.frame == 1.5f && d.speed == 1.0f && d.flags == 0x44);
    CHECK(body[16] == 0);   /* an older receiver: a plain set model's DOOR, acts on bytes 0..15 only */
    CHECK(!pm_sm_dec(body, 31, &d) && !pm_sm_dec(body, 16, &d));   /* the bodies of 0.3.1 */
    body[PM_SM_OFF] = 0;
    CHECK(!pm_sm_dec(body, 48, &d));
}

/* 7. timelines */
typedef struct { uint32_t act; int tail; } tl_t;
static void test_timelines(void)
{
    /* a lever pull: the owner 2 -> INT_MIN (layers 0 / 1 = 0xF00 / 0xF01 of the lever's list) -> 0 -> 2; the receiver's puppet: hold 0, both layers, exit, the action mirror switches to 2 */
    pm_tx_t t;
    pm_rx_t r;
    pa_guard_t g;
    pm_tx_reset(&t), pm_rx_reset(&r), pa_guard_reset(&g);
    static const uint32_t seq[] = {2, 2, PM_INT_MIN, PM_INT_MIN, PM_INT_MIN, PM_INT_MIN, 0, 0, 2, 2, 3};
    uint32_t pup_act = 2, now = 1000;
    int entered = 0, exited = 0, holds = 0, set0 = 0, set1 = 0, switched2 = 0, pa_during = 0;
    for (unsigned i = 0; i < sizeof seq / sizeof *seq; i++, now += 50) {
        int tail = pm_tx_want(&t, g_bits, seq[i], 0, seq[i] == PM_INT_MIN, 1);
        pm_rx_in in;
        memset(&in, 0, sizeof in);
        in.pup = 0x5000, in.own_act = seq[i], in.tail = tail, in.enabled = 1, in.now = now;
        int d = pm_rx_decide(&r, g_bits, &in);
        if (d == PM_RX_ENTER) entered++;
        if (d == PM_RX_ENTER || d == PM_RX_RUN) {
            if (pm_rx_hold(&r, pup_act, now)) holds++, pup_act = 0;
            uint8_t why;
            pm_layer_in l0 = lin(0xF00, (uint16_t)(set0 ? 0xF00 : 0x005), now), l1 = lin(0xF01, (uint16_t)(set1 ? 0xF01 : 0x006), now);
            if (pm_layer_plan(&r, 0, &l0, &why) == PM_L_SET) set0++;
            if (pm_layer_plan(&r, 1, &l1, &why) == PM_L_SET) set1++;
            if (pa_decide(&g, g_bits, seq[i], pup_act, now) != 0) pa_during++;   /* (not called by the game while the mirror runs; INT_MIN / 0 are not in the mask anyway) */
            continue;
        }
        if (d == PM_RX_EXIT) exited++, pa_guard_reset(&g);
        if (pa_decide(&g, g_bits, seq[i], pup_act, now) == 1) {
            if (seq[i] == 2 && pup_act == 0) switched2++;
            pup_act = seq[i];
        }
    }
    CHECK(entered == 1 && exited == 1 && holds == 1 && set0 == 1 && set1 == 1 && switched2 == 1 && pup_act == 3 && pa_during == 0);
    /* a pickup while walking: 3 -> 127 -> 3: mirrored, handed back */
    pm_tx_reset(&t), pm_rx_reset(&r), pa_guard_reset(&g);
    static const uint32_t seq2[] = {3, 3, 127, 127, 127, 3, 3};
    pup_act = 3, entered = exited = 0;
    int back3 = 0;
    for (unsigned i = 0; i < sizeof seq2 / sizeof *seq2; i++, now += 50) {
        int tail = pm_tx_want(&t, g_bits, seq2[i], 0, 0, 0);
        pm_rx_in in;
        memset(&in, 0, sizeof in);
        in.pup = 0x5000, in.own_act = seq2[i], in.tail = tail, in.enabled = 1, in.now = now;
        int d = pm_rx_decide(&r, g_bits, &in);
        if (d == PM_RX_ENTER) entered++;
        if (d == PM_RX_ENTER || d == PM_RX_RUN) {
            if (pm_rx_hold(&r, pup_act, now)) pup_act = 0;
            continue;
        }
        if (d == PM_RX_EXIT) exited++, pa_guard_reset(&g);
        if (pa_decide(&g, g_bits, seq2[i], pup_act, now) == 1) {
            if (seq2[i] == 3 && pup_act == 0) back3++;
            pup_act = seq2[i];
        }
    }
    CHECK(entered == 1 && exited == 1 && back3 == 1 && pup_act == 3);
    /* a combat burst (knife, dodge, aim, shoot, reload, hit, crouch): never a tail, never the mirror; the action mirror as before */
    pm_tx_reset(&t), pm_rx_reset(&r), pa_guard_reset(&g);
    static const uint32_t seq3[] = {2, 28, 28, 2, 8, 2, 13, 17, 22, 21, 32, 158, 159, 86, 4, 87, 2};
    int tails = 0, modes = 0;
    for (unsigned i = 0; i < sizeof seq3 / sizeof *seq3; i++, now += 50) {
        int tail = pm_tx_want(&t, g_bits, seq3[i], 0, 0, 0);
        tails += tail;
        pm_rx_in in;
        memset(&in, 0, sizeof in);
        in.pup = 0x5000, in.own_act = seq3[i], in.tail = 1, in.enabled = 1, in.now = now;   /* even with a tail (a forged one) */
        if (pm_rx_decide(&r, g_bits, &in) != PM_RX_OFF) modes++;
    }
    CHECK(tails == 0 && modes == 0);
}

int main(void)
{
    pa_anim_defaults(g_bits);
    test_wire();
    test_sender();
    test_receiver();
    test_layer();
    test_bank();
    test_bank_layers();
    test_handle();
    test_timelines();
    printf("test_pmot: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
