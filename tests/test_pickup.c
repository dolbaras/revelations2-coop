/* test_pickup.c: offline model of the PICKUP replay (native/pickup_logic.h, fix032: pair runs g032b / g032d of the guest interaction audit, 2026-11-13).
 * The model: each PC owns a table of placed item objects (LayoutID, id = `+0x19C4`, the running number of the creation order on that PC). The pick-up code calls
 * AddItem(.., object, flag = the object's own id); AddItem's tail calls RemoveItemByFlag(flag): the object whose id equals `flag` is removed. The PICKUP event carries the sender's flag.
 * Built with -DPK_MODEL_BEFORE_G032 (the receiver passes the sender's flag unchanged) the suite must fail: tests/build.sh checks both. */
#include <stdio.h>
#include <string.h>
#include "../native/pickup_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { int a, b, c; int id; int item; int alive; } obj_t;
typedef struct { obj_t o[8]; int n; } pc_t;

static obj_t *find_layout(pc_t *p, int a, int b, int c)
{
    for (int i = 0; i < p->n; i++)
        if (p->o[i].alive && p->o[i].a == a && p->o[i].b == b && p->o[i].c == c) return &p->o[i];
    return NULL;
}
/* sItem::RemoveItemByFlag(flag): the object with that id goes away (what AddItem's tail does with its flag argument) */
static int remove_by_flag(pc_t *p, int flag)
{
    for (int i = 0; i < p->n; i++)
        if (p->o[i].alive && p->o[i].id == flag) { p->o[i].alive = 0; return i; }
    return -1;
}
static int alive_count(const pc_t *p) { int n = 0; for (int i = 0; i < p->n; i++) n += p->o[i].alive; return n; }

/* the guest picks up object (a,b,c) on its PC (AddItem with the object's own id) and the host replays the event through the rule under test; returns the sent flag */
static int pickup_pair(pc_t *guest, pc_t *host, int a, int b, int c)
{
    obj_t *g = find_layout(guest, a, b, c);
    int sent = g ? g->id : 0;
    if (g) remove_by_flag(guest, g->id);                    /* the stock pick-up: AddItem(.., flag = own id) -> RemoveItemByFlag */
    obj_t *h = find_layout(host, a, b, c);                  /* the receiver finds its copy by LayoutID */
    if (h) remove_by_flag(host, pk_replay_flag(sent, 1, h->id));
    return sent;
}

static void t_same_ids(void)
{
    /* a warp / a fresh load: both PCs created the objects in the same order (pair run g032d: herb (151,0,0) id 252 on both) */
    pc_t guest = {{{151, 0, 0, 252, 0x301, 1}, {151, 10, 1, 253, 0x203, 1}}, 2}, host = guest;
    int sent = pickup_pair(&guest, &host, 151, 0, 0);
    CHECK(sent == 252);
    CHECK(alive_count(&guest) == 1 && alive_count(&host) == 1);
    CHECK(!find_layout(&host, 151, 0, 0));                  /* gone on the host too */
    CHECK(find_layout(&host, 151, 10, 1));                  /* the other item stays */
}

static void t_other_ids(void)
{
    /* a natural stage change: the same placed item carries another number on the host (pair run g032b: the guest's flag was 128), and the sender's number belongs to ANOTHER item there */
    pc_t guest = {{{151, 0, 0, 128, 0x301, 1}, {151, 10, 1, 129, 0x203, 1}}, 2};
    pc_t host = {{{151, 0, 0, 228, 0x301, 1}, {151, 10, 1, 128, 0x203, 1}}, 2};
    int sent = pickup_pair(&guest, &host, 151, 0, 0);
    CHECK(sent == 128);
    CHECK(!find_layout(&host, 151, 0, 0));                  /* the item the guest took is gone from the host's floor (no duplicate for the host's human) */
    CHECK(find_layout(&host, 151, 10, 1));                  /* and no OTHER item was removed by the sender's number */
    CHECK(alive_count(&host) == 1);
}

static void t_unmatched_flag(void)
{
    /* the sender's number matches nothing on the host: the old rule removed nothing at all */
    pc_t guest = {{{100, 0, 1, 77, 0x301, 1}}, 1}, host = {{{100, 0, 1, 90, 0x301, 1}}, 1};
    pickup_pair(&guest, &host, 100, 0, 1);
    CHECK(alive_count(&host) == 0);
}

static void t_rules(void)
{
    CHECK(pk_replay_flag(128, 1, 228) == 228);              /* own id wins */
    CHECK(pk_replay_flag(252, 1, 252) == 252);              /* equal ids: unchanged */
    CHECK(pk_replay_flag(0, 1, 228) == 0);                  /* a call without an object id stays without one */
    CHECK(pk_replay_flag(128, 0, 0) == 128);                /* no object found here: nothing to replace it with */
    CHECK(pk_flag_differs(128, 228));
    CHECK(!pk_flag_differs(252, 252));
    CHECK(!pk_flag_differs(0, 228));
}

