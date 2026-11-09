/* test_combat4.c: offline checks of native/invmirror_logic.h (notes\combat4.md).
 *   1. the combat3 run b timeline: client Moira picks a bottle up, throws it, picks the second one up, throws it: the host's copy ends without 0147
 *      (7f75163: x2, only PICKUP / GRANT crossed);
 *   2. rounds and a reload (the owner's gun and reserve; the puppet's replayed fire and reload change the host's copy too): the copy ends equal to the owner;
 *   3. a herb used twice: the stack is removed from the copy;
 *   4. the stock hand-over (client Moira's inventory GUI gives a bottle to the host's Claire): the item ends in the host's real Claire, both copies agree, the total
 *      number of bottles is kept (7f75163: the bottle vanished: Moira lost it, the host's Claire never got it);
 *   5. a checkpoint save and a game-over retry: the host's checkpoint holds the client's real items, after the retry neither a duplicate nor a loss;
 *   6. this PC's own story grant into the partner's copy (a key item, the flashlight) is not taken away before the owner's table shows it; weapons are never removed;
 *   7. the INV events: a full set in chunks, changes before a full set dropped, out-of-range entries refused;
 *   8. the puppet's hand: cleared when the owner's hand is empty, the light when the owner holds the light (7f75163: the bottle stayed in the hand);
 *   9. the partner's throw: the event round trip, who spawns it, and its damage on the host stays MHIT only (P2 drops the copy's own hit);
 *  10. a blow whose DI attacker is an object the human holds (a weapon's own collision: Moira's flash GUESS): the client sends it, the host drops the puppet's copy;
 *  11. Moira's flash through MHIT: a damage-0 DI with the light bit (DI+0x28 bit 26) on a replica becomes an MHIT, and the host's rebuilt DI puts the bit into the hit
 *      record the way FUN_00AD3ED0 does (OR of the 96 flag bits), with a player as the attacker: uEm1100Intel_vf62's DmgFlash test (vf334(0x1A)) sees it;
 *  12. the battery of Moira's light (0155 count, changing every 100 ms while she shines): battery-only changes go out at most once a second, every other change at once
 *      (built with -DIM_MODEL_BEFORE_THROTTLE, rev2-int 3e55c72, this check fails);
 *  13. flashlive: the real inventory screen's pass between two frames (outside the vf5 bracket) reaches the owner through the per-frame watcher (built with
 *      -DIM_MODEL_BEFORE_FL, rev2-int 6b40a90, the bottle vanishes).
 * Built a second time with -DIM_MODEL_BEFORE_C4 -DEQ_MODEL_BEFORE_C4 -DTH_MODEL_BEFORE_C4 (rev2-int 7f75163): that model must fail (tests/build.sh); a third time with
 * -DIM_MODEL_BEFORE_THROTTLE (rev2-int 3e55c72): the battery check must fail. Check 11 found no defect in the MHIT path for a damage-0 flash DI (it passes on every model
 * when the attacker is Moira herself; with the light object as the attacker only the held-object rule of combat4 takes it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/grapple_logic.h"
#include "../native/meleehit_logic.h"
#include "../native/invmirror_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 60) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define STACK 0   /* the model's stack limit (0 = none) */

/* ---- one PC of the pair: its own human's inventory, its copy of the partner, the owner's table, the grace list ---- */
typedef struct {
    int slot;           /* this PC's human */
    im_inv mine;        /* authoritative */
    im_inv copy;        /* the partner's slot as this PC holds it */
    im_inv sent;        /* the owner side: what was sent last */
    im_table tab;       /* the partner's table */
    im_grace grace;
    uint8_t seq;
} pc_t;

/* the reliable ordered channel of each direction */
typedef struct { uint8_t b[IM_EV_MAX]; unsigned len; int kind; } msg_t;   /* kind 0 INV, 1 ADJ, 2 REQ */
typedef struct { msg_t m[256]; int r, w; } chan_t;
static void ch_put(chan_t *c, int kind, const uint8_t *b, unsigned len) { msg_t *m = &c->m[c->w++ % 256]; memcpy(m->b, b, len), m->len = len, m->kind = kind; }

static void owner_send(pc_t *p, chan_t *out, int full)
{
    uint8_t idx[IM_N], buf[IM_EV_MAX];
    int n = full ? im_full_list(&p->mine, idx) : im_changed(&p->sent, &p->mine, idx);
    if (!n && !full) return;
    for (int s = 0; s < n || (full && s == 0); s += IM_EV_ENTS) {
        unsigned flags = 0, cnt = (unsigned)(n - s) > IM_EV_ENTS ? IM_EV_ENTS : (unsigned)(n - s);
        if (full && s == 0) flags |= IM_F_BEGIN;
        if (full && s + (int)IM_EV_ENTS >= n) flags |= IM_F_END;
        unsigned len = im_encode(buf, p->slot, flags, p->seq++, &p->mine, idx + s, cnt);
        ch_put(out, 0, buf, len);
    }
    p->sent = p->mine;
}
static void owner_apply_adj(pc_t *p, const im_op *a, int n)   /* story.inc: PutItem for n > 0, FUN_00995170 / FUN_007151A0 for n < 0 */
{
    for (int i = 0; i < n; i++) {
        if (a[i].n > 0) im_model_put(&p->mine, a[i].id, a[i].n, a[i].p18, STACK);
        else {
            int32_t d = -a[i].n;
            for (int j = (int)IM_N - 1; j >= 0 && d > 0; j--) {
                im_ent *e = &p->mine.e[j];
                if (e->id != a[i].id) continue;
                int32_t take = e->count < d ? e->count : d;
                e->count -= take, d -= take;
                if (e->count < 1) memset(e, 0, sizeof *e);
            }
        }
    }
}
static void pc_receive(pc_t *p, chan_t *in, chan_t *out)
{
    while (in->r < in->w) {
        msg_t *m = &in->m[in->r++ % 256];
        if (m->kind == 0) im_table_apply(&p->tab, m->b, m->len);
        else if (m->kind == 2) owner_send(p, out, 1);
        else {
            im_op a[9];
            int slot = -1, n = im_adj_decode(m->b, m->len, &slot, a, 9);
            if (n > 0 && slot == p->slot) owner_apply_adj(p, a, n);
        }
    }
}
static void pc_reconcile(pc_t *p, uint32_t now)
{
    if (!p->tab.have) return;
    im_op ops[64];
    int n = im_plan(&p->tab.inv, &p->copy, &p->grace, now, ops, 64);
    im_model_apply(&p->copy, ops, n, STACK);
}
/* one 100 ms step of both PCs: owner changes go out, events arrive, both reconcile */
static chan_t g_h2c, g_c2h;
static void step(pc_t *h, pc_t *c, uint32_t now)
{
    owner_send(h, &g_h2c, 0), owner_send(c, &g_c2h, 0);
    pc_receive(h, &g_c2h, &g_h2c), pc_receive(c, &g_h2c, &g_c2h);
    pc_reconcile(h, now), pc_reconcile(c, now);
}
static void pair_start(pc_t *h, pc_t *c)
{
    memset(&g_h2c, 0, sizeof g_h2c), memset(&g_c2h, 0, sizeof g_c2h);
    h->slot = 0, c->slot = 1;
    im_table_reset(&h->tab), im_table_reset(&c->tab);
    memset(&h->grace, 0, sizeof h->grace), memset(&c->grace, 0, sizeof c->grace);
    memset(&h->sent, 0, sizeof h->sent), memset(&c->sent, 0, sizeof c->sent);
    uint8_t req[1] = {0};
    ch_put(&g_h2c, 2, req, 1), ch_put(&g_c2h, 2, req, 1);   /* each PC asks the owner for a full set (the level start) */
}
static int inv_eq(const im_inv *a, const im_inv *b)
{
    for (unsigned i = 0; i < 2 * IM_N; i++) {
        uint16_t id = i < IM_N ? a->e[i].id : b->e[i - IM_N].id;
        if (id && im_total(a, id) != im_total(b, id)) return 0;
        if (id && im_has(a, id) != im_has(b, id)) return 0;
    }
    return 1;
}
static int count_of(const im_inv *v, uint16_t id) { return im_total(v, id); }
static void put(im_inv *v, unsigned idx, uint16_t id, int32_t n) { v->e[idx].id = id, v->e[idx].count = n, v->e[idx].p18 = 0, v->e[idx].max = 0; }
static void use1(im_inv *v, uint16_t id)   /* the owner consumes one of id (a throw's end, a herb, a round) */
{
    for (int i = (int)IM_N - 1; i >= 0; i--)
        if (v->e[i].id == id) {
            if (--v->e[i].count < 1 && !(im_is_weapon(id) && !im_is_throwable(id))) memset(&v->e[i], 0, sizeof v->e[i]);
            return;
        }
}
static int find(const im_inv *v, uint16_t id)
{
    for (unsigned i = 0; i < IM_N; i++)
        if (v->e[i].id == id) return (int)i;
    return -1;
}

/* 1. the combat3 run b timeline (13:06 / 13:08): the host's copy of Moira (slot 1) */
static void test_bottles(void)
{
    pc_t h, c;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c);
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    /* pickup of (101,0,6): the client's AddItem, PICKUP -> the host's AddItem on its copy of the object (+1 in the copy, a local add: grace) */
    im_model_put(&c.mine, 0x147, 1, 0, STACK);
    im_model_put(&h.copy, 0x147, 1, 0, STACK), im_grace_note(&h.grace, 0x147, t);
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x147) == 1);
    /* the throw: the owner consumes it at the action end (FUN_007151A0); the host's puppet never reaches the release (no consumption on the host) */
    t += 20000;
    use1(&c.mine, 0x147);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    CHECK(count_of(&c.mine, 0x147) == 0);
    CHECK(count_of(&h.copy, 0x147) == 0);
    /* the second bottle (101,0,5) at 13:08:27, thrown at the jailed em1100 */
    t += 100000;
    im_model_put(&c.mine, 0x147, 1, 0, STACK);
    im_model_put(&h.copy, 0x147, 1, 0, STACK), im_grace_note(&h.grace, 0x147, t);
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x147) == 1);
    t += 15000;
    use1(&c.mine, 0x147);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x147) == 0);   /* 7f75163: x2 */
    CHECK(inv_eq(&h.copy, &c.mine));
    /* the quick case: thrown 1 s after the pickup (inside the grace): the copy still ends right */
    im_model_put(&c.mine, 0x147, 1, 0, STACK);
    im_model_put(&h.copy, 0x147, 1, 0, STACK), im_grace_note(&h.grace, 0x147, t);
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    use1(&c.mine, 0x147);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x147) == 0);
}