/* 0.3.2 GRANT hold (pickup_logic.h pk_grant_decide), the knife table scene of r032fs on the guest's PC: the guest's own human is slot 1 (Moira). The host's GRANT of the
 * flashlight 0155 reaches the guest at `grant_t`; the guest's own scene plays until `end_t`; its own story op grants the item WITH the stock equip (hand slot 4) at
 * `own_t` (-1: no own grant), and the h_putitem rule skips that own grant when the item is in the bag already (no equip then). Frames every 16 ms. */
typedef struct { int have, hand, applied_t; } guest_t;
static guest_t sim_grant(int own_slot, int grant_slot, int grant_t, int start_t, int end_t, int own_t, int sim_ms)
{
    guest_t g = {0, 0, -1};
    int held = 0, held_t = 0, last_play = -1;
    for (int now = 0; now <= sim_ms; now += 16) {
        int plays = now >= start_t && now < end_t;
        if (plays) last_play = now;
        unsigned since = plays ? 0u : (last_play >= 0 ? (unsigned)(now - last_play) : 0xFFFFFFFFu);
        if (now <= grant_t && grant_t < now + 16) {   /* the GRANT arrives */
            int d = pk_grant_decide(grant_slot == own_slot, g.have, plays, since, 0);
            if (d == PK_GR_HOLD) held = 1, held_t = now;
            else if (d == PK_GR_APPLY) g.have = 1, g.applied_t = now;   /* story_put: no equip */
        }
        if (own_t >= 0 && now <= own_t && own_t < now + 16 && grant_slot == own_slot) {   /* this PC's own story grant with its equip */
            if (!g.have) g.have = 1, g.hand = 4;   /* else h_putitem skips it: not a second copy, no equip */
        }
        if (held) {
            int d = pk_grant_decide(grant_slot == own_slot, g.have, plays, since, (unsigned)(now - held_t));
            if (d == PK_GR_DROP) held = 0;
            else if (d == PK_GR_APPLY) held = 0, g.have = 1, g.applied_t = now;
        }
    }
    return g;
}
static void t_grant_hold(void)
{
    /* r032fs: the host's GRANT 29 ms before the guest's own scene end, the guest's own grant right after its end */
    guest_t g = sim_grant(1, 1, 8100, 0, 8129, 8150, 12000);
    CHECK(g.have && g.hand == 4);   /* the guest's own grant with its equip: the light in Moira's hand (0.3.2 before the fix: in the bag, hand slot 0) */
    CHECK(g.applied_t < 0);         /* the held GRANT was dropped, not applied */
    /* forced order of the live proof: the guest's scene starts 600 ms later (LAB `net scene trigdelay 600`), the GRANT comes 600 ms before its end */
    g = sim_grant(1, 1, 7500, 600, 8100, 8120, 12000);
    CHECK(g.have && g.hand == 4);
    /* the natural order of most runs: the guest's own grant first, the GRANT finds the item and is skipped */
    g = sim_grant(1, 1, 8200, 0, 8129, 8150, 12000);
    CHECK(g.have && g.hand == 4 && g.applied_t < 0);
    /* no own grant follows (this PC's story did not reach the node): the held GRANT is applied after the scene + 2 s, as before (in the bag) */
    g = sim_grant(1, 1, 8100, 0, 8129, -1, 12000);
    CHECK(g.have && g.applied_t >= 8129 + (int)PK_GRANT_AFTER_MS - 16 && g.applied_t < 8129 + (int)PK_GRANT_AFTER_MS + 32);   /* (frames of 16 ms) */
    /* the GRANT for the partner's copy (slot 0 on the guest's PC: the host's Claire, the knife 014a) is applied at once (0.3.1) */
    g = sim_grant(1, 0, 8100, 0, 8129, -1, 12000);
    CHECK(g.have && g.applied_t > 8100 - 16 && g.applied_t <= 8100);   /* the frame it arrives in */
    /* a GRANT for my own human outside any scene: at once (0.3.1) */
    g = sim_grant(1, 1, 3000, 9000, 9500, -1, 12000);
    CHECK(g.have && g.applied_t > 3000 - 16 && g.applied_t <= 3000);
    /* the rules themselves */
    CHECK(pk_grant_decide(1, 1, 1, 0, 0) == PK_GR_DROP);   /* the item is there already: nothing (both rules) */
    CHECK(pk_grant_decide(1, 0, 1, 0, PK_GRANT_HOLD_MAX_MS) == PK_GR_APPLY);   /* a scene that does not end: applied after the cap */
    CHECK(pk_grant_decide(1, 0, 0, PK_GRANT_AFTER_MS - 1, 100) == PK_GR_HOLD);   /* just after a scene: waits */
    CHECK(pk_grant_decide(1, 0, 0, 0xFFFFFFFFu, 0) == PK_GR_APPLY);   /* no scene since the session start: at once */
}

int main(void)
{
    t_same_ids();
    t_other_ids();
    t_unmatched_flag();
    t_rules();
    t_grant_hold();
#ifdef PK_MODEL_BEFORE_GRANTHOLD
    printf("pickup (model before the GRANT hold): %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
#endif
#ifdef PK_MODEL_BEFORE_G032
    printf("pickup (model before fix032): %d checks, %d failed\n", g_checks, g_fail);
#else
    printf("pickup: %d checks, %d failed\n", g_checks, g_fail);
#endif
    return g_fail ? 1 : 0;
}