/* 2. rounds and a reload: Claire as the client this time (slot 1 in this model), handgun 0105 with 9 loaded, ammo 0201 x18 */
static void test_ammo(void)
{
    pc_t h, c;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c);
    put(&c.mine, 0, 0x201, 18), put(&c.mine, 20, 0x105, 9);
    put(&h.copy, 0, 0x201, 18), put(&h.copy, 20, 0x105, 9);   /* the copy from the save */
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    for (int s = 0; s < 3; s++) {   /* three shots: the owner's rounds; the puppet's replayed shot takes one of the copy's too, a little later */
        c.mine.e[20].count--;
        step(&h, &c, t += 100);
        h.copy.e[20].count--;
        step(&h, &c, t += 100);
    }
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    CHECK(h.copy.e[20].id == 0x105 && h.copy.e[20].count == 6);
    /* the reload: 3 from the reserve; the puppet's replayed reload does it again on the host */
    c.mine.e[0].count -= 3, c.mine.e[20].count += 3;
    step(&h, &c, t += 100);
    h.copy.e[0].count -= 3, h.copy.e[20].count += 3;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x201) == 15);
    CHECK(count_of(&h.copy, 0x105) == 9);
    /* the magazine fired empty: the gun keeps its entry with 0 rounds on both */
    c.mine.e[20].count = 0;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    CHECK(find(&h.copy, 0x105) == 20 && h.copy.e[20].count == 0);
    CHECK(inv_eq(&h.copy, &c.mine));
}

/* 3. a herb used twice */
static void test_herb(void)
{
    pc_t h, c;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c);
    put(&c.mine, 3, 0x301, 2), put(&h.copy, 3, 0x301, 2);
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    use1(&c.mine, 0x301);
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x301) == 1);
    use1(&c.mine, 0x301);
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    CHECK(find(&h.copy, 0x301) < 0);
    CHECK(inv_eq(&h.copy, &c.mine));
    /* an empty stack is never kept: a herb picked up again lands as a new stack */
    im_model_put(&c.mine, 0x301, 1, 0, STACK);
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x301) == 1);
}

/* 4. the hand-over in the client's GUI: Moira (client, slot 1) gives a bottle to Claire (host, slot 0) */
static void test_handover(void)
{
    pc_t h, c;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c);
    put(&c.mine, 1, 0x147, 1), put(&h.copy, 1, 0x147, 1);   /* Moira holds the bottle */
    put(&h.mine, 0, 0x201, 10), put(&c.copy, 0, 0x201, 10); /* Claire's ammo */
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    /* the client's GUI update: FUN_009938F0 into the partner's personal inventory + FUN_00993BC0 from Moira's */
    im_inv before = c.copy;
    im_model_put(&c.copy, 0x147, 1, 0, STACK);
    memset(&c.mine.e[1], 0, sizeof c.mine.e[1]);
    im_op adj[16];
    int n = im_gui_delta(&before, &c.copy, adj, 16);
#ifndef IM_MODEL_BEFORE_C4
    CHECK(n == 1 && adj[0].id == 0x147 && adj[0].n == 1);
#endif
    if (n > 0) {
        uint8_t b[IM_EV_MAX];
        unsigned len = im_adj_encode(b, h.slot, adj, (unsigned)n);
        ch_put(&g_c2h, 1, b, len);
        for (int i = 0; i < n; i++) im_grace_note(&c.grace, adj[i].id, t);
    }
    for (int i = 0; i < 50; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.mine, 0x147) == 1);    /* the host's real Claire got it (7f75163: never) */
    CHECK(count_of(&c.copy, 0x147) == 1);    /* the client's copy of Claire keeps it */
    CHECK(count_of(&h.copy, 0x147) == 0);    /* the host's copy of Moira lost it */
    CHECK(count_of(&c.mine, 0x147) + count_of(&h.mine, 0x147) == 1);   /* one bottle in the game */
    CHECK(inv_eq(&h.copy, &c.mine) && inv_eq(&c.copy, &h.mine));
    /* a pull: Claire's (host) GUI takes Claire's own ammo? no: the host's GUI takes 4 of Moira's rounds into Claire's bag */
    put(&c.mine, 2, 0x201, 6);
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x201) == 6);
    before = h.copy;
    h.copy.e[find(&h.copy, 0x201)].count -= 4;
    im_model_put(&h.mine, 0x201, 4, 0, STACK);
    n = im_gui_delta(&before, &h.copy, adj, 16);
    if (n > 0) {
        uint8_t b[IM_EV_MAX];
        ch_put(&g_h2c, 1, b, im_adj_encode(b, c.slot, adj, (unsigned)n));
        for (int i = 0; i < n; i++) im_grace_note(&h.grace, adj[i].id, t);
    }
    for (int i = 0; i < 50; i++) step(&h, &c, t += 100);
    CHECK(count_of(&c.mine, 0x201) == 2);    /* the client's Moira lost the 4 rounds (7f75163: kept them, the host copy got them back: a duplicate) */
    CHECK(count_of(&h.mine, 0x201) == 14);
    CHECK(count_of(&h.mine, 0x201) + count_of(&c.mine, 0x201) == 16);
    CHECK(inv_eq(&h.copy, &c.mine) && inv_eq(&c.copy, &h.mine));
}

/* 13. flashlive (CH1, pair run flashlive-a 22:50): the REAL inventory screen's pass runs outside the vf5 bracket: host Claire passes one of her two bottles to Moira;
 * between two frames her count drops and the host's copy of Moira gains one, no hook sees it. Without the per-frame watcher the host's mirror took the copy's bottle
 * back 130 ms later (the owner, Moira, never had it): one bottle gone. The watcher (story.inc im_pw_tick: im_pass_delta before the reconcile, the copy re-read after
 * it) sends the ADJ; the owner gets it; nothing is lost or doubled; the mirror's own operations and a plain throw are never taken for a pass. */
typedef struct { im_inv mine, part; int have; int found; } pw_t;
static void pw_tick(pc_t *p, pw_t *w, chan_t *out, uint32_t now)
{
    if (w->have) {
        im_op adj[9];
        int n = im_pass_delta(&w->mine, &p->mine, &w->part, &p->copy, adj, 9);
        if (n > 0) {
            uint8_t b[IM_EV_MAX];
            ch_put(out, 1, b, im_adj_encode(b, 1 - p->slot, adj, (unsigned)n));
            for (int i = 0; i < n; i++) im_grace_note(&p->grace, adj[i].id, now), w->found += adj[i].n;
        }
    }
    w->mine = p->mine, w->part = p->copy, w->have = 1;
}
static void step_pw(pc_t *h, pc_t *c, pw_t *wh, pw_t *wc, uint32_t now)   /* story_inv_tick's order: owner send, events, the watcher, the reconcile, the copy re-read */
{
    owner_send(h, &g_h2c, 0), owner_send(c, &g_c2h, 0);
    pc_receive(h, &g_c2h, &g_h2c), pc_receive(c, &g_h2c, &g_c2h);
    pw_tick(h, wh, &g_h2c, now), pw_tick(c, wc, &g_c2h, now);
    pc_reconcile(h, now), pc_reconcile(c, now);
    wh->part = h->copy, wc->part = c->copy;
}
static void test_pass_watch(void)
{
    pc_t h, c;
    pw_t wh, wc;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c), memset(&wh, 0, sizeof wh), memset(&wc, 0, sizeof wc);
    put(&h.mine, 0, 0x147, 2), put(&c.copy, 0, 0x147, 2);   /* Claire (host) holds two bottles */
    put(&c.mine, 24, 0x155, 99), put(&h.copy, 24, 0x155, 99);   /* Moira's light */
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 6; i++) step_pw(&h, &c, &wh, &wc, t += 100);
    CHECK(count_of(&h.copy, 0x147) == 0 && count_of(&c.copy, 0x147) == 2);
    /* the screen's pass (FUN_009934B0 into Moira's personal inventory + Claire's count - 1), between two frames */
    im_model_put(&h.copy, 0x147, 1, 0, STACK);
    h.mine.e[0].count = 1;
    for (int i = 0; i < 40; i++) step_pw(&h, &c, &wh, &wc, t += 100);
    CHECK(count_of(&c.mine, 0x147) == 1);   /* Moira got it (6b40a90: 0, the bottle vanished) */
    CHECK(count_of(&h.mine, 0x147) == 1);
    CHECK(count_of(&h.mine, 0x147) + count_of(&c.mine, 0x147) == 2);
    CHECK(inv_eq(&h.copy, &c.mine) && inv_eq(&c.copy, &h.mine));
    CHECK(wh.found == 1 && wc.found == 0);
    /* Moira throws it (her own use) and Claire picks a bottle up at the same moment: no pass on either PC */
    use1(&c.mine, 0x147);
    put(&h.mine, 3, 0x147, 1);
    for (int i = 0; i < 40; i++) step_pw(&h, &c, &wh, &wc, t += 100);
    CHECK(count_of(&c.mine, 0x147) == 0 && count_of(&h.mine, 0x147) == 2);
    CHECK(inv_eq(&h.copy, &c.mine) && inv_eq(&c.copy, &h.mine));
    CHECK(wh.found == 1 && wc.found == 0);
    /* a pull on the client: Moira's screen takes one of Claire's bottles (the copy of Claire -1, Moira +1) */
    c.copy.e[find(&c.copy, 0x147)].count -= 1;
    if (count_of(&c.copy, 0x147) == 1 && find(&c.copy, 0x147) >= 0 && c.copy.e[find(&c.copy, 0x147)].count < 1) memset(&c.copy.e[find(&c.copy, 0x147)], 0, sizeof c.copy.e[0]);
    im_model_put(&c.mine, 0x147, 1, 0, STACK);
    for (int i = 0; i < 40; i++) step_pw(&h, &c, &wh, &wc, t += 100);
    CHECK(count_of(&h.mine, 0x147) == 1 && count_of(&c.mine, 0x147) == 1);
    CHECK(inv_eq(&h.copy, &c.mine) && inv_eq(&c.copy, &h.mine));
    CHECK(wc.found == -1);
}

/* 5. a checkpoint and a game-over retry (each PC writes its own save with both slots, the retry restores each PC's own) */
static void test_checkpoint(void)
{
    pc_t h, c;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c);
    put(&c.mine, 1, 0x147, 1), put(&h.copy, 1, 0x147, 1);
    put(&c.mine, 2, 0x301, 1), put(&h.copy, 2, 0x301, 1);
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    /* the bottle thrown, a second bottle picked up and thrown (the bug timeline), then the checkpoint */
    use1(&c.mine, 0x147);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    im_model_put(&c.mine, 0x147, 1, 0, STACK);
    im_model_put(&h.copy, 0x147, 1, 0, STACK), im_grace_note(&h.grace, 0x147, t);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    use1(&c.mine, 0x147);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    im_inv save_h_partner = h.copy, save_c_mine = c.mine;   /* both PCs write their checkpoint */
    CHECK(inv_eq(&save_h_partner, &save_c_mine));          /* the host's save holds Moira's real items (7f75163: a bottle more) */
    /* after the checkpoint: a herb used, ammo picked up */
    use1(&c.mine, 0x301);
    im_model_put(&c.mine, 0x201, 5, 0, STACK);
    im_model_put(&h.copy, 0x201, 5, 0, STACK), im_grace_note(&h.grace, 0x201, t);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    /* game over -> retry: both restore their own save, the level reset clears the tables, each PC asks for a full set */
    c.mine = save_c_mine, h.copy = save_h_partner;
    pair_start(&h, &c);
    for (int i = 0; i < 40; i++) step(&h, &c, t += 100);
    CHECK(inv_eq(&h.copy, &c.mine));
    CHECK(count_of(&h.copy, 0x147) == 0 && count_of(&c.mine, 0x147) == 0);   /* no duplicate */
    CHECK(count_of(&h.copy, 0x301) == 1 && count_of(&c.mine, 0x301) == 1);   /* no loss: the herb is back as at the checkpoint */
    CHECK(count_of(&h.copy, 0x201) == 0);
}

/* 6. this PC's own grant into the partner's copy; weapons */
static void test_grants(void)
{
    pc_t h, c;
    memset(&h, 0, sizeof h), memset(&c, 0, sizeof c);
    pair_start(&h, &c);
    uint32_t t = 1000;
    for (int i = 0; i < 5; i++) step(&h, &c, t += 100);
    /* the host's FSM grants Moira the flashlight 0155 and a key item 0525; the client's FSM does it 1.5 s later */
    im_model_put(&h.copy, 0x155, 99, 0, STACK), im_grace_note(&h.grace, 0x155, t);
    im_model_put(&h.copy, 0x525, 1, 0, STACK), im_grace_note(&h.grace, 0x525, t);
    for (int i = 0; i < 15; i++) {
        step(&h, &c, t += 100);
        CHECK(count_of(&h.copy, 0x525) == 1 && count_of(&h.copy, 0x155) == 99);
    }
    im_model_put(&c.mine, 0x155, 99, 0, STACK), im_model_put(&c.mine, 0x525, 1, 0, STACK);
    for (int i = 0; i < 50; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x525) == 1 && count_of(&h.copy, 0x155) == 99);
    /* the key is used on a door by the owner: gone from the copy */
    memset(&c.mine.e[find(&c.mine, 0x525)], 0, sizeof(im_ent));
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(find(&h.copy, 0x525) < 0);
    /* a weapon the owner does not have (a lost grant on the client): never removed from the copy, the plan says KEEPW */
    im_model_put(&h.copy, 0x14D, 1, 0, STACK);
    t += 10000;
    im_op ops[16];
    int n = im_plan(&h.tab.inv, &h.copy, &h.grace, t, ops, 16);
    int keepw = 0, del = 0;
    for (int i = 0; i < n; i++) keepw += ops[i].op == IM_OP_KEEPW && ops[i].id == 0x14D, del += (ops[i].op == IM_OP_DEL || ops[i].op == IM_OP_USE1) && ops[i].id == 0x14D;
#ifndef IM_MODEL_BEFORE_C4
    CHECK(keepw == 1);
#else
    (void)keepw;
#endif
    CHECK(del == 0);
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x14D) == 1);
    /* the flashlight's count (no count id? 0155 has one) follows: 99 -> 98 */
    c.mine.e[find(&c.mine, 0x155)].count = 98;
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(count_of(&h.copy, 0x155) == 98);
    /* an id without a count (0x102): presence only */
    im_model_put(&c.mine, 0x102, -1, 0, STACK);
    for (int i = 0; i < 10; i++) step(&h, &c, t += 100);
    CHECK(im_has(&h.copy, 0x102));
}

/* 7. the events */
static void test_events(void)
{
    im_inv own;
    memset(&own, 0, sizeof own);
    for (unsigned i = 0; i < 20; i++) own.e[i].id = (uint16_t)(0x300 + i), own.e[i].count = (int32_t)i + 1, own.e[i].p18 = i * 7;
    own.e[33].id = 0x601, own.e[33].count = 1, own.e[33].p18 = 0xABCDEF01u;
    uint8_t idx[IM_N], b[IM_EV_MAX];
    int n = im_full_list(&own, idx);
    CHECK(n == 21);
    im_table t;
    im_table_reset(&t);
    /* a change before any full set is dropped */
    uint8_t one[1] = {3};
    unsigned len = im_encode(b, 1, 0, 0, &own, one, 1);
    CHECK(len == 4 + IM_ENT_BYTES && len <= IM_EV_MAX);
    CHECK(im_table_apply(&t, b, len) == 0 && !t.have && t.n_dropped == 1);
    int chunks = 0;
    for (int s = 0; s < n; s += IM_EV_ENTS) {
        unsigned flags = (s == 0 ? IM_F_BEGIN : 0) | (s + (int)IM_EV_ENTS >= n ? IM_F_END : 0), cnt = (unsigned)(n - s) > IM_EV_ENTS ? IM_EV_ENTS : (unsigned)(n - s);
        len = im_encode(b, 1, flags, (uint8_t)s, &own, idx + s, cnt);
        CHECK(len <= IM_EV_MAX);
        CHECK(im_table_apply(&t, b, len) == 1);
        CHECK(t.have == (s + (int)IM_EV_ENTS >= n));
        chunks++;
    }
    CHECK(chunks == 3);
    for (unsigned i = 0; i < IM_N; i++) CHECK(t.inv.e[i].id == own.e[i].id && t.inv.e[i].count == own.e[i].count && t.inv.e[i].p18 == own.e[i].p18);
    /* a removal goes out as an empty entry */
    im_inv cur = own;
    memset(&cur.e[5], 0, sizeof cur.e[5]);
    cur.e[6].count = 77;
    n = im_changed(&own, &cur, idx);
    CHECK(n == 2 && idx[0] == 5 && idx[1] == 6);
    len = im_encode(b, 1, 0, 9, &cur, idx, (unsigned)n);
    CHECK(im_table_apply(&t, b, len) == 1);
    CHECK(t.inv.e[5].id == 0 && t.inv.e[6].count == 77);
    /* a new full set clears what the old one had */
    im_inv small;
    memset(&small, 0, sizeof small);
    small.e[0].id = 0x147, small.e[0].count = 1;
    n = im_full_list(&small, idx);
    len = im_encode(b, 1, IM_F_BEGIN | IM_F_END, 10, &small, idx, (unsigned)n);
    CHECK(im_table_apply(&t, b, len) == 1 && t.have);
    CHECK(t.inv.e[1].id == 0 && t.inv.e[0].id == 0x147);
    /* an empty owner's full set: BEGIN|END with no entries */
    memset(&small, 0, sizeof small);
    len = im_encode(b, 1, IM_F_BEGIN | IM_F_END, 11, &small, idx, 0);
    CHECK(im_table_apply(&t, b, len) == 1 && t.have && t.inv.e[0].id == 0);
    /* garbage */
    b[3] = 9;
    CHECK(im_table_apply(&t, b, 4 + IM_ENT_BYTES * 9) == 0);
    b[3] = 1, b[4] = 200;
    unsigned bad0 = t.n_bad;
    im_table_apply(&t, b, 4 + IM_ENT_BYTES);
    CHECK(t.n_bad == bad0 + 1);
    /* ADJ round trip */
    im_op a[3] = {{IM_OP_ADD, 0, 0x147, 1, 0}, {IM_OP_ADD, 0, 0x201, -4, 0}, {IM_OP_ADD, 0, 0x601, 1, 0x12345678u}}, d[9];
    int slot = -1;
    len = im_adj_encode(b, 0, a, 3);
    CHECK(len <= IM_EV_MAX);
    CHECK(im_adj_decode(b, len, &slot, d, 9) == 3 && slot == 0 && d[1].id == 0x201 && d[1].n == -4 && d[2].p18 == 0x12345678u);
    CHECK(im_adj_decode(b, 5, &slot, d, 9) == -1);
}

/* 8. the puppet's hand */
static void test_hand(void)
{
    const uint16_t moira[3] = {0x155, 0, 0x147}, claire[3] = {0x105, 0x14A, 0};
    CHECK(eq_hand_plan(3, moira, 0) == EQ_HAND_SLOT);
    CHECK(eq_hand_plan(3, moira, 3) == EQ_HAND_KEEP);
    CHECK(eq_hand_plan(0, moira, 3) == EQ_HAND_DEFAULT);   /* after the throw: the owner's hand is empty, the puppet still holds the bottle (7f75163: kept) */
    CHECK(eq_hand_plan(0, claire, 1) == EQ_HAND_DEFAULT);
    CHECK(eq_hand_plan(0, claire, 0) == EQ_HAND_KEEP);
    CHECK(eq_hand_plan(0, claire, 4) == EQ_HAND_KEEP);
    CHECK(eq_hand_plan(4, moira, 0) == EQ_HAND_LIGHT);     /* Moira's light in the owner's hand */
    CHECK(eq_hand_plan(4, moira, 3) == EQ_HAND_LIGHT);
    CHECK(eq_hand_plan(4, moira, 4) == EQ_HAND_KEEP);
    CHECK(eq_hand_plan(2, claire, 1) == EQ_HAND_SLOT);
    CHECK(eq_hand_plan(2, moira, 0) == EQ_HAND_KEEP);      /* no item in that slot */
}

/* 9. the partner's throw */
static void test_throw(void)
{
    float v[12], w[12];
    for (int i = 0; i < 12; i++) v[i] = (float)(i * 13 - 50) + 0.25f;
    uint8_t b[IM_EV_MAX];
    unsigned len = th_encode(b, 1, 0x147, v);
    CHECK(len == TH_LEN && len <= IM_EV_MAX);
    int slot = -1;
    uint16_t id = 0;
    CHECK(th_decode(b, len, &slot, &id, w) && slot == 1 && id == 0x147 && !memcmp(v, w, sizeof v));
    CHECK(!th_decode(b, len - 1, &slot, &id, w));
    float nan = 0.0f / 0.0f;
    memcpy(b + 4, &nan, 4);
    CHECK(!th_decode(b, len, &slot, &id, w));
    /* who spawns: the host (my slot 0, partner 1) spawns the client Moira's bottle; never its own human's, never the special branch */
    CHECK(th_accept(1, 0, 1, 0x147) == 1);   /* 7f75163: the host never showed it */
    CHECK(th_accept(0, 1, 0, 0x147) == 1);   /* the other role order */
    CHECK(th_accept(0, 0, 1, 0x147) == 0);
    CHECK(th_accept(1, 0, 1, 0x702) == 0);
    CHECK(th_accept(1, 0, 1, 0x301) == 0);
    CHECK(th_accept(1, 0, 1, 0) == 0);
    CHECK(th_accept(5, 0, 1, 0x147) == 0);
    /* the damage: the host's copy of the projectile has the puppet as owner (+0x19C0): P2 drops its hit on a host enemy, the client's real hit comes as MHIT
     * (the client's mh_take uses the same one-level owner rule): one damage per client hit */
    const uint32_t puppet = 0x2ABEC470u, proj_host = 0x2B0F0060u, host_human = 0x2A0C8060u;
    int host_damage = 0;
    for (int hit = 0; hit < 3; hit++) {
        if (!gp_puppet_blow_drop(1, proj_host, puppet, puppet, 0)) host_damage++;   /* the copy's own explosion on the host's enemy */
        host_damage++;                                                                /* the MHIT of the client's real explosion */
    }
    CHECK(host_damage == 3);
    CHECK(gp_puppet_blow_drop(1, proj_host, host_human, puppet, 0) == 0);   /* the host's own human's bottle counts as usual */
}

/* 10. a blow of an object the human holds */
static int held_rule(uint32_t att, uint32_t owner19c0, uint32_t human, const uint32_t held[HW_N])
{
#ifdef IM_MODEL_BEFORE_C4
    (void)held;
    return att == human || (att && owner19c0 == human);   /* 7f75163: one level through +0x19C0 only */
#else
    return att == human || (att && owner19c0 == human) || hw_held(att, held);
#endif
}
static void test_held(void)
{
    const uint32_t moira_c = 0x2A230060u, light_c = 0x2A238060u, puppet_h = 0x2A1B8060u, light_h = 0x2A1C0060u, enemy = 0x2A610060u;
    uint32_t held_c[HW_N] = {light_c, 0, 0, 0, light_c, 0, 0x2A240000u, 0, 0, 0, 0}, held_h[HW_N] = {light_h, 0, 0, 0, light_h, 0, 0, 0, 0, 0, 0};
    /* the client: Moira's flash hits a replica with the light object as attacker (no +0x19C0 owner): taken as her blow, so MHIT carries it to the host */
    mh_take_in_t in;
    memset(&in, 0, sizeof in);
    in.valid = 1, in.attacker = light_c, in.attacker_owner = 0, in.mine = moira_c, in.fl0 = 1u << 26, in.replica_hp = 400;
    int t = mh_take(&in);
    if (t == MH_TAKE_OTHER && held_rule(in.attacker, 0, moira_c, held_c)) in.attacker_owner = moira_c, t = mh_take(&in);
    CHECK(t == MH_TAKE_MINE);
    /* the host: the puppet's own light hit on the host's enemy is dropped (the real one comes as MHIT): one stun source */
    int host_sources = 0;
    uint32_t own = held_rule(light_h, 0, puppet_h, held_h) ? puppet_h : 0;
    if (!gp_puppet_blow_drop(1, light_h, own, puppet_h, 0)) host_sources++;
    host_sources++;   /* the MHIT of the client's real flash */
    CHECK(host_sources == 1);
    /* not held: an enemy's blow, another object */
    CHECK(!hw_held(enemy, held_h) && !hw_held(0, held_h));
    CHECK(held_rule(enemy, 0, puppet_h, held_h) == 0);
    /* a no-damage DI stays refused even from a held object */
    in.attacker_owner = 0, in.fl0 = (1u << 26) | (1u << MH_FL_NODMG_BIT);
    t = mh_take(&in);
    if (t == MH_TAKE_OTHER && held_rule(in.attacker, 0, moira_c, held_c)) in.attacker_owner = moira_c, t = mh_take(&in);
    CHECK(t == MH_TAKE_INVALID);
}

/* 11. Moira's flash through MHIT */
static void test_flash_mhit(void)
{
    const uint32_t moira_c = 0x2A230060u, light_c = 0x2A238060u, puppet_h = 0x2A1B8060u;
    uint32_t held_c[HW_N] = {light_c, 0, 0, 0, light_c, 0, light_c, 0, 0, 0, 0};
    for (int variant = 0; variant < 2; variant++) {   /* 0: the DI's attacker is Moira, 1: the light object she holds */
        unsigned char di[MH_DI_BUF];
        memset(di, 0, sizeof di);
        di[DI_VALID] = 1;
        mh_wr32(di, DI_DMG, 0);                       /* no damage: the flash only stuns */
        mh_wr32(di, DI_FL, 1u << 26);                 /* the light: the bit uEm1100Intel_vf62 tests through vf334(0x1A) */
        mh_wr32(di, DI_ATT, variant ? light_c : moira_c);
        float epos[3] = {3797.0f, 2031.0f, -16192.0f};
        for (int i = 0; i < 3; i++) memcpy(di + DI_POS + 4 * i, &epos[i], 4);
        /* the client: the replica's skipped update reads the pending DI */
        mh_take_in_t in;
        memset(&in, 0, sizeof in);
        in.valid = 1, in.attacker = mh_rd32(di, DI_ATT), in.mine = moira_c, in.fl0 = mh_rd32(di, DI_FL), in.replica_hp = 500;
        int t = mh_take(&in);
        if (t == MH_TAKE_OTHER && held_rule(in.attacker, 0, moira_c, held_c)) in.attacker_owner = moira_c, t = mh_take(&in);
        CHECK(t == MH_TAKE_MINE);
        if (t != MH_TAKE_MINE) continue;
        mh_rec_t r, r2;
        mh_from_di(di, epos, 0, &r);
        r.la = 101, r.lb = 0, r.lc = 3, r.slot = 1;
        unsigned char b[MH_REC_LEN];
        mh_pack(&r, b);
        CHECK(mh_unpack(b, MH_REC_LEN, &r2));
        CHECK(r2.dmg == 0 && (r2.fl[0] >> 26 & 1));
        /* the host: the rebuilt DI (attacker = the partner's puppet) through the hit handler: FUN_00AD3ED0 ORs DI+0x28..+0x33 into the record +0x38DC.. */
        unsigned char hdi[MH_DI_BUF];
        memset(hdi, 0, sizeof hdi);
        mh_to_di(&r2, epos, puppet_h, hdi);
        CHECK(mh_apply_ok(500));
        uint32_t rec[3] = {0, 0, 0};
        for (unsigned bit = 0; bit < 96; bit++)
            if (mh_rd32(hdi, DI_FL + 4 * (bit >> 5)) >> (bit & 31) & 1) rec[bit >> 5] |= 1u << (bit & 31);
        int lit = (rec[0] >> 26 & 1) && mh_rd32(hdi, DI_ATT) == puppet_h;   /* vf62: the light bit and a player as the last attacker (+0x38D0) */
        CHECK(lit);
        CHECK(mh_rd32(hdi, DI_DMG) == 0);
        CHECK(!(mh_rd32(hdi, DI_FL) >> MH_FL_NODMG_BIT & 1));   /* nothing on the way added the no-damage bit */
    }
}

/* 12. the battery of Moira's light */
static void test_battery(void)
{
    im_inv sent, cur;
    memset(&cur, 0, sizeof cur);
    put(&cur, 24, 0x155, 99), put(&cur, 0, 0x301, 1);
    sent = cur;
    uint32_t t = 10000, last_batt = 0;
    int events = 0, bottle_sent = 0;
    uint8_t idx[IM_N];
    for (int step = 0; step < 40; step++) {   /* 4 s of shining: -2 every 100 ms for 2 s, then +2 every 100 ms */
        t += 100;
        cur.e[24].count += step < 20 ? -2 : 2;
        if (step == 10) im_model_put(&cur, 0x147, 1, 0, STACK);   /* a bottle picked up while she shines */
        int n = im_changed(&sent, &cur, idx);
        if (n && im_send_now(&sent, &cur, idx, n, t, last_batt)) {
            events++;
            if (step == 10) bottle_sent = 1;
            if (im_has_battery(&cur, idx, n)) last_batt = t;
            sent = cur;
        }
    }
    CHECK(events <= 6);   /* 3e55c72: 40 */
    CHECK(bottle_sent);   /* the bottle went out at once */
    for (int step = 0; step < 12; step++) {   /* the light off: the last battery value goes out within a second */
        t += 100;
        int n = im_changed(&sent, &cur, idx);
        if (n && im_send_now(&sent, &cur, idx, n, t, last_batt)) {
            if (im_has_battery(&cur, idx, n)) last_batt = t;
            sent = cur;
        }
    }
    CHECK(im_changed(&sent, &cur, idx) == 0 && sent.e[24].count == cur.e[24].count);
    /* a big step goes out at once */
    cur.e[24].count -= IM_BATT_BIG;
    t += 100;
    int n = im_changed(&sent, &cur, idx);
    CHECK(n == 1 && im_send_now(&sent, &cur, idx, n, t, t - 50));
    /* another item's count is never held */
    sent = cur;
    cur.e[0].count = 2;
    n = im_changed(&sent, &cur, idx);
    CHECK(n == 1 && im_send_now(&sent, &cur, idx, n, t, t - 50));
}

int main(void)
{
    test_bottles();
    test_ammo();
    test_herb();
    test_handover();
    test_checkpoint();
    test_grants();
    test_events();
    test_hand();
    test_throw();
    test_held();
    test_flash_mhit();
    test_battery();
    test_pass_watch();
    printf("test_combat4: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
